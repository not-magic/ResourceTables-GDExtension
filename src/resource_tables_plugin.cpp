#include "resource_tables_plugin.h"

#include "resource_table_container.h"
#include "resource_table_name.h"
#include "resource_table_utils.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_inspector.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_property.hpp>
#include <godot_cpp/classes/editor_settings.hpp>
#include <godot_cpp/classes/editor_undo_redo_manager.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/file_system_dock.hpp>
#include <godot_cpp/classes/grid_container.hpp>
#include <godot_cpp/classes/h_box_container.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/margin_container.hpp>
#include <godot_cpp/classes/menu_bar.hpp>
#include <godot_cpp/classes/popup_menu.hpp>
#include <godot_cpp/classes/popup_panel.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/resource_saver.hpp>
#include <godot_cpp/classes/resource_uid.hpp>
#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/texture_rect.hpp>
#include <godot_cpp/classes/theme.hpp>
#include <godot_cpp/classes/v_box_container.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <vector>

using namespace godot;

namespace {

enum ToolsMenuId {
	TOOLS_MENU_NEW_GENERATOR,
	TOOLS_MENU_GENERATE_ALL,
	TOOLS_MENU_EXPORT_CSV,
	TOOLS_MENU_IMPORT_CSV,
};

// Table column 0 is the revert icon, 1 is Name, 2+i are properties; sort_column is offset by one
// (see _on_sort_header_pressed).

constexpr float HEADER_COLUMN_WIDTH = 200.0;
constexpr int TYPE_ICON_MARGIN = 8;
const char *const NEW_RESOURCE_DIR_KEY = "new_resource_directory";

const char *EXPORT_RESOURCE_TABLES_SCRIPT_PATH = "res://addons/resource_tables/export_resource_tables.gd";

const char *DEFAULT_NEW_GENERATOR_PATH = "res://NewTable.gd";
const char *NEW_GENERATOR_TEMPLATE_PATH = "res://addons/resource_tables/new_resource_table_generator_template.txt";

String column_property_name(const Array &p_properties, int p_column) {
	if (p_column == 1) {
		return "Name";
	}
	if (p_column >= 2 && p_column - 2 < p_properties.size()) {
		const Dictionary info = p_properties[p_column - 2];
		return info["name"];
	}
	return String();
}

PackedStringArray find_visible_resource_paths(const Array &p_row_resources) {
	PackedStringArray paths;
	for (int i = 0; i < p_row_resources.size(); i++) {
		const Ref<Resource> resource = p_row_resources[i];
		if (resource.is_valid()) {
			paths.push_back(resource->get_path());
		}
	}
	return paths;
}

void create_generator_script(const String &p_resource_class_name, const String &p_path) {
	Ref<FileAccess> template_file = FileAccess::open(NEW_GENERATOR_TEMPLATE_PATH, FileAccess::READ);
	if (template_file.is_null()) {
		UtilityFunctions::push_error("ResourceTablesPlugin: failed to open '", NEW_GENERATOR_TEMPLATE_PATH, "'.");
		return;
	}
	String content = template_file->get_as_text();
	template_file.unref();

	String table_dir = p_path.get_base_dir();
	if (!table_dir.ends_with("/")) {
		table_dir += "/"; // get_base_dir() keeps the slash only for a bare "res://".
	}
	const String table_name = p_path.get_file().get_basename();

	content = content.replace("${RESOURCE_CLASS_NAME}", p_resource_class_name);
	content = content.replace("${RESOURCE_TABLE_DIR}", table_dir);
	content = content.replace("${RESOURCE_TABLE_NAME}", table_name);

	const Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE);
	if (file.is_null()) {
		return;
	}
	file->store_string(content);
	file->close();

	EditorInterface::get_singleton()->get_resource_filesystem()->scan();

	const Ref<Script> new_script = ResourceLoader::get_singleton()->load(p_path, "", ResourceLoader::CACHE_MODE_IGNORE);
	if (new_script.is_valid()) {
		EditorInterface::get_singleton()->edit_script(new_script);
	}
}

String find_unused_resource_name(const String &p_dir, const String &p_class_name) {
	for (int index = 0;; index++) {
		const String file_name = p_class_name + String("_") + String::num_int64(index) + String(".tres");
		if (!FileAccess::file_exists(p_dir.path_join(file_name))) {
			return file_name;
		}
	}
}

