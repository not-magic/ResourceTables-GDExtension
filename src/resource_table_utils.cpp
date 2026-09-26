#include "resource_table_utils.h"

#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/resource_saver.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/templates/hash_set.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

namespace {

const char *GENERATED_TABLES_DIR = "res://generated_resource_tables";

// Resolves p_base's chain -- possibly through other project global classes
// -- down to a real engine/GDExtension class, then checks whether that
// chain passes through p_target.
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

// The script backing a project global class, or null if p_class_name isn't
// one (e.g. it's a builtin/engine class).
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

// The identifying name of an existing resource's concrete type: its
// script's global class name if it has one (a project global class), or its
// own native engine class name otherwise. Mirrors how a table's ITEMS
// element type is resolved to a name in _resolve_resource_class_by_name, so
// the two agree on what counts as "an instance of that type".
StringName _resource_type_name(const Ref<Resource> &p_resource) {
	Ref<Script> script = p_resource->get_script();
	if (script.is_valid()) {
		return script->get_global_name();
	}
	return StringName(p_resource->get_class());
}

// Recursively finds every .tres/.res file under p_dir whose concrete type
// (see _resource_type_name) is exactly p_class_name (not a subclass -- a
// table shows one flat list per concrete resource type).
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

// Whether a property (as returned by Script::get_script_property_list or
// ClassDB::class_get_property_list) is one the user actually exported,
// rather than a plain internal `var` (or, for a native class, an
// implementation-detail property with no editor usage flag).
bool _is_editable_property(const Dictionary &p_property_info) {
	int64_t usage = p_property_info["usage"];
	return (usage & PROPERTY_USAGE_EDITOR) != 0;
}

// GDScript compiles `@export var ITEMS: Array[Type]` to a TYPE_ARRAY
// property whose hint_string is "<elem_type>/<elem_hint>:<elem_hint_string>"
// (e.g. "24/17:AAA" for Array[AAA], both for a project global class and a
// native/engine Type) -- elem_hint_string is Type's class name, and is all
// this needs.
StringName _array_element_class_name(const Dictionary &p_property_info) {
	String hint_string = p_property_info["hint_string"];
	int colon = hint_string.find(":");
	if (colon < 0) {
		return StringName();
	}
	return StringName(hint_string.substr(colon + 1));
}

// Finds the `ITEMS: Array[Type]` property declared on p_table_script (a
// ResourceTable subclass) and returns Type's class name, or an empty
// StringName if there's no such property.
StringName _find_items_element_class_name(const Ref<Script> &p_table_script) {
	Array properties = p_table_script->get_script_property_list();
	for (int i = 0; i < properties.size(); i++) {
		Dictionary info = properties[i];
		if (StringName(info["name"]) == StringName("ITEMS") && (Variant::Type)(int64_t)info["type"] == Variant::ARRAY) {
			return _array_element_class_name(info);
		}
	}
	return StringName();
}

// What a table's ITEMS element type resolved to: its own exported
// properties (this table's columns). p_class_name is either a project
// global class (a scripted Resource type) or a native/engine type.
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

// Natural, case-insensitive sort by filename -- matches the table's own
// default (Name-column) sort order.
bool _compare_resources_by_name(const Variant &p_a, const Variant &p_b) {
	Ref<Resource> a = p_a;
	Ref<Resource> b = p_b;
	if (a.is_null() || b.is_null()) {
		return false;
	}
	return a->get_path().get_file().get_basename().naturalnocasecmp_to(b->get_path().get_file().get_basename()) < 0;
}

// Converts a CSV cell's raw text back to a property's declared type.
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

// Where regenerate_table saves (and run_table reads back) p_table_class_name's data.
String _generated_table_path(const StringName &p_table_class_name) {
	return String(GENERATED_TABLES_DIR) + "/" + String(p_table_class_name) + ".tres";
}

// Marks p_array (which must still be empty) as a typed Array[p_class_name],
// exactly as a compiled `Array[Type]` GDScript variable is typed. This
// matters because Object::set() into a script's `@export var ITEMS:
// Array[Type]` property silently rejects a plain untyped Array (leaving the
// property at its default, empty value) -- it has to already be typed to
// match.
void _make_typed_resource_array(Array &p_array, const StringName &p_class_name) {
	Ref<Script> script = _find_script_for_class(p_class_name);
	if (script.is_valid()) {
		// Matches what the GDScript compiler itself stores for `Array[Type]`
		// where Type is a project global class: the type's native base (e.g.
		// "Resource"), narrowed by the script.
		p_array.set_typed(Variant::OBJECT, script->get_instance_base_type(), script);
	} else {
		p_array.set_typed(Variant::OBJECT, p_class_name, Variant());
	}
}

// Shared body of import_csv/preview_import_csv: always reads the CSV and
// tallies how many resources of p_table_class_name's type would be added,
// updated in place, or deleted to match it; only actually creates/edits/
// saves/deletes anything (and regenerates the table's cache) when p_execute
// is true.
ResourceTableUtils::ImportPreview _import_csv(const StringName &p_table_class_name, const String &p_path, bool p_execute) {
	ResourceTableUtils::ImportPreview result;

	Dictionary table_result = ResourceTableUtils::run_table(p_table_class_name);
	StringName resource_class_name = table_result["resource_class_name"];
	if (resource_class_name == StringName()) {
		return result;
	}
	Array properties = table_result["properties"];
	Array existing = table_result["resources"];

	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ);
	if (file.is_null()) {
		result.error = FileAccess::get_open_error();
		return result;
	}

