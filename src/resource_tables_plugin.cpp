#include "resource_tables_plugin.h"

#include "resource_table_utils.h"

#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_inspector.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/h_box_container.hpp>
#include <godot_cpp/classes/menu_bar.hpp>
#include <godot_cpp/classes/popup_menu.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/classes/style_box_flat.hpp>
#include <godot_cpp/classes/tree_item.hpp>
#include <godot_cpp/classes/v_box_container.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>

using namespace godot;

namespace {

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

enum FileMenuId {
	FILE_MENU_NEW_TABLE,
	FILE_MENU_EXPORT_CSV,
	FILE_MENU_IMPORT_CSV,
};

// Converts an arbitrary filename (its extension-less basename) into a
// GDScript-identifier-safe CamelCase class name, e.g. "enemy_table" ->
// "EnemyTable", "AAA-11" -> "AAA11". Only the character right after a
// non-alphanumeric run is uppercased -- the rest of each run is left as-is,
// so an already-CamelCase basename like "EnemyTable" passes through
// unchanged.
String _filename_to_camel_case(const String &p_basename) {
	String result;
	bool capitalize_next = true;
	for (int i = 0; i < p_basename.length(); i++) {
		char32_t c = p_basename[i];
		bool is_alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
		if (!is_alnum) {
			capitalize_next = true;
			continue;
		}
		if (capitalize_next) {
			if (c >= 'a' && c <= 'z') {
				c -= ('a' - 'A');
			}
			capitalize_next = false;
		}
		result += c;
	}

	if (!result.is_empty() && result[0] >= '0' && result[0] <= '9') {
		result = "_" + result; // Identifiers can't start with a digit.
	}
	return result;
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

	MenuBar *menu_bar = memnew(MenuBar);
	PopupMenu *file_menu = memnew(PopupMenu);
	file_menu->set_name("File");
	file_menu->add_item("New ResourceTable...", FILE_MENU_NEW_TABLE);
	file_menu->add_separator();
	file_menu->add_item("Export to CSV...", FILE_MENU_EXPORT_CSV);
	file_menu->add_item("Import from CSV...", FILE_MENU_IMPORT_CSV);
	file_menu->connect("id_pressed", callable_mp(this, &ResourceTablesPlugin::_on_file_menu_id_pressed));
	menu_bar->add_child(file_menu);
	top_row->add_child(menu_bar);

	class_dropdown = memnew(OptionButton);
	class_dropdown->set_h_size_flags(Control::SIZE_EXPAND_FILL);
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

	csv_dialog = memnew(EditorFileDialog);
	csv_dialog->set_access(EditorFileDialog::ACCESS_FILESYSTEM);
	csv_dialog->add_filter("*.csv", "CSV Files");
	csv_dialog->connect("file_selected", callable_mp(this, &ResourceTablesPlugin::_on_csv_file_selected));
	main_panel->add_child(csv_dialog);

	import_confirm_dialog = memnew(ConfirmationDialog);
	import_confirm_dialog->set_title("Import from CSV");
	import_confirm_dialog->connect("confirmed", callable_mp(this, &ResourceTablesPlugin::_on_import_confirmed));
	main_panel->add_child(import_confirm_dialog);

	// Step 2 of "New ResourceTable...": saving the new .gd file, once a
	// Resource type has been picked via EditorInterface::popup_create_dialog
	// (step 1 -- see _on_file_menu_id_pressed).
	new_table_file_dialog = memnew(EditorFileDialog);
	new_table_file_dialog->set_access(EditorFileDialog::ACCESS_RESOURCES);
	new_table_file_dialog->set_file_mode(EditorFileDialog::FILE_MODE_SAVE_FILE);
	new_table_file_dialog->add_filter("*.gd", "GDScript Files");
	new_table_file_dialog->connect("file_selected", callable_mp(this, &ResourceTablesPlugin::_on_new_table_path_selected));
	main_panel->add_child(new_table_file_dialog);

	add_control_to_bottom_panel(main_panel, "ResourceTables");

	// Catches resources being added, removed, or externally modified on disk.
	EditorInterface::get_singleton()->get_resource_filesystem()->connect("filesystem_changed", callable_mp(this, &ResourceTablesPlugin::_on_filesystem_changed));
	// Catches in-place edits made through the regular property editor,
	// whatever object happens to be selected there.
	EditorInterface::get_singleton()->get_inspector()->connect("property_edited", callable_mp(this, &ResourceTablesPlugin::_on_inspector_property_edited));

	_refresh_class_dropdown();
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

void ResourceTablesPlugin::_refresh_class_dropdown() {
	StringName previous_selection = current_class_name;

	class_dropdown->clear();
	PackedStringArray class_names = ResourceTableUtils::find_table_class_names();
	for (const String &class_name : class_names) {
		class_dropdown->add_item(class_name);
	}

	int index_to_select = class_names.find(previous_selection);
	if (index_to_select < 0 && !class_names.is_empty()) {
		index_to_select = 0;
	}

	if (index_to_select < 0) {
		// No ResourceTable subclasses left in the project.
		current_class_name = StringName();
		current_resource_class_name = StringName();
		current_properties = Array();
		table_tree->clear();
		table_tree->set_columns(1);
		return;
	}

	class_dropdown->select(index_to_select);
	StringName selected_class_name = class_names[index_to_select];
	if (selected_class_name != previous_selection) {
		sort_column = 0; // Reset to the default sort (Name, ascending) for the newly selected class.
		sort_ascending = true;
	}
	_rebuild_table(selected_class_name);
}

void ResourceTablesPlugin::_on_class_selected(int p_index) {
	sort_column = 0; // Reset to the default sort (Name, ascending) for the newly selected class.
	sort_ascending = true;
	_rebuild_table(class_dropdown->get_item_text(p_index));
}

void ResourceTablesPlugin::_rebuild_table(const StringName &p_class_name) {
	current_class_name = p_class_name;
	table_tree->clear();

	Dictionary table_result = ResourceTableUtils::run_table(p_class_name);
	StringName resource_class_name = table_result["resource_class_name"];
	if (resource_class_name == StringName()) {
		table_tree->set_columns(1);
		current_properties = Array();
		current_resource_class_name = StringName();
		return;
	}
	current_resource_class_name = resource_class_name;
	current_properties = table_result["properties"];

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

	Array resources = table_result["resources"];

	if (sort_column >= 0 && sort_column <= current_properties.size()) {
		resources.sort_custom(callable_mp(this, &ResourceTablesPlugin::_compare_by_sort_column));
	}

	TreeItem *root = table_tree->create_item();
	for (int i = 0; i < resources.size(); i++) {
		Ref<Resource> resource = resources[i];
		if (resource.is_null()) {
			continue;
		}

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

	// A script's global class name if the resource has one, or its own
	// native engine class otherwise -- matches how ResourceTableUtils
	// resolves and matches resource types (see _resource_type_name).
	Ref<Script> script = resource->get_script();
	StringName resource_type_name = script.is_valid() ? script->get_global_name() : StringName(resource->get_class());
	if (resource_type_name != current_resource_class_name) {
		return; // Not an instance of the type currently displayed.
	}

	// Deferred for the same reentrancy reason as _on_column_title_clicked.
	callable_mp(this, &ResourceTablesPlugin::_rebuild_table).call_deferred(current_class_name);
}

void ResourceTablesPlugin::_on_filesystem_changed() {
	// A file being added, removed, or reloaded after an external edit could
	// mean: a new/removed ResourceTable subclass (the dropdown itself is
	// stale), or a new/removed/changed resource of the currently displayed
	// type (the table rows are stale). _refresh_class_dropdown re-scans the
	// dropdown and then rebuilds the table for whatever ends up selected.
	// Deferred for the same reentrancy reason as _on_column_title_clicked.
	callable_mp(this, &ResourceTablesPlugin::_refresh_class_dropdown).call_deferred();
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
	if (a.is_null() || b.is_null()) {
		return false;
	}

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

void ResourceTablesPlugin::_on_file_menu_id_pressed(int p_id) {
	switch (p_id) {
		case FILE_MENU_NEW_TABLE: {
			new_table_resource_class_name = StringName();
			EditorInterface::get_singleton()->popup_create_dialog(
					callable_mp(this, &ResourceTablesPlugin::_on_new_table_resource_class_selected),
					"Resource", "", "Select Resource Type");
			break;
		}
		case FILE_MENU_EXPORT_CSV: {
			csv_dialog_is_export = true;
			csv_dialog->set_file_mode(EditorFileDialog::FILE_MODE_SAVE_FILE);
			csv_dialog->set_title("Export to CSV");
			csv_dialog->set_current_file(String(current_resource_class_name) + ".csv");
			csv_dialog->popup_centered_ratio(0.5);
			break;
		}
		case FILE_MENU_IMPORT_CSV: {
			csv_dialog_is_export = false;
			csv_dialog->set_file_mode(EditorFileDialog::FILE_MODE_OPEN_FILE);
			csv_dialog->set_title("Import from CSV");
			csv_dialog->popup_centered_ratio(0.5);
			break;
		}
	}
}

void ResourceTablesPlugin::_on_csv_file_selected(String p_path) {
	if (csv_dialog_is_export) {
		ResourceTableUtils::export_csv(current_class_name, p_path);
		return;
	}

	ResourceTableUtils::ImportPreview preview = ResourceTableUtils::preview_import_csv(current_class_name, p_path);
	if (preview.error != OK) {
		return;
	}

	int adds = preview.adds;
	int updates = preview.updates;
	int deletes = preview.deletes;
	if (adds == 0 && updates == 0 && deletes == 0) {
		return; // Nothing would change -- no need to ask.
	}

	pending_import_csv_path = p_path;
	import_confirm_dialog->set_text(
			"Importing this CSV will:\n"
			"  " + String::num_int64(adds) + " resource(s) added\n" +
			"  " + String::num_int64(updates) + " resource(s) updated\n" +
			"  " + String::num_int64(deletes) + " resource(s) deleted\n\n"
			"This cannot be undone. Continue?");
	import_confirm_dialog->popup_centered();
}

void ResourceTablesPlugin::_on_import_confirmed() {
	ResourceTableUtils::import_csv(current_class_name, pending_import_csv_path);
	_rebuild_table(current_class_name);
}

void ResourceTablesPlugin::_on_new_table_resource_class_selected(StringName p_class_name) {
	if (p_class_name == StringName()) {
		return; // The class browser was cancelled.
	}
	new_table_resource_class_name = p_class_name;

	// Step 2: where to save the new ResourceTable script.
	new_table_file_dialog->set_current_file(String(p_class_name) + "Table.gd");
	new_table_file_dialog->popup_centered_ratio(0.5);
}

void ResourceTablesPlugin::_on_new_table_path_selected(String p_path) {
	if (new_table_resource_class_name == StringName()) {
		return;
	}

	String class_name = _filename_to_camel_case(p_path.get_file().get_basename());
	if (class_name.is_empty()) {
		return;
	}

	String content = "@tool\n";
	content += "class_name " + class_name + "\n";
	content += "extends ResourceTable\n\n";
	content += "@export var ITEMS: Array[" + String(new_table_resource_class_name) + "]\n";

	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE);
	if (file.is_null()) {
		return;
	}
	file->store_string(content);
	file->close();

	EditorInterface::get_singleton()->get_resource_filesystem()->scan();

	Ref<Script> new_script = ResourceLoader::get_singleton()->load(p_path, "", ResourceLoader::CACHE_MODE_IGNORE);
	if (new_script.is_valid()) {
		EditorInterface::get_singleton()->edit_script(new_script);
	}
}