bool is_before_by_sort_column(Variant p_a, Variant p_b, int p_sort_column, const Array &p_properties, bool p_is_ascending) {
	const Ref<Resource> a = p_a;
	const Ref<Resource> b = p_b;
	if (a.is_null() || b.is_null()) {
		return false;
	}

	Variant value_a;
	Variant value_b;
	if (p_sort_column == 0) {
		value_a = a->get_path().get_file().get_basename();
		value_b = b->get_path().get_file().get_basename();
	} else {
		const Dictionary info = p_properties[p_sort_column - 1];
		const StringName property_name = info["name"];
		value_a = a->get(property_name);
		value_b = b->get(property_name);
	}

	if (!p_is_ascending) {
		const Variant temp = value_a;
		value_a = value_b;
		value_b = temp;
	}

	if (value_a.get_type() == Variant::STRING && value_b.get_type() == Variant::STRING) {
		return String(value_a).naturalnocasecmp_to(value_b) < 0;
	}
	return value_a < value_b;
}

} // namespace

void ResourceTablesPlugin::_bind_methods() {
}

void ResourceTablesPlugin::_enter_tree() {
	const EditorInterface *const editor_interface = EditorInterface::get_singleton();
	const Control *const editor_base = editor_interface->get_base_control();

	main_panel = memnew(VBoxContainer);
	main_panel->set_name("Resources");
	main_panel->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	main_panel->set_v_size_flags(Control::SIZE_EXPAND_FILL);

	HBoxContainer *const top_row = memnew(HBoxContainer);
	main_panel->add_child(top_row);

	MarginContainer *const type_icon_margin = memnew(MarginContainer);
	type_icon_margin->add_theme_constant_override("margin_left", TYPE_ICON_MARGIN);
	type_icon_margin->add_theme_constant_override("margin_right", TYPE_ICON_MARGIN);
	top_row->add_child(type_icon_margin);

	TextureRect *const type_icon = memnew(TextureRect);
	type_icon->set_texture(editor_base->get_theme_icon("FileList", "EditorIcons"));
	type_icon->set_stretch_mode(TextureRect::STRETCH_KEEP_CENTERED);
	type_icon->set_tooltip_text("Source");
	type_icon_margin->add_child(type_icon);

	type_dropdown = memnew(OptionButton);
	type_dropdown->connect("item_selected", callable_mp(this, &ResourceTablesPlugin::_on_type_selected));
	top_row->add_child(type_dropdown);

	new_resource_button = memnew(Button);
	new_resource_button->set_text("Add");
	new_resource_button->set_button_icon(editor_base->get_theme_icon("Add", "EditorIcons"));
	new_resource_button->set_disabled(true);
	new_resource_button->connect("pressed", callable_mp(this, &ResourceTablesPlugin::_on_new_resource_button_pressed));
	top_row->add_child(new_resource_button);

	Control *const top_row_spacer = memnew(Control);
	top_row_spacer->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	top_row->add_child(top_row_spacer);

	MenuBar *const menu_bar = memnew(MenuBar);
	tools_menu = memnew(PopupMenu);
	tools_menu->set_name("Tools");
	tools_menu->add_icon_item(editor_base->get_theme_icon("ScriptCreate", "EditorIcons"), "New ResourceTable Generator...", TOOLS_MENU_NEW_GENERATOR);
	tools_menu->add_icon_item(editor_base->get_theme_icon("Play", "EditorIcons"), "Run Generators (For Debugging)", TOOLS_MENU_GENERATE_ALL);
	tools_menu->add_separator();
	tools_menu->add_icon_item(editor_base->get_theme_icon("Save", "EditorIcons"), "Export to CSV...", TOOLS_MENU_EXPORT_CSV);
	tools_menu->add_icon_item(editor_base->get_theme_icon("Load", "EditorIcons"), "Import from CSV...", TOOLS_MENU_IMPORT_CSV);
	tools_menu->connect("id_pressed", callable_mp(this, &ResourceTablesPlugin::_on_tools_menu_id_pressed));
	menu_bar->add_child(tools_menu);
	top_row->add_child(menu_bar);

	table = memnew(ResourceTableContainer);
	table->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	table->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	table->connect("delete_requested", callable_mp(this, &ResourceTablesPlugin::_on_delete_requested));
	table->connect("sort_requested", callable_mp(this, &ResourceTablesPlugin::_on_sort_header_pressed));
	table->connect("column_resize_ended", callable_mp(this, &ResourceTablesPlugin::_on_column_resized));
	main_panel->add_child(table);

	csv_dialog = memnew(EditorFileDialog);
	csv_dialog->set_access(EditorFileDialog::ACCESS_FILESYSTEM);
	csv_dialog->add_filter("*.csv", "CSV Files");
	csv_dialog->connect("file_selected", callable_mp(this, &ResourceTablesPlugin::_on_csv_file_selected));
	main_panel->add_child(csv_dialog);

	import_confirm_dialog = memnew(ConfirmationDialog);
	import_confirm_dialog->set_title("Import from CSV");
	import_confirm_dialog->connect("confirmed", callable_mp(this, &ResourceTablesPlugin::_on_import_confirmed));
	main_panel->add_child(import_confirm_dialog);

	delete_confirm_dialog = memnew(ConfirmationDialog);
	delete_confirm_dialog->set_title("Delete Resources");
	delete_confirm_dialog->connect("confirmed", callable_mp(this, &ResourceTablesPlugin::_on_delete_confirmed));
	main_panel->add_child(delete_confirm_dialog);

	new_generator_dialog = memnew(ConfirmationDialog);
	new_generator_dialog->set_title("New ResourceTable Generator");
	GridContainer *const new_generator_grid = memnew(GridContainer);
	new_generator_grid->set_columns(3);

	Label *const new_generator_class_label = memnew(Label);
	new_generator_class_label->set_text("Resource Class:");
	new_generator_grid->add_child(new_generator_class_label);
	new_generator_class_line_edit = memnew(LineEdit);
	new_generator_class_line_edit->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	new_generator_class_line_edit->set_custom_minimum_size(Vector2(300, 0));
	new_generator_grid->add_child(new_generator_class_line_edit);
	Button *const new_generator_class_button = memnew(Button);
	new_generator_class_button->set_button_icon(editor_base->get_theme_icon("ClassList", "EditorIcons"));
	new_generator_class_button->connect("pressed", callable_mp(this, &ResourceTablesPlugin::_on_new_generator_class_browse_pressed));
	new_generator_grid->add_child(new_generator_class_button);

	Label *const new_generator_path_label = memnew(Label);
	new_generator_path_label->set_text("Output Path:");
	new_generator_grid->add_child(new_generator_path_label);
	new_generator_path_line_edit = memnew(LineEdit);
	new_generator_path_line_edit->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	new_generator_grid->add_child(new_generator_path_line_edit);
	Button *const new_generator_path_button = memnew(Button);
	new_generator_path_button->set_button_icon(editor_base->get_theme_icon("FileBrowse", "EditorIcons"));
	new_generator_path_button->connect("pressed", callable_mp(this, &ResourceTablesPlugin::_on_new_generator_path_browse_pressed));
	new_generator_grid->add_child(new_generator_path_button);

	new_generator_dialog->add_child(new_generator_grid);
	new_generator_dialog->connect("confirmed", callable_mp(this, &ResourceTablesPlugin::_on_new_generator_confirmed));
	main_panel->add_child(new_generator_dialog);

	new_generator_file_dialog = memnew(EditorFileDialog);
	new_generator_file_dialog->set_access(EditorFileDialog::ACCESS_RESOURCES);
	new_generator_file_dialog->set_file_mode(EditorFileDialog::FILE_MODE_SAVE_FILE);
	new_generator_file_dialog->add_filter("*.gd", "GDScript Files");
	new_generator_file_dialog->connect("file_selected", callable_mp(this, &ResourceTablesPlugin::_on_new_generator_path_browsed));
	main_panel->add_child(new_generator_file_dialog);

	new_resource_file_dialog = memnew(EditorFileDialog);
	new_resource_file_dialog->set_access(EditorFileDialog::ACCESS_RESOURCES);
	new_resource_file_dialog->set_file_mode(EditorFileDialog::FILE_MODE_SAVE_FILE);
	new_resource_file_dialog->add_filter("*.tres", "Resource Files");
	new_resource_file_dialog->connect("file_selected", callable_mp(this, &ResourceTablesPlugin::_on_new_resource_path_selected));
	main_panel->add_child(new_resource_file_dialog);

	add_control_to_bottom_panel(main_panel, "Resources");

	editor_interface->get_resource_filesystem()->connect("filesystem_changed", callable_mp(this, &ResourceTablesPlugin::_on_filesystem_changed));
	editor_interface->get_inspector()->connect("property_edited", callable_mp(this, &ResourceTablesPlugin::_on_inspector_property_edited));
	// Undo/redo doesn't notify the table, so refresh here.
	get_undo_redo()->connect("version_changed", callable_mp(this, &ResourceTablesPlugin::_on_undo_redo_version_changed));

	export_plugin.instantiate();
	export_plugin->set_generate_callback(callable_mp(this, &ResourceTablesPlugin::_run_export_tables).bind(String()));
	add_export_plugin(export_plugin);
	callable_mp(this, &ResourceTablesPlugin::_sync_generators).call_deferred(false);

	_refresh_type_dropdown();
}