	PackedStringArray header = file->get_csv_line();

	HashMap<StringName, Variant::Type> property_types;
	for (int i = 0; i < properties.size(); i++) {
		Dictionary info = properties[i];
		property_types[StringName(String(info["name"]))] = (Variant::Type)(int64_t)info["type"];
	}

	// Every path named in the CSV, so any existing resource of this type
	// NOT mentioned can be deleted afterward -- the CSV is the source of
	// truth for exactly which resources should exist.
	HashSet<String> seen_paths;
	int adds = 0;
	int updates = 0;

	while (!file->eof_reached()) {
		PackedStringArray fields = file->get_csv_line();
		if (fields.size() == 0 || fields[0].is_empty()) {
			continue;
		}

		String path = fields[0];
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

		// Edit the resource already at that path, if there is one -- this
		// works whether or not it's one of this table's own (_is_valid())
		// resources, since a row identifies a resource by its actual path.
		Ref<Resource> resource;
		if (exists) {
			resource = ResourceLoader::get_singleton()->load(path);
		}
		if (resource.is_null()) {
			Ref<Script> resource_script = _find_script_for_class(resource_class_name);
			if (resource_script.is_valid()) {
				// Script::call("new") reflectively instantiating it would fail
				// here: Godot only allows that for @tool scripts while the
				// editor is running, and this resource type (e.g. a plain data
				// Resource like AAA/BBB) normally isn't one. Building a bare
				// Resource and attaching the script afterward is the same
				// tool-mode-independent path the resource loader itself uses.
				Ref<Resource> new_resource;
				new_resource.instantiate();
				new_resource->set_script(resource_script);
				resource = new_resource;
			} else {
				// A native/engine resource type -- ClassDB constructs it
				// directly, no script attachment needed or possible.
				resource = Ref<Resource>(ClassDB::instantiate(resource_class_name));
			}
		}
		if (resource.is_null()) {
			continue;
		}

		for (int col = 1; col < header.size() && col < fields.size(); col++) {
			StringName property_name = header[col];
			HashMap<StringName, Variant::Type>::ConstIterator type_it = property_types.find(property_name);
			if (type_it == property_types.end()) {
				continue;
			}
			resource->set(property_name, _parse_csv_value(fields[col], type_it->value));
		}

		Error save_err = ResourceSaver::get_singleton()->save(resource, path);
		if (save_err != OK) {
			UtilityFunctions::push_error("ResourceTableUtils: failed to save '", path, "' (error ", (int64_t)save_err, ") -- check that its parent directory exists.");
		}
	}

	int deletes = 0;
	for (int i = 0; i < existing.size(); i++) {
		Ref<Resource> resource = existing[i];
		if (resource.is_null()) {
			continue;
		}
		String existing_path = resource->get_path();
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

		// The files on disk (and hence what run_table's cache should show)
		// may have just changed -- refresh it so callers see the update
		// immediately instead of stale data from before the import.
		ResourceTableUtils::regenerate_table(p_table_class_name);
	}

	result.error = OK;
	result.adds = adds;
	result.updates = updates;
	result.deletes = deletes;
	return result;
}

} // namespace

void ResourceTableUtils::_bind_methods() {
	ClassDB::bind_static_method("ResourceTableUtils", D_METHOD("find_table_class_names"), &ResourceTableUtils::find_table_class_names);
	ClassDB::bind_static_method("ResourceTableUtils", D_METHOD("find_resources_of_type", "class_name"), &ResourceTableUtils::find_resources_of_type);
	ClassDB::bind_static_method("ResourceTableUtils", D_METHOD("regenerate_table", "table_class_name"), &ResourceTableUtils::regenerate_table);
	ClassDB::bind_static_method("ResourceTableUtils", D_METHOD("run_table", "table_class_name"), &ResourceTableUtils::run_table);
	ClassDB::bind_static_method("ResourceTableUtils", D_METHOD("export_csv", "table_class_name", "path"), &ResourceTableUtils::export_csv);
	ClassDB::bind_static_method("ResourceTableUtils", D_METHOD("import_csv", "table_class_name", "path"), &ResourceTableUtils::import_csv);
}

