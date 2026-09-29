#include "resource_table_utils.h"

#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/resource_saver.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/templates/hash_set.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <fstream>
#include <memory>
#include <string>
#include <vector>

// CSV import/export is backed by the vendored csv-parser submodule
// (csv-parser/, see its own AGENTS.md), not Godot's own
// FileAccess::get_csv_line()/store_csv_line() -- p_path is always a real OS
// path (the panel's CSV dialog uses EditorFileDialog::ACCESS_FILESYSTEM),
// so std::ifstream/std::ofstream work directly. This is the one file in
// src/ built with C++ exceptions enabled (csv-parser throws on a malformed
// row or a file it can't open) -- see SConstruct.
#include <csv.hpp>

using namespace godot;

namespace {

Ref<Script> _find_script_for_class(const StringName &p_class_name) {
	TypedArray<Dictionary> global_classes = ProjectSettings::get_singleton()->get_global_class_list();
	for (int i = 0; i < global_classes.size(); i++) {
		Dictionary entry = global_classes[i];
		if (StringName(entry["class"]) == p_class_name) {
			return ResourceLoader::get_singleton()->load(entry["path"]);
		}
	}
	return Ref<Script>();
}

// Resolves p_base's chain -- possibly through other project global classes
// -- down to a real engine class, then checks whether it passes p_target.
bool _base_chain_inherits(StringName p_base, const StringName &p_target, const HashMap<StringName, StringName> &p_global_class_bases) {
	for (int i = 0; i < 32 && p_base != StringName(); i++) {
		if (p_base == p_target) {
			return true;
		}
		HashMap<StringName, StringName>::ConstIterator it = p_global_class_bases.find(p_base);
		if (it == p_global_class_bases.end()) {
			return ClassDB::is_parent_class(p_base, p_target);
		}
		p_base = it->value;
	}
	return false;
}

// Mirrors how a type name is resolved in _resolve_resource_class_by_name,
// so the two agree on what counts as "an instance of that type".
StringName _resource_type_name(const Ref<Resource> &p_resource) {
	Ref<Script> script = p_resource->get_script();
	if (script.is_valid()) {
		return script->get_global_name();
	}
	return StringName(p_resource->get_class());
}

void _collect_resources_of_class(const String &p_dir, const StringName &p_class_name, Array &r_results) {
	Ref<DirAccess> dir = DirAccess::open(p_dir);
	if (dir.is_null()) {
		return;
	}

	dir->list_dir_begin();
	for (String entry = dir->get_next(); !entry.is_empty(); entry = dir->get_next()) {
		String full_path = p_dir.path_join(entry);
		if (dir->current_is_dir()) {
			if (!entry.begins_with(".")) {
				_collect_resources_of_class(full_path, p_class_name, r_results);
			}
			continue;
		}

		String ext = entry.get_extension().to_lower();
		if (ext != "tres" && ext != "res") {
			continue;
		}

		Ref<Resource> resource = ResourceLoader::get_singleton()->load(full_path);
		if (resource.is_null()) {
			continue;
		}

		if (_resource_type_name(resource) == p_class_name) {
			r_results.push_back(resource);
		}
	}
	dir->list_dir_end();
}

HashMap<StringName, StringName> _global_class_bases() {
	HashMap<StringName, StringName> bases;
	TypedArray<Dictionary> global_classes = ProjectSettings::get_singleton()->get_global_class_list();
	for (int i = 0; i < global_classes.size(); i++) {
		Dictionary entry = global_classes[i];
		bases[StringName(entry["class"])] = StringName(entry["base"]);
	}
	return bases;
}

// Object::is_class() only sees the native class chain, so a script-derived
// ResourceTable (e.g. GenericResourceTable) has to be resolved through the
// global class list instead.
bool _is_resource_table(const Ref<Resource> &p_resource, const HashMap<StringName, StringName> &p_global_class_bases) {
	Ref<Script> script = p_resource->get_script();
	if (script.is_valid()) {
		return _base_chain_inherits(script->get_global_name(), "ResourceTable", p_global_class_bases);
	}
	return p_resource->is_class("ResourceTable");
}

void _collect_resource_type_names_with_instances(const String &p_dir, const HashMap<StringName, StringName> &p_global_class_bases, HashSet<String> &r_results) {
	Ref<DirAccess> dir = DirAccess::open(p_dir);
	if (dir.is_null()) {
		return;
	}

	dir->list_dir_begin();
	for (String entry = dir->get_next(); !entry.is_empty(); entry = dir->get_next()) {
		String full_path = p_dir.path_join(entry);
		if (dir->current_is_dir()) {
			if (!entry.begins_with(".")) {
				_collect_resource_type_names_with_instances(full_path, p_global_class_bases, r_results);
			}
			continue;
		}

		String ext = entry.get_extension().to_lower();
		if (ext != "tres" && ext != "res") {
			continue;
		}

		Ref<Resource> resource = ResourceLoader::get_singleton()->load(full_path);
		if (resource.is_null() || _is_resource_table(resource, p_global_class_bases)) {
			continue;
		}

		// An anonymous inline script (no `class_name`) resolves to an empty
		// type name -- must not show up as a blank dropdown entry.
		StringName type_name = _resource_type_name(resource);
		if (type_name == StringName()) {
			continue;
		}

		r_results.insert(String(type_name));
	}
	dir->list_dir_end();
}

bool _is_editable_property(const Dictionary &p_property_info) {
	int64_t usage = p_property_info["usage"];
	return (usage & PROPERTY_USAGE_EDITOR) != 0;
}

struct ResourceClassInfo {
	Array properties;
	bool valid = false;
};

ResourceClassInfo _resolve_resource_class_by_name(const StringName &p_class_name) {
	ResourceClassInfo info;
	if (p_class_name == StringName()) {
		return info;
	}

	Array all_properties;
	Ref<Script> script = _find_script_for_class(p_class_name);
	if (script.is_valid()) {
		all_properties = script->get_script_property_list();
	} else if (ClassDB::class_exists(p_class_name)) {
		all_properties = ClassDB::class_get_property_list(p_class_name, /*p_no_inheritance=*/true);
	} else {
		return info; // Neither a project global class nor a known native type.
	}

	for (int i = 0; i < all_properties.size(); i++) {
		if (_is_editable_property(all_properties[i])) {
			info.properties.push_back(all_properties[i]);
		}
	}
	info.valid = true;
	return info;
}

// Matches the panel's own default (Name-column) sort order.
bool _compare_resources_by_name(const Variant &p_a, const Variant &p_b) {
	Ref<Resource> a = p_a;
	Ref<Resource> b = p_b;
	if (a.is_null() || b.is_null()) {
		return false;
	}
	return a->get_path().get_file().get_basename().naturalnocasecmp_to(b->get_path().get_file().get_basename()) < 0;
}

bool _compare_scripts_by_class_name(const Variant &p_a, const Variant &p_b) {
	Ref<Script> a = p_a;
	Ref<Script> b = p_b;
	if (a.is_null() || b.is_null()) {
		return false;
	}
	return String(a->get_global_name()).naturalnocasecmp_to(b->get_global_name()) < 0;
}

// A project global class gets a bare Resource with its script attached
// rather than Script::call("new") -- Godot only allows reflective
// instantiation that way for @tool scripts while the editor is running, and
// a plain data Resource type normally isn't one.
Ref<Resource> _instantiate_resource_of_type(const StringName &p_class_name) {
	Ref<Script> resource_script = _find_script_for_class(p_class_name);
	if (resource_script.is_valid()) {
		Ref<Resource> new_resource;
		new_resource.instantiate();
		new_resource->set_script(resource_script);
		return new_resource;
	}
	return Ref<Resource>(ClassDB::instantiate(p_class_name));
}

Variant _parse_csv_value(const String &p_text, Variant::Type p_type) {
	switch (p_type) {
		case Variant::BOOL:
			return p_text.strip_edges().to_lower() == "true";
		case Variant::INT:
			return (int64_t)p_text.to_int();
		case Variant::FLOAT:
			return p_text.to_float();
		default:
			return p_text;
	}
}

// Shared by import_csv/preview_import_csv; only actually creates/edits/
// saves/deletes anything when p_execute is true.
ResourceTableUtils::ImportPreview _import_csv(const StringName &p_resource_class_name, const PackedStringArray &p_resource_paths, const String &p_path, bool p_execute) {
	ResourceTableUtils::ImportPreview result;

	ResourceClassInfo class_info = _resolve_resource_class_by_name(p_resource_class_name);
	if (!class_info.valid) {
		return result;
	}
	Array properties = class_info.properties;

	HashMap<StringName, Variant::Type> property_types;
	for (int i = 0; i < properties.size(); i++) {
		Dictionary info = properties[i];
		property_types[StringName(String(info["name"]))] = (Variant::Type)(int64_t)info["type"];
	}

	csv::CSVFormat format;
	format.delimiter(',').header_row(0).variable_columns(csv::VariableColumnPolicy::KEEP);

	std::unique_ptr<csv::CSVReader> reader;
	try {
		reader = std::make_unique<csv::CSVReader>(std::string(p_path.utf8().get_data()), format);
	} catch (const std::exception &e) {
		UtilityFunctions::push_error("ResourceTableUtils: failed to open '", p_path, "' (", e.what(), ").");
		result.error = ERR_FILE_CANT_OPEN;
		return result;
	}
	const std::vector<std::string> &header = reader->get_col_names();

	// Every path the CSV mentions, so any existing resource of this type NOT
	// mentioned can be deleted afterward -- the CSV is the source of truth.
	HashSet<String> seen_paths;
	int adds = 0;
	int updates = 0;

	try {
		for (csv::CSVRow &row : *reader) {
			if (row.size() == 0) {
				continue;
			}

			String path = String::utf8(row[0].get<std::string>().c_str());
			if (path.is_empty()) {
				continue;
			}
			if (seen_paths.has(path)) {
				continue; // Duplicate row for the same path -- already counted/applied.
			}
			seen_paths.insert(path);

			bool exists = ResourceLoader::get_singleton()->exists(path);
			if (exists) {
				updates++;
			} else {
				adds++;
			}

			if (!p_execute) {
				continue;
			}

			Ref<Resource> resource;
			if (exists) {
				resource = ResourceLoader::get_singleton()->load(path);
			}
			if (resource.is_null()) {
				resource = _instantiate_resource_of_type(p_resource_class_name);
			}
			if (resource.is_null()) {
				continue;
			}

			for (size_t col = 1; col < header.size() && col < row.size(); col++) {
				StringName property_name = String::utf8(header[col].c_str());
				HashMap<StringName, Variant::Type>::ConstIterator type_it = property_types.find(property_name);
				if (type_it == property_types.end()) {
					continue;
				}
				String cell = String::utf8(row[col].get<std::string>().c_str());
				resource->set(property_name, _parse_csv_value(cell, type_it->value));
			}

			Error save_err = ResourceSaver::get_singleton()->save(resource, path);
			if (save_err != OK) {
				UtilityFunctions::push_error("ResourceTableUtils: failed to save '", path, "' (error ", (int64_t)save_err, ") -- check that its parent directory exists.");
			}
		}
	} catch (const std::exception &e) {
		UtilityFunctions::push_error("ResourceTableUtils: failed to read '", p_path, "' (", e.what(), ").");
		result.error = FAILED;
		return result;
	}

	int deletes = 0;
	for (int i = 0; i < p_resource_paths.size(); i++) {
		String existing_path = p_resource_paths[i];
		if (existing_path.is_empty() || seen_paths.has(existing_path)) {
			continue;
		}
		deletes++;
		if (p_execute) {
			DirAccess::remove_absolute(existing_path);
		}
	}

	if (p_execute) {
		EditorInterface *editor_interface = EditorInterface::get_singleton();
		if (editor_interface) {
			EditorFileSystem *filesystem = editor_interface->get_resource_filesystem();
			if (filesystem) {
				filesystem->scan();
			}
		}
	}

	result.error = OK;
	result.adds = adds;
	result.updates = updates;
	result.deletes = deletes;
	return result;
}

} // namespace