bool ResourceTablesPlugin::_build() {
	_run_export_tables(String());
	return true;
}

void ResourceTablesPlugin::_exit_tree() {
	if (export_plugin.is_valid()) {
		remove_export_plugin(export_plugin);
		export_plugin.unref();
	}
	const EditorInterface *const editor_interface = EditorInterface::get_singleton();
	EditorFileSystem *const filesystem = editor_interface->get_resource_filesystem();
	if (filesystem) {
		filesystem->disconnect("filesystem_changed", callable_mp(this, &ResourceTablesPlugin::_on_filesystem_changed));
	}
	EditorInspector *const inspector = editor_interface->get_inspector();
	if (inspector) {
		inspector->disconnect("property_edited", callable_mp(this, &ResourceTablesPlugin::_on_inspector_property_edited));
	}
	EditorUndoRedoManager *const undo_redo = get_undo_redo();
	if (undo_redo) {
		undo_redo->disconnect("version_changed", callable_mp(this, &ResourceTablesPlugin::_on_undo_redo_version_changed));
	}

	if (main_panel) {
		remove_control_from_bottom_panel(main_panel);
		main_panel->queue_free();
		main_panel = nullptr;
		tools_menu = nullptr;
		type_dropdown = nullptr;
		table = nullptr;
	}
}