PackedStringArray ResourceTableUtils::find_table_class_names() {
	TypedArray<Dictionary> global_classes = ProjectSettings::get_singleton()->get_global_class_list();

	HashMap<StringName, StringName> global_class_bases;
	for (int i = 0; i < global_classes.size(); i++) {
		Dictionary entry = global_classes[i];
		global_class_bases[StringName(entry["class"])] = StringName(entry["base"]);
	}

	HashSet<String> names;
	for (int i = 0; i < global_classes.size(); i++) {
		Dictionary entry = global_classes[i];
		StringName class_name = entry["class"];
		if (_base_chain_inherits(entry["base"], "ResourceTable", global_class_bases)) {
			names.insert(class_name);
		}
	}

	PackedStringArray sorted_names;
	for (const String &name : names) {
		sorted_names.push_back(name);
	}
	sorted_names.sort();
	return sorted_names;
}

Array ResourceTableUtils::find_resources_of_type(const StringName &p_class_name) {
	Array results;
	_collect_resources_of_class("res://", p_class_name, results);
	return results;
}

StringName ResourceTableUtils::regenerate_table(const StringName &p_table_class_name) {
	Ref<Script> table_script = _find_script_for_class(p_table_class_name);
	if (table_script.is_null() || !table_script->can_instantiate()) {
		return StringName();
	}

	StringName resource_class_name = _find_items_element_class_name(table_script);
	if (resource_class_name == StringName()) {
		return StringName();
	}

	Ref<Resource> table_instance = Ref<Resource>(table_script->call("new"));
	if (table_instance.is_null()) {
		return StringName();
	}

	Array candidates = find_resources_of_type(resource_class_name);

	bool has_is_valid = table_instance->has_method("_is_valid");
	Array items;
	_make_typed_resource_array(items, resource_class_name);
	for (int i = 0; i < candidates.size(); i++) {
		Ref<Resource> candidate = candidates[i];
		if (candidate.is_null()) {
			continue;
		}
		if (!has_is_valid || (bool)table_instance->call("_is_valid", candidate)) {
			items.push_back(candidate);
		}
	}
	items.sort_custom(callable_mp_static(&_compare_resources_by_name));

	table_instance->set("ITEMS", items);

	if (!DirAccess::dir_exists_absolute(GENERATED_TABLES_DIR)) {
		DirAccess::make_dir_recursive_absolute(GENERATED_TABLES_DIR);
	}

	String path = _generated_table_path(p_table_class_name);
	Error save_err = ResourceSaver::get_singleton()->save(table_instance, path);
	if (save_err != OK) {
		UtilityFunctions::push_error("ResourceTableUtils: failed to save '", path, "' (error ", (int64_t)save_err, ").");
		return StringName();
	}

	return resource_class_name;
}

Dictionary ResourceTableUtils::run_table(const StringName &p_table_class_name) {
	Dictionary result;
	result["resource_class_name"] = StringName();
	result["properties"] = Array();
	result["resources"] = Array();

	Ref<Script> table_script = _find_script_for_class(p_table_class_name);
	if (table_script.is_null()) {
		return result;
	}

	StringName resource_class_name = _find_items_element_class_name(table_script);
	if (resource_class_name == StringName()) {
		return result;
	}

	ResourceClassInfo info = _resolve_resource_class_by_name(resource_class_name);
	if (!info.valid) {
		return result;
	}

	Array items;
	String path = _generated_table_path(p_table_class_name);
	if (ResourceLoader::get_singleton()->exists(path)) {
		Ref<Resource> table_instance = ResourceLoader::get_singleton()->load(path);
		if (table_instance.is_valid()) {
			items = table_instance->get("ITEMS");
		}
	}

	result["resource_class_name"] = resource_class_name;
	result["properties"] = info.properties;
	result["resources"] = items;
	return result;
}

Error ResourceTableUtils::export_csv(const StringName &p_table_class_name, const String &p_path) {
	Dictionary table_result = run_table(p_table_class_name);
	StringName resource_class_name = table_result["resource_class_name"];
	if (resource_class_name == StringName()) {
		return ERR_DOES_NOT_EXIST;
	}
	Array properties = table_result["properties"];
	Array resources = table_result["resources"];
	resources.sort_custom(callable_mp_static(&_compare_resources_by_name));

	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE);
	if (file.is_null()) {
		return FileAccess::get_open_error();
	}

	PackedStringArray header;
	header.push_back("Path");
	for (int i = 0; i < properties.size(); i++) {
		Dictionary info = properties[i];
		header.push_back(info["name"]);
	}
	file->store_csv_line(header);

	for (int i = 0; i < resources.size(); i++) {
		Ref<Resource> resource = resources[i];
		if (resource.is_null()) {
			continue;
		}
		PackedStringArray fields;
		fields.push_back(resource->get_path());
		for (int j = 0; j < properties.size(); j++) {
			Dictionary info = properties[j];
			StringName property_name = info["name"];
			fields.push_back(resource->get(property_name).stringify());
		}
		file->store_csv_line(fields);
	}

	return OK;
}

Error ResourceTableUtils::import_csv(const StringName &p_table_class_name, const String &p_path) {
	return _import_csv(p_table_class_name, p_path, /*p_execute=*/true).error;
}

ResourceTableUtils::ImportPreview ResourceTableUtils::preview_import_csv(const StringName &p_table_class_name, const String &p_path) {
	return _import_csv(p_table_class_name, p_path, /*p_execute=*/false);
}