void ResourceTableUtils::_bind_methods() {
	ClassDB::bind_static_method("ResourceTableUtils", D_METHOD("find_resources_of_type", "class_name"), &ResourceTableUtils::find_resources_of_type);
	ClassDB::bind_static_method("ResourceTableUtils", D_METHOD("find_resource_type_names_with_instances"), &ResourceTableUtils::find_resource_type_names_with_instances);
	ClassDB::bind_static_method("ResourceTableUtils", D_METHOD("find_generator_scripts"), &ResourceTableUtils::find_generator_scripts);
	ClassDB::bind_static_method("ResourceTableUtils", D_METHOD("find_safe_name", "resource_class_name", "prefix"), &ResourceTableUtils::find_safe_name);
	ClassDB::bind_static_method("ResourceTableUtils", D_METHOD("create_instance", "class_name", "path"), &ResourceTableUtils::create_instance);
	ClassDB::bind_static_method("ResourceTableUtils", D_METHOD("find_properties_of_type", "class_name"), &ResourceTableUtils::find_properties_of_type);
	ClassDB::bind_static_method("ResourceTableUtils", D_METHOD("find_or_create", "class_name", "path"), &ResourceTableUtils::find_or_create);
	ClassDB::bind_static_method("ResourceTableUtils", D_METHOD("instantiate_resource_of_type", "class_name"), &ResourceTableUtils::instantiate_resource_of_type);
	ClassDB::bind_static_method("ResourceTableUtils", D_METHOD("export_csv", "resource_class_name", "resource_paths", "path"), &ResourceTableUtils::export_csv);
	ClassDB::bind_static_method("ResourceTableUtils", D_METHOD("import_csv", "resource_class_name", "resource_paths", "path"), &ResourceTableUtils::import_csv);
}