void ResourceTablesPlugin::_refresh_type_dropdown() {
	const int selected_index = type_dropdown->get_selected();
	const String previous_text = selected_index >= 0 ? type_dropdown->get_item_text(selected_index) : String();

	type_dropdown->clear();
	const PackedStringArray type_names = ResourceTableUtils::find_resource_type_names();
	for (const String &type_name : type_names) {
		type_dropdown->add_item(type_name);
		type_dropdown->set_item_metadata(type_dropdown->get_item_count() - 1, type_name);
	}

	std::vector<std::pair<String, Variant>> sorted_items;
	for (int i = 0; i < type_dropdown->get_item_count(); i++) {
		sorted_items.emplace_back(type_dropdown->get_item_text(i), type_dropdown->get_item_metadata(i));
	}
	std::stable_sort(sorted_items.begin(), sorted_items.end(), [](const auto &p_a, const auto &p_b) {
		return p_a.first.nocasecmp_to(p_b.first) < 0;
	});
	type_dropdown->clear();
	for (const auto &item : sorted_items) {
		type_dropdown->add_item(item.first);
		type_dropdown->set_item_metadata(type_dropdown->get_item_count() - 1, item.second);
	}

	if (type_dropdown->get_item_count() == 0) {
		current_resource_class_name = StringName();
		current_properties = Array();
		new_resource_button->set_disabled(true);
		_rebuild_table();
		return;
	}

	int index_to_select = 0;
	for (int i = 0; i < type_dropdown->get_item_count(); i++) {
		if (type_dropdown->get_item_text(i) == previous_text) {
			index_to_select = i;
			break;
		}
	}

	type_dropdown->select(index_to_select);
	_apply_type_selection(index_to_select, type_dropdown->get_item_text(index_to_select) != previous_text);
}

void ResourceTablesPlugin::_on_type_selected(int p_index) {
	_apply_type_selection(p_index, true);
}

void ResourceTablesPlugin::_apply_type_selection(int p_index, bool p_is_sort_reset) {
	if (p_is_sort_reset) {
		sort_column = 0;
		is_sort_ascending = true;
	}
	current_resource_class_name = StringName(String(type_dropdown->get_item_metadata(p_index)));
	new_resource_button->set_disabled(false);
	_rebuild_table();
}

