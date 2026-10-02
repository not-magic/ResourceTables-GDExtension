#pragma once

#ifdef TOOLS_ENABLED

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

namespace godot {

class ResourceTableUtils : public Object {
	GDCLASS(ResourceTableUtils, Object) // NOLINT

protected:
	static void _bind_methods();

public:
	static Array find_resources_of_type(const StringName &p_class_name);

	static PackedStringArray find_resource_type_names();

	static Array find_generator_scripts();

	// p_class_name's exported/editor-usage properties. Empty if p_class_name
	// can't be resolved.
	static Array find_properties_of_type(const StringName &p_class_name);

	// Loads what's at p_path, else instantiates p_class_name with that path (creating parent dirs). Doesn't save.
	static Ref<Resource> find_or_create_resource(const StringName &p_class_name, const String &p_path);

	// A fresh, unsaved instance of p_class_name, or null if it doesn't
	// resolve. Unlike find_or_create_resource, never touches disk or sets a
	// resource_path.
	static Ref<Resource> instantiate_resource_of_type(const StringName &p_class_name);

	// Writes a header row plus one row per resource, sorted by name. Error if the class or file can't be resolved/opened.
	static Error export_csv(const StringName &p_resource_class_name, const PackedStringArray &p_resource_paths, const String &p_path);

	// Applies a CSV (export_csv's format): existing Path updates, new Path creates, unnamed paths are deleted.
	static Error import_csv(const StringName &p_resource_class_name, const PackedStringArray &p_resource_paths, const String &p_path);

	struct ImportPreview {
		Error error = ERR_DOES_NOT_EXIST;
		int add_total = 0;
		int update_total = 0;
		int delete_total = 0;
	};

	// Dry run of import_csv: counts what it would add/update/delete without
	// modifying anything. Not bound to GDScript (call import_csv directly
	// there). error matches import_csv's own failure conditions.
	static ImportPreview preview_import_csv(const StringName &p_resource_class_name, const PackedStringArray &p_resource_paths, const String &p_path);
};

} // namespace godot

#endif // TOOLS_ENABLED