Array ResourceTableUtils::find_resources_of_type(const StringName &p_class_name) {
	Array results;
	_collect_resources_of_class("res://", p_class_name, results);
	return results;
}

PackedStringArray ResourceTableUtils::find_resource_type_names_with_instances() {
	HashSet<String> names;
	_collect_resource_type_names_with_instances("res://", _global_class_bases(), names);

	PackedStringArray sorted_names;
	for (const String &name : names) {
		sorted_names.push_back(name);
	}
	sorted_names.sort();
	return sorted_names;
}

Array ResourceTableUtils::find_generator_scripts() {
	TypedArray<Dictionary> global_classes = ProjectSettings::get_singleton()->get_global_class_list();

	HashMap<StringName, StringName> global_class_bases;
	for (int i = 0; i < global_classes.size(); i++) {
		Dictionary entry = global_classes[i];
		global_class_bases[StringName(entry["class"])] = StringName(entry["base"]);
	}

	Array scripts;
	HashSet<String> seen_names;
	for (int i = 0; i < global_classes.size(); i++) {
		Dictionary entry = global_classes[i];
		StringName class_name = entry["class"];
		if (seen_names.has(class_name) || !_base_chain_inherits(entry["base"], "ResourceTableGenerator", global_class_bases)) {
			continue;
		}
		Ref<Script> script = ResourceLoader::get_singleton()->load(entry["path"]);
		if (script.is_valid()) {
			seen_names.insert(class_name);
			scripts.push_back(script);
		}
	}

	scripts.sort_custom(callable_mp_static(&_compare_scripts_by_class_name));
	return scripts;
}