void ResourceTablesPlugin::_rebuild_table() {
	// Rebuilds everything; must not run mid-drag or it destroys the dragged widget.
	table->clear();
	current_row_resources = Array();

	if (current_resource_class_name == StringName()) {
		table->set_columns(1, {});
		current_properties = Array();
		return;
	}

	current_properties = ResourceTableUtils::find_properties_of_type(current_resource_class_name);
	const int column_count = 2 + current_properties.size();
	std::vector<float> default_widths;
	default_widths.push_back(HEADER_COLUMN_WIDTH);
	table->set_columns(column_count, default_widths);

	table->set_column_header_text(0, "");

	table->set_column_header_text(1, "Name");
	table->set_column_sort_direction(1, sort_column == 0 ? (is_sort_ascending ? ResourceTableContainer::SORT_ASCENDING : ResourceTableContainer::SORT_DESCENDING) : ResourceTableContainer::SORT_NONE);
	_apply_saved_column_width(1);

	for (int i = 0; i < current_properties.size(); i++) {
		const Dictionary info = current_properties[i];
		// Approximates the Inspector's label capitalization (EditorPropertyNameProcessor isn't exposed).
		const String title = String(info["name"]).capitalize();
		table->set_column_header_text(2 + i, title);
		table->set_column_sort_direction(2 + i, sort_column == 1 + i ? (is_sort_ascending ? ResourceTableContainer::SORT_ASCENDING : ResourceTableContainer::SORT_DESCENDING) : ResourceTableContainer::SORT_NONE);
		_apply_saved_column_width(2 + i);
	}

	Array resources = ResourceTableUtils::find_resources_of_type(current_resource_class_name);

	if (sort_column >= 0 && sort_column <= current_properties.size()) {
		resources.sort_custom(callable_mp_static(&is_before_by_sort_column).bind(sort_column, current_properties, is_sort_ascending));
	}

	const Control *const editor_base = EditorInterface::get_singleton()->get_base_control();

	for (int i = 0; i < resources.size(); i++) {
		const Ref<Resource> resource = resources[i];
		if (resource.is_null()) {
			continue;
		}
		current_row_resources.push_back(resource);
		const int row = table->add_row();

		// Always built with an icon (hidden when not dirty) so the revert column's width doesn't jitter.
		const Ref<Texture2D> revert_icon = editor_base->get_theme_icon("ReloadSmall", "EditorIcons");
		Button *const revert_button = memnew(Button);
		revert_button->set_flat(true);
		revert_button->set_button_icon(revert_icon);
		const bool is_dirty = dirty_resource_mtimes.has(resource->get_path());
		revert_button->set_visible(is_dirty);
		if (is_dirty) {
			revert_button->set_tooltip_text("Revert (reload from disk)");
			revert_button->connect("pressed", callable_mp(this, &ResourceTablesPlugin::_on_revert_button_pressed).bind(resource));
		}
		table->set_cell(row, 0, revert_button);

		ResourceTableName *const name_cell = memnew(ResourceTableName);
		name_cell->set_text(resource->get_path().get_file().get_basename());
		name_cell->set_resource(resource);
		name_cell->connect("delete_requested", callable_mp(this, &ResourceTablesPlugin::_on_delete_requested));
		table->set_cell(row, 1, name_cell);

		for (int j = 0; j < current_properties.size(); j++) {
			const Dictionary info = current_properties[j];
			const StringName property_name = info["name"];
			const Variant::Type type = (Variant::Type)(int64_t)info["type"];
			const PropertyHint hint = (PropertyHint)(int64_t)info["hint"];
			const String hint_string = info["hint_string"];
			const uint32_t usage = (uint32_t)(int64_t)info["usage"];

			EditorProperty *const editor = EditorInspector::instantiate_property_editor(resource.ptr(), type, property_name, hint, hint_string, usage, false);
			if (!editor) {
				Label *const fallback = memnew(Label);
				fallback->set_text(resource->get(property_name).stringify());
				table->set_cell(row, 2 + j, fallback);
				continue;
			}

			editor->set_object_and_property(resource.ptr(), property_name);
			editor->set_draw_label(false);
			editor->update_property();
			editor->connect("property_changed", callable_mp(this, &ResourceTablesPlugin::_on_property_editor_changed).bind(resource));
			table->set_cell(row, 2 + j, editor);
		}
	}
}

void ResourceTablesPlugin::_apply_saved_column_width(int p_column) {
	const String property_name = column_property_name(current_properties, p_column);
	if (property_name.is_empty()) {
		return;
	}
	// -1 rather than Variant(): ConfigFile::get_value logs an error for a NIL default on a missing key.
	const int saved = EditorInterface::get_singleton()->get_editor_settings()->get_project_metadata(current_resource_class_name, property_name, -1);
	if (saved >= 0) {
		table->set_column_width(p_column, saved);
	}
}

void ResourceTablesPlugin::_on_column_resized(int p_column, int p_width) {
	const String property_name = column_property_name(current_properties, p_column);
	if (property_name.is_empty()) {
		return;
	}
	EditorInterface::get_singleton()->get_editor_settings()->set_project_metadata(current_resource_class_name, property_name, p_width);
}

// p_table_column is the container's index (0 revert, 1 Name, 2+i properties), not sort_column's.
void ResourceTablesPlugin::_on_sort_header_pressed(int p_table_column) {
	if (p_table_column == 0) {
		return;
	}
	const int clicked_sort_column = p_table_column - 1;
	if (sort_column == clicked_sort_column) {
		is_sort_ascending = !is_sort_ascending;
	} else {
		sort_column = clicked_sort_column;
		is_sort_ascending = true;
	}
	_rebuild_table();
}

// Undo/redo-backed edit; skipped if unchanged. p_old_value is explicit because live ticks already
// set the resource to the new value.
void ResourceTablesPlugin::_apply_property_edit(const Ref<Resource> &p_resource, const StringName &p_property_name, const Variant &p_old_value, const Variant &p_new_value) {
	if (p_old_value == p_new_value) {
		return;
	}

	EditorUndoRedoManager *const undo_redo = get_undo_redo();
	Resource *const resource = p_resource.ptr();
	undo_redo->create_action("Set " + String(p_property_name));
	undo_redo->add_do_property(resource, p_property_name, p_new_value);
	undo_redo->add_undo_property(resource, p_property_name, p_old_value);
	undo_redo->commit_action();

	_mark_resource_dirty(p_resource);
}

