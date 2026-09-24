#include "resource_tables_plugin.h"

#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_inspector.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/h_box_container.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/classes/style_box_flat.hpp>
#include <godot_cpp/classes/tree_item.hpp>
#include <godot_cpp/classes/v_box_container.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/templates/hash_set.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>

using namespace godot;

namespace {

// Resolves p_base's chain -- possibly through other project global classes
// -- down to a real engine/GDExtension class, then checks whether that
// inherits Resource.
bool _base_chain_inherits_resource(StringName p_base, const HashMap<StringName, StringName> &p_global_class_bases) {
	for (int i = 0; i < 32 && p_base != StringName(); i++) {
		if (p_base == StringName("Resource")) {
			return true;
		}
		HashMap<StringName, StringName>::ConstIterator it = p_global_class_bases.find(p_base);
		if (it == p_global_class_bases.end()) {
			return ClassDB::is_parent_class(p_base, "Resource");
		}
		p_base = it->value;
	}
	return false;
}

// Names of the project's own global classes (i.e. scripts with a
// `class_name`) that inherit Resource, sorted alphabetically. Engine/
// GDExtension-builtin Resource types (Texture2D, AudioEffect, ...) are
// deliberately excluded -- only resource types actually created in this
// project belong in the dropdown.
PackedStringArray _find_resource_class_names() {
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
		if (_base_chain_inherits_resource(entry["base"], global_class_bases)) {
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

// Recursively finds every .tres/.res file under p_dir whose attached script
// is exactly p_class_name (not a subclass -- this addon shows one flat
// table per concrete resource type).
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

		Ref<Script> script = resource->get_script();
		if (script.is_valid() && script->get_global_name() == p_class_name) {
			r_results.push_back(resource);
		}
	}
	dir->list_dir_end();
}

// Whether a script property (as returned by Script::get_script_property_list)
// is one the user actually exported, rather than a plain internal `var`.
bool _is_editable_property(const Dictionary &p_property_info) {
	int64_t usage = p_property_info["usage"];
	return (usage & PROPERTY_USAGE_EDITOR) != 0;
}

struct RangeConfig {
	double min;
	double max;
	double step;
};

// Reads the min/max/step out of an int property's `@export_range(...)`
// hint, if it has one (PROPERTY_HINT_RANGE, hint_string
// "min,max[,step][,flags...]"), falling back to a wide default range
// otherwise. "or_greater"/"or_less" widen the respective bound back out,
// since TreeItem's range cell has no separate "allow beyond min/max" flag
// like SpinBox does.
RangeConfig _get_range_config(const Dictionary &p_property_info) {
	RangeConfig config{ -1e15, 1e15, 1.0 };

	int64_t hint = p_property_info["hint"];
	if (hint != PROPERTY_HINT_RANGE) {
		return config;
	}

	PackedStringArray parts = String(p_property_info["hint_string"]).split(",");
	if (parts.size() >= 2) {
		String min_str = parts[0].strip_edges();
		String max_str = parts[1].strip_edges();
		if (min_str.is_valid_float()) {
			config.min = min_str.to_float();
		}
		if (max_str.is_valid_float()) {
			config.max = max_str.to_float();
		}
	}
	if (parts.size() >= 3) {
		String step_str = parts[2].strip_edges();
		if (step_str.is_valid_float()) {
			config.step = step_str.to_float();
		}
	}
	for (int i = 2; i < parts.size(); i++) {
		String flag = parts[i].strip_edges();
		if (flag == "or_greater") {
			config.max = 1e15;
		} else if (flag == "or_less") {
			config.min = -1e15;
		}
	}

	return config;
}

} // namespace

void ResourceTablesPlugin::_bind_methods() {
}