String ResourceTableUtils::find_safe_name(const StringName &p_resource_class_name, const String &p_prefix) {
	Array existing;
	_collect_resources_of_class("res://", p_resource_class_name, existing);

	String match_prefix = p_prefix + String("_");
	HashSet<int> used_indices;
	for (int i = 0; i < existing.size(); i++) {
		Ref<Resource> resource = existing[i];
		if (resource.is_null()) {
			continue;
		}
		String basename = resource->get_path().get_file().get_basename();
		if (!basename.begins_with(match_prefix)) {
			continue;
		}
		String suffix = basename.substr(match_prefix.length());
		if (suffix.is_valid_int()) {
			used_indices.insert(suffix.to_int());
		}
	}

	int index = 0;
	while (used_indices.has(index)) {
		index++;
	}
	return match_prefix + String::num_int64(index);
}

Ref<Resource> ResourceTableUtils::create_instance(const StringName &p_class_name, const String &p_path) {
	Ref<Resource> instance = _instantiate_resource_of_type(p_class_name);
	if (instance.is_null()) {
		return instance;
	}

	String dir = p_path.get_base_dir();
	if (!dir.is_empty() && !DirAccess::dir_exists_absolute(dir)) {
		DirAccess::make_dir_recursive_absolute(dir);
	}

	Error save_err = ResourceSaver::get_singleton()->save(instance, p_path);
	if (save_err != OK) {
		UtilityFunctions::push_error("ResourceTableUtils: failed to save '", p_path, "' (error ", (int64_t)save_err, ").");
		return Ref<Resource>();
	}
	return instance;
}