// See property_edit_session_resource: p_is_changing alone can't signal a commit; _process decides.
void ResourceTablesPlugin::_on_property_editor_changed(StringName p_property, Variant p_value, StringName p_field, bool p_is_changing, Ref<Resource> p_resource) {
	const bool is_new_session = property_edit_session_resource != p_resource || property_edit_session_property != p_property;
	if (is_new_session) {
		property_edit_session_resource = p_resource;
		property_edit_session_property = p_property;
		property_edit_session_old_value = p_resource->get(p_property);
		set_process(true);
	}

	is_property_edit_session_changing = p_is_changing;
	p_resource->set(p_property, p_value);
}

void ResourceTablesPlugin::_process(double p_delta) {
	if (property_edit_session_resource.is_null()) {
		set_process(false); // Shouldn't happen (see _finish_property_edit_session), but don't spin forever if it does.
		return;
	}

	// Still going if the last tick reported changing or the left button is held (spin sliders never
	// report changing, and emit nothing on release -- hence the poll).
	if (is_property_edit_session_changing || DisplayServer::get_singleton()->mouse_get_button_state().has_flag(MOUSE_BUTTON_MASK_LEFT)) {
		return;
	}

	_finish_property_edit_session();
}

void ResourceTablesPlugin::_finish_property_edit_session() {
	const Ref<Resource> resource = property_edit_session_resource;
	const StringName property = property_edit_session_property;
	const Variant old_value = property_edit_session_old_value;

	property_edit_session_resource = Ref<Resource>();
	set_process(false);

	_apply_property_edit(resource, property, old_value, resource->get(property));

	callable_mp(this, &ResourceTablesPlugin::_rebuild_table).call_deferred();
}

void ResourceTablesPlugin::_on_inspector_property_edited(String p_property) {
	Object *const edited_object = EditorInterface::get_singleton()->get_inspector()->get_edited_object();
	const Ref<Resource> resource = Object::cast_to<Resource>(edited_object);
	if (resource.is_null()) {
		return;
	}

	const Ref<Script> script = resource->get_script();
	const StringName resource_type_name = script.is_valid() ? script->get_global_name() : StringName(resource->get_class());
	if (resource_type_name != current_resource_class_name) {
		return;
	}

	_mark_resource_dirty(resource);

	// Deferred: this can fire from within the Inspector's own signal
	// dispatch, and a synchronous rebuild there is asking for trouble.
	callable_mp(this, &ResourceTablesPlugin::_rebuild_table).call_deferred();
}

void ResourceTablesPlugin::_on_delete_requested() {
	const PackedInt32Array selected = table->get_selected_rows();
	if (selected.is_empty()) {
		return;
	}
	String text;
	if (selected.size() == 1) {
		const Ref<Resource> resource = current_row_resources[selected[0]];
		text = "Delete '" + resource->get_path() + "'?";
	} else {
		text = "Delete " + String::num_int64(selected.size()) + " resources?";
	}
	delete_confirm_dialog->set_text(text + "\nThis cannot be undone.");
	delete_confirm_dialog->popup_centered();
}

void ResourceTablesPlugin::_on_delete_confirmed() {
	const PackedInt32Array selected = table->get_selected_rows();
	for (const int row : selected) {
		if (row < 0 || row >= current_row_resources.size()) {
			continue;
		}
		const Ref<Resource> resource = current_row_resources[row];
		if (resource.is_null()) {
			continue;
		}
		String path = resource->get_path();
		dirty_resource_mtimes.erase(path);
		DirAccess::remove_absolute(path);
		if (FileAccess::file_exists(path + ".uid")) {
			DirAccess::remove_absolute(path + ".uid");
		}
	}
	table->clear_selection();
	EditorInterface::get_singleton()->get_resource_filesystem()->scan();
}

void ResourceTablesPlugin::_on_filesystem_changed() {
	callable_mp(this, &ResourceTablesPlugin::_check_dirty_resources_saved).call_deferred();
	callable_mp(this, &ResourceTablesPlugin::_sync_generators).call_deferred(true);
	callable_mp(this, &ResourceTablesPlugin::_refresh_type_dropdown).call_deferred();
}

void ResourceTablesPlugin::_on_undo_redo_version_changed() {
	// Fires for every undo/redo action in the editor, not just ones
	// touching a currently-displayed resource -- rebuilding is cheap enough
	// that filtering which action this was isn't worth the complexity.
	callable_mp(this, &ResourceTablesPlugin::_rebuild_table).call_deferred();
}