void ResourceTablesPlugin::_enter_tree() {
	main_panel = memnew(VBoxContainer);
	main_panel->set_name("ResourceTables");
	main_panel->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	main_panel->set_v_size_flags(Control::SIZE_EXPAND_FILL);

	HBoxContainer *top_row = memnew(HBoxContainer);
	main_panel->add_child(top_row);

	class_dropdown = memnew(OptionButton);
	class_dropdown->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	PackedStringArray class_names = _find_resource_class_names();
	for (const String &class_name : class_names) {
		class_dropdown->add_item(class_name);
	}
	class_dropdown->connect("item_selected", callable_mp(this, &ResourceTablesPlugin::_on_class_selected));
	top_row->add_child(class_dropdown);

	table_tree = memnew(Tree);
	table_tree->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	table_tree->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	table_tree->set_hide_root(true);
	table_tree->set_select_mode(Tree::SELECT_ROW);
	table_tree->set_column_titles_visible(true);
	table_tree->connect("item_selected", callable_mp(this, &ResourceTablesPlugin::_on_row_selected));
	table_tree->connect("item_edited", callable_mp(this, &ResourceTablesPlugin::_on_item_edited));
	table_tree->connect("column_title_clicked", callable_mp(this, &ResourceTablesPlugin::_on_column_title_clicked));

	// Darker background with light text and faint row separators, so the
	// grid actually reads as a table. The header row is lighter than the
	// body so it reads as distinct from the data rows.
	const Color row_color(0.108, 0.108, 0.108); // base 0.09, +20%
	const Color header_color(0.1404, 0.1404, 0.1404); // row_color, +30%

	Ref<StyleBoxFlat> panel_style;
	panel_style.instantiate();
	panel_style->set_bg_color(row_color);
	table_tree->add_theme_stylebox_override("panel", panel_style);

	Ref<StyleBoxFlat> header_style;
	header_style.instantiate();
	header_style->set_bg_color(header_color);
	table_tree->add_theme_stylebox_override("title_button_normal", header_style);

	table_tree->add_theme_color_override("font_color", Color(0.9, 0.9, 0.9));
	table_tree->add_theme_color_override("guide_color", Color(1.0, 1.0, 1.0, 0.08));
	table_tree->add_theme_constant_override("draw_guides", 1);

	main_panel->add_child(table_tree);

	add_control_to_bottom_panel(main_panel, "Resources");

	// Catches resources being added, removed, or externally modified on disk.
	EditorInterface::get_singleton()->get_resource_filesystem()->connect("filesystem_changed", callable_mp(this, &ResourceTablesPlugin::_on_filesystem_changed));
	// Catches in-place edits made through the regular property editor,
	// whatever object happens to be selected there.
	EditorInterface::get_singleton()->get_inspector()->connect("property_edited", callable_mp(this, &ResourceTablesPlugin::_on_inspector_property_edited));

	if (!class_names.is_empty()) {
		_rebuild_table(class_names[0]);
	}
}

void ResourceTablesPlugin::_exit_tree() {
	EditorFileSystem *filesystem = EditorInterface::get_singleton()->get_resource_filesystem();
	if (filesystem) {
		filesystem->disconnect("filesystem_changed", callable_mp(this, &ResourceTablesPlugin::_on_filesystem_changed));
	}
	EditorInspector *inspector = EditorInterface::get_singleton()->get_inspector();
	if (inspector) {
		inspector->disconnect("property_edited", callable_mp(this, &ResourceTablesPlugin::_on_inspector_property_edited));
	}

	if (main_panel) {
		remove_control_from_bottom_panel(main_panel);
		main_panel->queue_free();
		main_panel = nullptr;
		class_dropdown = nullptr;
		table_tree = nullptr;
	}
}

void ResourceTablesPlugin::_on_class_selected(int p_index) {
	sort_column = 0; // Reset to the default sort (Name, ascending) for the newly selected class.
	sort_ascending = true;
	_rebuild_table(class_dropdown->get_item_text(p_index));
}

