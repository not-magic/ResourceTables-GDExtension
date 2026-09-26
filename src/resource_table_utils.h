#pragma once

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

namespace godot {

// Shared discovery/run logic for ResourceTable subclasses -- the single
// implementation of "what resources does this table show", exposed as
// static methods so it's callable both from GDScript (e.g.
// addons/ResourceTables/export_resource_tables.gd, or any automation script)
// and from ResourceTablesPlugin without duplicating it in each language.
//
// A ResourceTable subclass (see resource_table.h) is itself a Resource,
// declaring an `@export var ITEMS: Array[Type]` -- Type is reflected off of
// ITEMS' typed-array hint (not passed explicitly anywhere), and is either a
// project global class (a scripted Resource type) or a native/engine type.
// ITEMS itself is (re)populated by regenerate_table, which scans res:// and
// saves the result to res://generated_resource_tables/<TableClassName>.tres
// -- run_table and the CSV export just read whatever's currently saved
// there, they don't scan res:// live.
class ResourceTableUtils : public Object {
	GDCLASS(ResourceTableUtils, Object)

protected:
	static void _bind_methods();

public:
	// Names of the project's own global classes that inherit ResourceTable,
	// sorted alphabetically.
	static PackedStringArray find_table_class_names();

	// Recursively finds every resource under res:// whose concrete type
	// (its script's global class name if it has one, its own native engine
	// class otherwise -- not a subclass match) is exactly p_class_name.
	static Array find_resources_of_type(const StringName &p_class_name);

	// Rebuilds and saves res://generated_resource_tables/<p_table_class_name>.tres:
	// a fresh instance of p_table_class_name with its ITEMS array set to
	// every resource of ITEMS' declared element type (see
	// find_resources_of_type), filtered through the table's _is_valid() if
	// it defines one, sorted naturally by name. Returns the resolved
	// resource_class_name (see run_table), or empty if p_table_class_name
	// doesn't exist, isn't instantiable, has no `ITEMS: Array[Type]`
	// property, or the save fails.
	static StringName regenerate_table(const StringName &p_table_class_name);

	// Reads back what regenerate_table last saved for p_table_class_name
	// (an empty "resources" array if it hasn't been generated yet -- this
	// does NOT scan res:// itself). Returns { "resource_class_name":
	// StringName, "properties": Array[Dictionary], "resources":
	// Array[Resource] } -- properties is ITEMS' element type's own exported
	// properties (i.e. this table's columns), reflected via
	// Script::get_script_property_list() for a project global class or via
	// ClassDB for a native type. resource_class_name is empty (properties/
	// resources empty too) if p_table_class_name doesn't exist or has no
	// `ITEMS: Array[Type]` property, or Type can't be resolved.
	static Dictionary run_table(const StringName &p_table_class_name);

	// Writes every resource p_table_class_name's table currently shows (see
	// run_table) to a CSV file at p_path: a header row (Path + each
	// property), then one row per resource (first field is its res:// path),
	// sorted by name. Returns an error if the table doesn't resolve (see
	// run_table) or the file can't be opened for writing.
	static Error export_csv(const StringName &p_table_class_name, const String &p_path);

	// Reads a CSV file previously written by export_csv (or matching its
	// format) and makes the table's resources match it exactly: a row whose
	// Path points at an existing resource updates it in place, a row whose
	// Path doesn't exist yet creates a new resource of p_table_class_name's
	// resource type there, and any resource currently in the table whose
	// path is NOT named by any row is deleted from disk. Unrecognized
	// columns are ignored. Saves every resource it touches, re-runs
	// regenerate_table for p_table_class_name, and rescans the filesystem
	// when done. Returns an error if the table doesn't resolve or the file
	// can't be opened for reading.
	static Error import_csv(const StringName &p_table_class_name, const String &p_path);

	// Result of preview_import_csv.
	struct ImportPreview {
		Error error = ERR_DOES_NOT_EXIST;
		int adds = 0;
		int updates = 0;
		int deletes = 0;
	};

	// Reads a CSV file (in import_csv's format) and reports what applying it
	// would do, without modifying or deleting anything: how many resources
	// would be newly created, how many existing ones would be updated in
	// place, and how many existing resources of this type aren't mentioned
	// and would be deleted. Meant to drive a confirmation prompt (in C++,
	// e.g. ResourceTablesPlugin) before actually calling import_csv -- not
	// bound to GDScript, since automation scripts can just call import_csv
	// directly. error is non-OK on the same conditions as import_csv, in
	// which case the counts are all 0.
	static ImportPreview preview_import_csv(const StringName &p_table_class_name, const String &p_path);
};

} // namespace godot
