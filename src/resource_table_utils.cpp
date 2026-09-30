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

// Only file built with C++ exceptions enabled (csv-parser throws) -- see SConstruct.
#include <csv.hpp>

using namespace godot;

namespace {

Ref<Script> find_script_for_class(const StringName &p_class_name) {
	const TypedArray<Dictionary> global_classes = ProjectSettings::get_singleton()->get_global_class_list();
	for (int i = 0; i < global_classes.size(); i++) {
		const Dictionary entry = global_classes[i];
		if (StringName(entry["class"]) == p_class_name) {
			return ResourceLoader::get_singleton()->load(entry["path"]);
		}
	}
	return Ref<Script>();
}

bool has_ancestor_in_chain(StringName p_base, const StringName &p_target, const HashMap<StringName, StringName> &p_global_class_bases) {
	for (int i = 0; i < 32 && p_base != StringName(); i++) {
		if (p_base == p_target) {
			return true;
		}
		const HashMap<StringName, StringName>::ConstIterator it = p_global_class_bases.find(p_base);
		if (it == p_global_class_bases.end()) {
			return ClassDB::is_parent_class(p_base, p_target);
		}
		p_base = it->value;
	}
	return false;
}

StringName resource_type_name(const Ref<Resource> &p_resource) {
	const Ref<Script> script = p_resource->get_script();
	if (script.is_valid()) {
		return script->get_global_name();
	}
	return StringName(p_resource->get_class());
}

void collect_resources_of_class(const String &p_dir, const StringName &p_class_name, const HashMap<StringName, StringName> &p_global_class_bases, Array &r_results) {
	const Ref<DirAccess> dir = DirAccess::open(p_dir);
	if (dir.is_null()) {
		return;
	}

	dir->list_dir_begin();
	for (String entry = dir->get_next(); !entry.is_empty(); entry = dir->get_next()) {
		const String full_path = p_dir.path_join(entry);
		if (dir->current_is_dir()) {
			if (!entry.begins_with(".")) {
				collect_resources_of_class(full_path, p_class_name, p_global_class_bases, r_results);
			}
			continue;
		}

		const String ext = entry.get_extension().to_lower();
		if (ext != "tres" && ext != "res") {
			continue;
		}

		const Ref<Resource> resource = ResourceLoader::get_singleton()->load(full_path);
		if (resource.is_null()) {
			continue;
		}

		if (has_ancestor_in_chain(resource_type_name(resource), p_class_name, p_global_class_bases)) {
			r_results.push_back(resource);
		}
	}
	dir->list_dir_end();
}

HashMap<StringName, StringName> calc_global_class_bases() {
	HashMap<StringName, StringName> bases;
	const TypedArray<Dictionary> global_classes = ProjectSettings::get_singleton()->get_global_class_list();
	for (int i = 0; i < global_classes.size(); i++) {
		const Dictionary entry = global_classes[i];
		bases[StringName(entry["class"])] = StringName(entry["base"]);
	}
	return bases;
}

bool is_hidden_in_table_view(const Ref<Script> &p_script) {
	if (p_script.is_null()) {
		return false;
	}
	const Variant is_shown = p_script->get("_show_in_resource_table_view");
	return is_shown.get_type() == Variant::BOOL && !static_cast<bool>(is_shown);
}

void collect_resource_type_names_with_instances(const String &p_dir, HashSet<String> &r_results) {
	const Ref<DirAccess> dir = DirAccess::open(p_dir);
	if (dir.is_null()) {
		return;
	}

	dir->list_dir_begin();
	for (String entry = dir->get_next(); !entry.is_empty(); entry = dir->get_next()) {
		const String full_path = p_dir.path_join(entry);
		if (dir->current_is_dir()) {
			if (!entry.begins_with(".")) {
				collect_resource_type_names_with_instances(full_path, r_results);
			}
			continue;
		}

		const String ext = entry.get_extension().to_lower();
		if (ext != "tres" && ext != "res") {
			continue;
		}

		const Ref<Resource> resource = ResourceLoader::get_singleton()->load(full_path);
		if (resource.is_null() || is_hidden_in_table_view(resource->get_script())) {
			continue;
		}

		// An anonymous inline script (no `class_name`) resolves to an empty
		// type name -- must not show up as a blank dropdown entry.
		const StringName type_name = resource_type_name(resource);
		if (type_name == StringName()) {
			continue;
		}

		r_results.insert(String(type_name));
	}
	dir->list_dir_end();
}

bool is_editable_property(const Dictionary &p_property_info) {
	const int64_t usage = p_property_info["usage"];
	return (usage & PROPERTY_USAGE_EDITOR) != 0;
}

struct ResourceClassInfo {
	Array properties;
	bool is_valid = false;
};

ResourceClassInfo resolve_resource_class_by_name(const StringName &p_class_name) {
	ResourceClassInfo info;
	if (p_class_name == StringName()) {
		return info;
	}

	Array all_properties;
	const Ref<Script> script = find_script_for_class(p_class_name);
	if (script.is_valid()) {
		all_properties = script->get_script_property_list();
	} else if (ClassDB::class_exists(p_class_name)) {
		all_properties = ClassDB::class_get_property_list(p_class_name, /*p_no_inheritance=*/true);
	} else {
		return info;
	}

	for (int i = 0; i < all_properties.size(); i++) {
		if (is_editable_property(all_properties[i])) {
			info.properties.push_back(all_properties[i]);
		}
	}
	info.is_valid = true;
	return info;
}

bool is_resource_before_by_name(const Variant &p_a, const Variant &p_b) {
	const Ref<Resource> a = p_a;
	const Ref<Resource> b = p_b;
	if (a.is_null() || b.is_null()) {
		return false;
	}
	return a->get_path().get_file().get_basename().naturalnocasecmp_to(b->get_path().get_file().get_basename()) < 0;
}

bool is_script_before_by_class_name(const Variant &p_a, const Variant &p_b) {
	const Ref<Script> a = p_a;
	const Ref<Script> b = p_b;
	if (a.is_null() || b.is_null()) {
		return false;
	}
	return String(a->get_global_name()).naturalnocasecmp_to(b->get_global_name()) < 0;
}

// Bare native-base object with the script attached: Script::call("new") only works for @tool scripts in the editor.
Ref<Resource> instantiate_typed_resource(const StringName &p_class_name) {
	const Ref<Script> resource_script = find_script_for_class(p_class_name);
	if (resource_script.is_valid()) {
		const Ref<Resource> new_resource = Ref<Resource>(ClassDB::instantiate(resource_script->get_instance_base_type()));
		if (new_resource.is_null()) {
			return new_resource;
		}
		new_resource->set_script(resource_script);
		return new_resource;
	}
	return Ref<Resource>(ClassDB::instantiate(p_class_name));
}

Variant parse_csv_value(const String &p_text, Variant::Type p_type) {
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

ResourceTableUtils::ImportPreview run_csv_import(const StringName &p_resource_class_name, const PackedStringArray &p_resource_paths, const String &p_path, bool p_execute) {
	ResourceTableUtils::ImportPreview result;

	const ResourceClassInfo class_info = resolve_resource_class_by_name(p_resource_class_name);
	if (!class_info.is_valid) {
		return result;
	}
	const Array properties = class_info.properties;

	HashMap<StringName, Variant::Type> property_types;
	for (int i = 0; i < properties.size(); i++) {
		const Dictionary info = properties[i];
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

	HashSet<String> seen_paths;
	int adds = 0;
	int updates = 0;

	try {
		for (const csv::CSVRow &row : *reader) {
			if (row.size() == 0) {
				continue;
			}

			const String path = String::utf8(row[0].get<std::string>().c_str());
			if (path.is_empty()) {
				continue;
			}
			if (seen_paths.has(path)) {
				continue;
			}
			seen_paths.insert(path);

			const bool is_existing = ResourceLoader::get_singleton()->exists(path);
			if (is_existing) {
				updates++;
			} else {
				adds++;
			}

			if (!p_execute) {
				continue;
			}

			Ref<Resource> resource;
			if (is_existing) {
				resource = ResourceLoader::get_singleton()->load(path);
			}
			if (resource.is_null()) {
				resource = instantiate_typed_resource(p_resource_class_name);
			}
			if (resource.is_null()) {
				continue;
			}

			for (size_t col = 1; col < header.size() && col < row.size(); col++) {
				const StringName property_name = String::utf8(header[col].c_str());
				const HashMap<StringName, Variant::Type>::ConstIterator type_it = property_types.find(property_name);
				if (type_it == property_types.end()) {
					continue;
				}
				const String cell = String::utf8(row[col].get<std::string>().c_str());
				resource->set(property_name, parse_csv_value(cell, type_it->value));
			}

			const Error save_err = ResourceSaver::get_singleton()->save(resource, path);
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
		const String existing_path = p_resource_paths[i];
		if (existing_path.is_empty() || seen_paths.has(existing_path)) {
			continue;
		}
		deletes++;
		if (p_execute) {
			DirAccess::remove_absolute(existing_path);
		}
	}

	if (p_execute) {
		const EditorInterface *const editor_interface = EditorInterface::get_singleton();
		if (editor_interface) {
			EditorFileSystem *const filesystem = editor_interface->get_resource_filesystem();
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
	ClassDB::bind_static_method("ResourceTableUtils", D_METHOD("find_generator_scripts"), &ResourceTableUtils::find_generator_scripts);
	ClassDB::bind_static_method("ResourceTableUtils", D_METHOD("find_or_create_resource", "class_name", "path"), &ResourceTableUtils::find_or_create_resource);
	ClassDB::bind_static_method("ResourceTableUtils", D_METHOD("export_csv", "resource_class_name", "resource_paths", "path"), &ResourceTableUtils::export_csv);
	ClassDB::bind_static_method("ResourceTableUtils", D_METHOD("import_csv", "resource_class_name", "resource_paths", "path"), &ResourceTableUtils::import_csv);
}

Array ResourceTableUtils::find_resources_of_type(const StringName &p_class_name) {
	Array results;
	collect_resources_of_class("res://", p_class_name, calc_global_class_bases(), results);
	return results;
}

PackedStringArray ResourceTableUtils::find_resource_type_names() {
	HashSet<String> names;
	const HashMap<StringName, StringName> global_class_bases = calc_global_class_bases();
	collect_resource_type_names_with_instances("res://", names);

	for (const KeyValue<StringName, StringName> &entry : global_class_bases) {
		if (has_ancestor_in_chain(entry.value, "Resource", global_class_bases) && !is_hidden_in_table_view(find_script_for_class(entry.key))) {
			names.insert(String(entry.key));
		}
	}

	const ClassDBSingleton *const class_db = ClassDBSingleton::get_singleton();
	const PackedStringArray native_resources = class_db->get_inheriters_from_class("Resource");
	for (const String &native_name : native_resources) {
		const ClassDBSingleton::APIType api_type = class_db->class_get_api_type(native_name);
		const bool is_extension_class = api_type == ClassDBSingleton::API_EXTENSION || api_type == ClassDBSingleton::API_EDITOR_EXTENSION;
		if (is_extension_class && class_db->can_instantiate(native_name)) {
			names.insert(native_name);
		}
	}

	const Array generator_scripts = find_generator_scripts();
	for (int i = 0; i < generator_scripts.size(); i++) {
		const Ref<Script> script = generator_scripts[i];
		if (script.is_null() || !script->can_instantiate()) {
			continue;
		}
		const Ref<RefCounted> generator = script->call("new");
		if (generator.is_null()) {
			continue;
		}
		const StringName generator_class_name = generator->get("_resource_class_name");
		if (global_class_bases.has(generator_class_name) || class_db->class_exists(generator_class_name)) {
			names.insert(String(generator_class_name));
		}
	}

	PackedStringArray sorted_names;
	for (const String &name : names) {
		sorted_names.push_back(name);
	}
	sorted_names.sort();
	return sorted_names;
}

Array ResourceTableUtils::find_generator_scripts() {
	const TypedArray<Dictionary> global_classes = ProjectSettings::get_singleton()->get_global_class_list();

	HashMap<StringName, StringName> global_class_bases;
	for (int i = 0; i < global_classes.size(); i++) {
		const Dictionary entry = global_classes[i];
		global_class_bases[StringName(entry["class"])] = StringName(entry["base"]);
	}

	Array scripts;
	HashSet<String> seen_names;
	for (int i = 0; i < global_classes.size(); i++) {
		const Dictionary entry = global_classes[i];
		const StringName class_name = entry["class"];
		if (seen_names.has(class_name) || !has_ancestor_in_chain(entry["base"], "ResourceTableGenerator", global_class_bases)) {
			continue;
		}
		const Ref<Script> script = ResourceLoader::get_singleton()->load(entry["path"]);
		if (script.is_valid()) {
			seen_names.insert(class_name);
			scripts.push_back(script);
		}
	}

	scripts.sort_custom(callable_mp_static(&is_script_before_by_class_name));
	return scripts;
}

Array ResourceTableUtils::find_properties_of_type(const StringName &p_class_name) {
	return resolve_resource_class_by_name(p_class_name).properties;
}

Ref<Resource> ResourceTableUtils::find_or_create_resource(const StringName &p_class_name, const String &p_path) {
	ResourceLoader *const loader = ResourceLoader::get_singleton();
	if (loader->exists(p_path)) {
		const Ref<Resource> existing = loader->load(p_path);
		if (existing.is_valid()) {
			return existing;
		}
	}

	const Ref<Resource> instance = instantiate_typed_resource(p_class_name);
	if (instance.is_null()) {
		return instance;
	}

	// Parent directory needs to exist now, since the caller's own later save
	// (e.g. ResourceSaver.save(instance), no path needed there) won't create it.
	const String dir = p_path.get_base_dir();
	if (!dir.is_empty() && !DirAccess::dir_exists_absolute(dir)) {
		DirAccess::make_dir_recursive_absolute(dir);
	}
	instance->set_path(p_path);
	return instance;
}

Ref<Resource> ResourceTableUtils::instantiate_resource_of_type(const StringName &p_class_name) {
	return instantiate_typed_resource(p_class_name);
}

Error ResourceTableUtils::export_csv(const StringName &p_resource_class_name, const PackedStringArray &p_resource_paths, const String &p_path) {
	const ResourceClassInfo class_info = resolve_resource_class_by_name(p_resource_class_name);
	if (!class_info.is_valid) {
		return ERR_DOES_NOT_EXIST;
	}
	const Array properties = class_info.properties;

	Array resources;
	for (int i = 0; i < p_resource_paths.size(); i++) {
		const Ref<Resource> resource = ResourceLoader::get_singleton()->load(p_resource_paths[i]);
		if (resource.is_valid()) {
			resources.push_back(resource);
		}
	}
	resources.sort_custom(callable_mp_static(&is_resource_before_by_name));

	std::ofstream out(p_path.utf8().get_data(), std::ios::out | std::ios::binary | std::ios::trunc);
	if (!out.is_open()) {
		return ERR_FILE_CANT_OPEN;
	}
	auto writer = csv::make_csv_writer(out);

	std::vector<std::string> header;
	header.push_back("Path");
	for (int i = 0; i < properties.size(); i++) {
		const Dictionary info = properties[i];
		header.push_back(std::string(String(info["name"]).utf8().get_data()));
	}
	writer << header;

	for (int i = 0; i < resources.size(); i++) {
		const Ref<Resource> resource = resources[i];
		if (resource.is_null()) {
			continue;
		}
		std::vector<std::string> fields;
		fields.push_back(std::string(resource->get_path().utf8().get_data()));
		for (int j = 0; j < properties.size(); j++) {
			const Dictionary info = properties[j];
			const StringName property_name = info["name"];
			fields.push_back(std::string(resource->get(property_name).stringify().utf8().get_data()));
		}
		writer << fields;
	}

	return OK;
}

Error ResourceTableUtils::import_csv(const StringName &p_resource_class_name, const PackedStringArray &p_resource_paths, const String &p_path) {
	return run_csv_import(p_resource_class_name, p_resource_paths, p_path, /*p_execute=*/true).error;
}

ResourceTableUtils::ImportPreview ResourceTableUtils::preview_import_csv(const StringName &p_resource_class_name, const PackedStringArray &p_resource_paths, const String &p_path) {
	return run_csv_import(p_resource_class_name, p_resource_paths, p_path, /*p_execute=*/false);
}