void ResourceTablesPlugin::_rebuild_table(const StringName &p_class_name) {
	current_class_name = p_class_name;
	table_tree->clear();

	Ref<Script> script = _find_script_for_class(p_class_name);
	if (script.is_null()) {
		table_tree->set_columns(1);
		current_properties = Array();
		return;
	}

	Array all_properties = script->get_script_property_list();
	current_properties.clear();
	for (int i = 0; i < all_properties.size(); i++) {
		if (_is_editable_property(all_properties[i])) {
			current_properties.push_back(all_properties[i]);
		}
	}

	table_tree->set_columns(1 + current_properties.size());
	// Every column can be resized by dragging its header border; the name
	// column just starts narrower than the property columns.
	table_tree->set_column_expand(0, false);
	table_tree->set_column_custom_minimum_width(0, 120);

	const String ascending_suffix = " ^";
	const String descending_suffix = " v";
	table_tree->set_column_title(0, "Name" + (sort_column == 0 ? (sort_ascending ? ascending_suffix : descending_suffix) : ""));
	for (int i = 0; i < current_properties.size(); i++) {
		Dictionary info = current_properties[i];
		String title = info["name"];
		if (sort_column == 1 + i) {
			title += sort_ascending ? ascending_suffix : descending_suffix;
		}
		table_tree->set_column_title(1 + i, title);
		table_tree->set_column_expand(1 + i, true);
	}

	Array resources;
	_collect_resources_of_class("res://", p_class_name, resources);
	if (sort_column >= 0 && sort_column <= current_properties.size()) {
		resources.sort_custom(callable_mp(this, &ResourceTablesPlugin::_compare_by_sort_column));
	}

	TreeItem *root = table_tree->create_item();
	for (int i = 0; i < resources.size(); i++) {
		Ref<Resource> resource = resources[i];

		TreeItem *row = table_tree->create_item(root);
		row->set_text(0, resource->get_path().get_file().get_basename());
		row->set_metadata(0, resource);

		for (int j = 0; j < current_properties.size(); j++) {
			Dictionary info = current_properties[j];
			StringName property_name = info["name"];
			Variant value = resource->get(property_name);
			int column = 1 + j;

			// String/int/float/bool are directly editable in the cell itself
			// (committed edits land in _on_item_edited); anything else falls
			// back to a read-only stringified display.
			switch (value.get_type()) {
				case Variant::BOOL: {
					row->set_cell_mode(column, TreeItem::CELL_MODE_CHECK);
					row->set_checked(column, value);
					row->set_editable(column, true);
					break;
				}
				case Variant::INT: {
					RangeConfig range = _get_range_config(info);
					row->set_cell_mode(column, TreeItem::CELL_MODE_RANGE);
					row->set_range_config(column, range.min, range.max, range.step);
					row->set_range(column, value);
					row->set_editable(column, true);
					break;
				}
				case Variant::FLOAT: {
					row->set_cell_mode(column, TreeItem::CELL_MODE_STRING);
					row->set_text(column, value.stringify());
					row->set_editable(column, true);
					break;
				}
				case Variant::STRING:
				case Variant::STRING_NAME: {
					row->set_cell_mode(column, TreeItem::CELL_MODE_STRING);
					row->set_text(column, value);
					row->set_editable(column, true);
					break;
				}
				default: {
					row->set_cell_mode(column, TreeItem::CELL_MODE_STRING);
					row->set_text(column, value.stringify());
					row->set_editable(column, false);
					break;
				}
			}
		}
	}
}

void ResourceTablesPlugin::_on_row_selected() {
	TreeItem *selected = table_tree->get_selected();
	if (!selected) {
		return;
	}

	Ref<Resource> resource = selected->get_metadata(0);
	if (resource.is_valid()) {
		EditorInterface::get_singleton()->edit_resource(resource);
	}
}