void ResourceTablesPlugin::_on_tools_menu_id_pressed(int p_id) {
	switch (p_id) {
		case TOOLS_MENU_NEW_GENERATOR: {
			_on_new_generator_button_pressed();
			break;
		}
		case TOOLS_MENU_GENERATE_ALL: {
			_run_export_tables(String());
			_rebuild_table();
			break;
		}
		case TOOLS_MENU_EXPORT_CSV: {
			is_csv_dialog_export = true;
			csv_dialog->set_file_mode(EditorFileDialog::FILE_MODE_SAVE_FILE);
			csv_dialog->set_title("Export to CSV");
			csv_dialog->set_current_file(String(current_resource_class_name) + ".csv");
			csv_dialog->popup_centered_ratio(0.5);
			break;
		}
		case TOOLS_MENU_IMPORT_CSV: {
			is_csv_dialog_export = false;
			csv_dialog->set_file_mode(EditorFileDialog::FILE_MODE_OPEN_FILE);
			csv_dialog->set_title("Import from CSV");
			csv_dialog->popup_centered_ratio(0.5);
			break;
		}
	}
}

void ResourceTablesPlugin::_on_new_generator_button_pressed() {
	new_generator_class_line_edit->set_text("");
	new_generator_path_line_edit->set_text(DEFAULT_NEW_GENERATOR_PATH);
	new_generator_dialog->popup_centered();
	new_generator_class_line_edit->grab_focus();
}

void ResourceTablesPlugin::_on_new_generator_class_browse_pressed() {
	EditorInterface::get_singleton()->popup_create_dialog(
			callable_mp(this, &ResourceTablesPlugin::_on_new_generator_class_picked),
			"Resource", new_generator_class_line_edit->get_text().strip_edges(), "Select Resource Class");
}

void ResourceTablesPlugin::_on_new_generator_class_picked(String p_class_name) {
	if (p_class_name.is_empty()) {
		return;
	}
	// The create dialog reports a script class as its script's path, not its name.
	if (p_class_name.begins_with("res://")) {
		const Ref<Script> script = ResourceLoader::get_singleton()->load(p_class_name);
		const StringName global_name = script.is_valid() ? script->get_global_name() : StringName();
		p_class_name = global_name != StringName() ? String(global_name) : p_class_name.get_file().get_basename();
	}
	new_generator_class_line_edit->set_text(p_class_name);
}

void ResourceTablesPlugin::_on_new_generator_path_browse_pressed() {
	const String path = new_generator_path_line_edit->get_text().strip_edges();
	new_generator_file_dialog->set_current_path(path.is_empty() ? String(DEFAULT_NEW_GENERATOR_PATH) : path);
	new_generator_file_dialog->popup_centered_ratio(0.5);
}

void ResourceTablesPlugin::_on_new_generator_path_browsed(String p_path) {
	new_generator_path_line_edit->set_text(p_path);
}

void ResourceTablesPlugin::_on_new_generator_confirmed() {
	const String resource_class_name = new_generator_class_line_edit->get_text().strip_edges();
	String path = new_generator_path_line_edit->get_text().strip_edges();
	if (resource_class_name.is_empty() || path.is_empty()) {
		return;
	}
	if (path.get_extension().is_empty()) {
		path += ".gd";
	}
	create_generator_script(resource_class_name, path);
}

void ResourceTablesPlugin::_on_csv_file_selected(String p_path) {
	if (is_csv_dialog_export) {
		ResourceTableUtils::export_csv(current_resource_class_name, find_visible_resource_paths(current_row_resources), p_path);
		return;
	}

	const PackedStringArray visible_paths = find_visible_resource_paths(current_row_resources);
	const ResourceTableUtils::ImportPreview preview = ResourceTableUtils::preview_import_csv(current_resource_class_name, visible_paths, p_path);
	if (preview.error != OK) {
		return;
	}

	const int adds = preview.adds;
	const int updates = preview.updates;
	const int deletes = preview.deletes;
	if (adds == 0 && updates == 0 && deletes == 0) {
		return;
	}

	pending_import_csv_path = p_path;
	pending_import_resource_paths = visible_paths;
	import_confirm_dialog->set_text(
			"Importing this CSV will:\n"
			"  " +
			String::num_int64(adds) + " resource(s) added\n" +
			"  " + String::num_int64(updates) + " resource(s) updated\n" +
			"  " + String::num_int64(deletes) + " resource(s) deleted\n\n"
												"This cannot be undone. Continue?");
	import_confirm_dialog->popup_centered();
}

void ResourceTablesPlugin::_on_import_confirmed() {
	ResourceTableUtils::import_csv(current_resource_class_name, pending_import_resource_paths, pending_import_csv_path);
	_rebuild_table();
}

