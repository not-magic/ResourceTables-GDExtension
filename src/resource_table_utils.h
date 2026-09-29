#pragma once

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

namespace godot {

// Static discovery/save methods shared by ResourceTablesPlugin and GDScript
// callers (e.g. export_resource_tables.gd). No generated cache: browsing a
// type is always a live res:// scan.
class ResourceTableUtils : public Object {
	GDCLASS(ResourceTableUtils, Object)

protected:
	static void _bind_methods();

public:
	// Every resource under res:// whose concrete type (script global class
	// name, or native class if none) is exactly p_class_name.
	static Array find_resources_of_type(const StringName &p_class_name);

	// Concrete resource types with at least one instance under res://,
	// sorted alphabetically, excluding ResourceTable types themselves (a
	// generator's own output, not a browsable resource type in its own right).
	static PackedStringArray find_resource_type_names_with_instances();

	// The Script of every project global class inheriting ResourceTableGenerator
	// (a GDScript class, project/addons/ResourceTables/resource_table_generator.gd),
	// sorted alphabetically.
	static Array find_generator_scripts();

	// p_prefix followed by the smallest non-negative integer not already used
	// as some other p_resource_class_name instance's own "p_prefix_N"
	// basename anywhere under res:// (e.g. with AAA_0/AAA_1/AAA_3 present,
	// p_prefix "AAA" returns "AAA_2"; a p_prefix none of them use, e.g.
	// "NewAAA", returns "NewAAA_0") -- meant to name a new instance's file so
	// it doesn't collide with an existing one.
	static String find_safe_name(const StringName &p_resource_class_name, const String &p_prefix);

	// Instantiates p_class_name, saves it at p_path (creating parent
	// directories first), and returns it. Null if p_class_name isn't
	// instantiable or the save fails.
	static Ref<Resource> create_instance(const StringName &p_class_name, const String &p_path);

	// p_class_name's exported/editor-usage properties. Empty if p_class_name
	// can't be resolved.
	static Array find_properties_of_type(const StringName &p_class_name);

	// Loads and returns whatever's already saved at p_path, if anything;
	// otherwise instantiates p_class_name and sets its resource_path to
	// p_path (creating parent directories first). Doesn't save -- nothing
	// exists at p_path until the caller saves it. Null if p_class_name isn't
	// instantiable and nothing exists at p_path yet.
	static Ref<Resource> find_or_create(const StringName &p_class_name, const String &p_path);

	// A fresh, unsaved instance of p_class_name, or null if it doesn't
	// resolve. Unlike find_or_create, never touches disk or sets a
	// resource_path.
	static Ref<Resource> instantiate_resource_of_type(const StringName &p_class_name);

	// Writes p_resource_paths' resources (p_resource_class_name resolves
	// which properties are the columns) to a CSV at p_path: a header row,
	// then one row per resource sorted by name. Error if p_resource_class_name
	// can't be resolved or the file can't be opened for writing.
	static Error export_csv(const StringName &p_resource_class_name, const PackedStringArray &p_resource_paths, const String &p_path);

	// Applies a CSV at p_path (export_csv's format) to p_resource_paths: a
	// row with an existing Path updates it in place, a new Path creates a
	// resource there, and any path in p_resource_paths not named by a row is
	// deleted. Saves everything it touches and rescans the filesystem.
	static Error import_csv(const StringName &p_resource_class_name, const PackedStringArray &p_resource_paths, const String &p_path);

	// Result of preview_import_csv.
	struct ImportPreview {
		Error error = ERR_DOES_NOT_EXIST;
		int adds = 0;
		int updates = 0;
		int deletes = 0;
	};

	// Dry run of import_csv: counts what it would add/update/delete without
	// modifying anything. Not bound to GDScript (call import_csv directly
	// there). error matches import_csv's own failure conditions.
	static ImportPreview preview_import_csv(const StringName &p_resource_class_name, const PackedStringArray &p_resource_paths, const String &p_path);
};

} // namespace godot