void ResourceTablesPlugin::_on_item_edited() {
	TreeItem *item = table_tree->get_edited();
	if (!item) {
		return;
	}

	int column = table_tree->get_edited_column();
	if (column <= 0 || column > current_properties.size()) {
		return;
	}

	Ref<Resource> resource = item->get_metadata(0);
	if (resource.is_null()) {
		return;
	}

	Dictionary info = current_properties[column - 1];
	StringName property_name = info["name"];
	Variant::Type type = (Variant::Type)(int64_t)info["type"];

	switch (type) {
		case Variant::BOOL:
			resource->set(property_name, item->is_checked(column));
			break;
		case Variant::INT:
			resource->set(property_name, (int64_t)item->get_range(column));
			break;
		case Variant::FLOAT:
			resource->set(property_name, item->get_text(column).to_float());
			break;
		default:
			resource->set(property_name, item->get_text(column));
			break;
	}
}

void ResourceTablesPlugin::_on_inspector_property_edited(String p_property) {
	// Whatever object the Inspector currently has open -- it doesn't have to
	// be one of this table's own rows (e.g. it could be a brand new resource
	// not yet saved to disk, or a value being tweaked before this table's
	// class was even selected).
	Object *edited_object = EditorInterface::get_singleton()->get_inspector()->get_edited_object();
	Ref<Resource> resource = Object::cast_to<Resource>(edited_object);
	if (resource.is_null()) {
		return;
	}

	Ref<Script> script = resource->get_script();
	if (script.is_null() || script->get_global_name() != current_class_name) {
		return; // Not an instance of the type currently displayed.
	}

	// Deferred for the same reentrancy reason as _on_column_title_clicked.
	callable_mp(this, &ResourceTablesPlugin::_rebuild_table).call_deferred(current_class_name);
}

void ResourceTablesPlugin::_on_filesystem_changed() {
	// A file being added, removed, or reloaded after an external edit means
	// the currently displayed table (the only one visible at a time) may be
	// stale -- e.g. a new resource of this type could now exist on disk.
	// Deferred for the same reentrancy reason as _on_column_title_clicked.
	if (current_class_name != StringName()) {
		callable_mp(this, &ResourceTablesPlugin::_rebuild_table).call_deferred(current_class_name);
	}
}

void ResourceTablesPlugin::_on_column_title_clicked(int p_column, int p_mouse_button_index) {
	if (p_mouse_button_index != MOUSE_BUTTON_LEFT) {
		return;
	}

	if (sort_column == p_column) {
		sort_ascending = !sort_ascending;
	} else {
		sort_column = p_column;
		sort_ascending = true;
	}

	// Deferred: this signal fires from inside the Tree's own input handling,
	// and rebuilding (which frees and recreates all of its TreeItems)
	// synchronously from in there corrupts the Tree's internal state -- the
	// table would render empty until the next unrelated redraw.
	callable_mp(this, &ResourceTablesPlugin::_rebuild_table).call_deferred(current_class_name);
}

bool ResourceTablesPlugin::_compare_by_sort_column(Variant p_a, Variant p_b) {
	Ref<Resource> a = p_a;
	Ref<Resource> b = p_b;

	Variant value_a;
	Variant value_b;
	if (sort_column == 0) {
		value_a = a->get_path().get_file().get_basename();
		value_b = b->get_path().get_file().get_basename();
	} else {
		Dictionary info = current_properties[sort_column - 1];
		StringName property_name = info["name"];
		value_a = a->get(property_name);
		value_b = b->get(property_name);
	}

	if (!sort_ascending) {
		Variant temp = value_a;
		value_a = value_b;
		value_b = temp;
	}

	// String columns (including Name) sort the way the FileSystem dock
	// does -- "AAA_2" before "AAA_10" -- rather than plain lexical order,
	// which would put "AAA_10" first.
	if (value_a.get_type() == Variant::STRING && value_b.get_type() == Variant::STRING) {
		return String(value_a).naturalnocasecmp_to(value_b) < 0;
	}
	return value_a < value_b;
}