Array ResourceTableUtils::find_properties_of_type(const StringName &p_class_name) {
	return _resolve_resource_class_by_name(p_class_name).properties;
}

Ref<Resource> ResourceTableUtils::find_or_create(const StringName &p_class_name, const String &p_path) {
	if (ResourceLoader::get_singleton()->exists(p_path)) {
		Ref<Resource> existing = ResourceLoader::get_singleton()->load(p_path);
		if (existing.is_valid()) {
			return existing;
		}
	}

	Ref<Resource> instance = _instantiate_resource_of_type(p_class_name);
	if (instance.is_null()) {
		return instance;
	}

	// Parent directory needs to exist now, since the caller's own later save
	// (e.g. ResourceSaver.save(instance), no path needed there) won't create it.
	String dir = p_path.get_base_dir();
	if (!dir.is_empty() && !DirAccess::dir_exists_absolute(dir)) {
		DirAccess::make_dir_recursive_absolute(dir);
	}
	instance->set_path(p_path);
	return instance;
}

Ref<Resource> ResourceTableUtils::instantiate_resource_of_type(const StringName &p_class_name) {
	return _instantiate_resource_of_type(p_class_name);
}

Error ResourceTableUtils::export_csv(const StringName &p_resource_class_name, const PackedStringArray &p_resource_paths, const String &p_path) {
	ResourceClassInfo class_info = _resolve_resource_class_by_name(p_resource_class_name);
	if (!class_info.valid) {
		return ERR_DOES_NOT_EXIST;
	}
	Array properties = class_info.properties;

	Array resources;
	for (int i = 0; i < p_resource_paths.size(); i++) {
		Ref<Resource> resource = ResourceLoader::get_singleton()->load(p_resource_paths[i]);
		if (resource.is_valid()) {
			resources.push_back(resource);
		}
	}
	resources.sort_custom(callable_mp_static(&_compare_resources_by_name));

	std::ofstream out(p_path.utf8().get_data(), std::ios::out | std::ios::binary | std::ios::trunc);
	if (!out.is_open()) {
		return ERR_FILE_CANT_OPEN;
	}
	auto writer = csv::make_csv_writer(out);

	std::vector<std::string> header;
	header.push_back("Path");
	for (int i = 0; i < properties.size(); i++) {
		Dictionary info = properties[i];
		header.push_back(std::string(String(info["name"]).utf8().get_data()));
	}
	writer << header;

	for (int i = 0; i < resources.size(); i++) {
		Ref<Resource> resource = resources[i];
		if (resource.is_null()) {
			continue;
		}
		std::vector<std::string> fields;
		fields.push_back(std::string(resource->get_path().utf8().get_data()));
		for (int j = 0; j < properties.size(); j++) {
			Dictionary info = properties[j];
			StringName property_name = info["name"];
			fields.push_back(std::string(resource->get(property_name).stringify().utf8().get_data()));
		}
		writer << fields;
	}

	return OK;
}

Error ResourceTableUtils::import_csv(const StringName &p_resource_class_name, const PackedStringArray &p_resource_paths, const String &p_path) {
	return _import_csv(p_resource_class_name, p_resource_paths, p_path, /*p_execute=*/true).error;
}

ResourceTableUtils::ImportPreview ResourceTableUtils::preview_import_csv(const StringName &p_resource_class_name, const PackedStringArray &p_resource_paths, const String &p_path) {
	return _import_csv(p_resource_class_name, p_resource_paths, p_path, /*p_execute=*/false);
}