void ResourceTablesPlugin::_on_new_resource_button_pressed() {
	if (current_resource_class_name == StringName()) {
		return;
	}

	const String saved_dir = EditorInterface::get_singleton()->get_editor_settings()->get_project_metadata(current_resource_class_name, NEW_RESOURCE_DIR_KEY, String());
	if (!saved_dir.is_empty()) {
		new_resource_file_dialog->set_current_dir(saved_dir);
	}
	new_resource_file_dialog->set_current_file(find_unused_resource_name(new_resource_file_dialog->get_current_dir(), current_resource_class_name));
	new_resource_file_dialog->popup_centered_ratio(0.5);
}

void ResourceTablesPlugin::_on_new_resource_path_selected(String p_path) {
	if (current_resource_class_name == StringName()) {
		return;
	}

	EditorInterface::get_singleton()->get_editor_settings()->set_project_metadata(current_resource_class_name, NEW_RESOURCE_DIR_KEY, p_path.get_base_dir());

	const Ref<Resource> resource = ResourceTableUtils::instantiate_resource_of_type(current_resource_class_name);
	if (resource.is_null()) {
		return;
	}

	const Error save_err = ResourceSaver::get_singleton()->save(resource, p_path);
	if (save_err != OK) {
		UtilityFunctions::push_error("ResourceTablesPlugin: failed to save '", p_path, "' (error ", (int64_t)save_err, ").");
		return;
	}

	EditorInterface *const editor_interface = EditorInterface::get_singleton();
	editor_interface->get_resource_filesystem()->scan();
	editor_interface->edit_resource(resource);
}

void ResourceTablesPlugin::_run_export_tables(const String &p_modified_resource_class) {
	if (is_generating) {
		return;
	}
	const Ref<Script> script = ResourceLoader::get_singleton()->load(EXPORT_RESOURCE_TABLES_SCRIPT_PATH);
	if (script.is_null()) {
		return;
	}

	is_generating = true;
	script->call("export_tables", p_modified_resource_class);
	is_generating = false;
}

void ResourceTablesPlugin::_sync_generators(bool p_should_generate) {
	if (is_generating) {
		return;
	}
	is_generating = true;

	PackedStringArray modified_classes;
	const Array scripts = ResourceTableUtils::find_generator_scripts();
	for (int i = 0; i < scripts.size(); i++) {
		const Ref<Script> script = scripts[i];
		if (script.is_null() || !script->can_instantiate()) {
			continue;
		}
		const Ref<RefCounted> generator = script->call("new");
		if (generator.is_null()) {
			continue;
		}

		const String class_name = generator->get("_resource_class_name");
		Dictionary snapshot;
		const Array resources = ResourceTableUtils::find_resources_of_type(class_name);
		for (int j = 0; j < resources.size(); j++) {
			const String path = Ref<Resource>(resources[j])->get_path();
			snapshot[path] = (int64_t)FileAccess::get_modified_time(path);
		}

		const String key = script->get_path();
		const bool is_changed = !generator_snapshots.has(key) || Dictionary(generator_snapshots[key]) != snapshot;
		generator_snapshots[key] = snapshot;
		if (is_changed && p_should_generate) {
			modified_classes.push_back(class_name);
		}
	}
	is_generating = false;

	for (const String &class_name : modified_classes) {
		_run_export_tables(class_name);
	}
}

void ResourceTablesPlugin::_mark_resource_dirty(const Ref<Resource> &p_resource) {
	const String path = p_resource->get_path();
	if (path.is_empty() || dirty_resource_mtimes.has(path)) {
		return;
	}
	dirty_resource_mtimes[path] = FileAccess::get_modified_time(path);
}

void ResourceTablesPlugin::_check_dirty_resources_saved() {
	if (dirty_resource_mtimes.is_empty()) {
		return;
	}
	PackedStringArray saved_paths;
	for (const KeyValue<String, uint64_t> &entry : dirty_resource_mtimes) {
		if (FileAccess::get_modified_time(entry.key) != entry.value) {
			saved_paths.push_back(entry.key);
		}
	}
	for (int i = 0; i < saved_paths.size(); i++) {
		dirty_resource_mtimes.erase(saved_paths[i]);
	}
}

void ResourceTablesPlugin::_on_revert_button_pressed(Ref<Resource> p_resource) {
	const String path = p_resource->get_path();
	dirty_resource_mtimes.erase(path);

	// Not CACHE_MODE_REPLACE: the .tres saver omits default-valued properties, so a replace leaves
	// a since-edited value in place. Load an independent copy and copy every displayed property across.
	const Ref<Resource> fresh = ResourceLoader::get_singleton()->load(path, "", ResourceLoader::CACHE_MODE_IGNORE);
	if (fresh.is_valid()) {
		for (int i = 0; i < current_properties.size(); i++) {
			const Dictionary info = current_properties[i];
			const StringName property_name = info["name"];
			p_resource->set(property_name, fresh->get(property_name));
		}
	}
	_rebuild_table();
}
