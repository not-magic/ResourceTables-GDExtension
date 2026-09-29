#include "resource_tables_plugin.h"

#include "resource_table_name.h"
#include "resource_table_utils.h"
#include "resource_table_container.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_inspector.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_property.hpp>
#include <godot_cpp/classes/editor_settings.hpp>
#include <godot_cpp/classes/editor_script.hpp>
#include <godot_cpp/classes/editor_undo_redo_manager.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/texture_rect.hpp>
#include <godot_cpp/classes/file_system_dock.hpp>
#include <godot_cpp/classes/grid_container.hpp>
#include <godot_cpp/classes/h_box_container.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/menu_bar.hpp>
#include <godot_cpp/classes/popup_menu.hpp>
#include <godot_cpp/classes/popup_panel.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/resource_saver.hpp>
#include <godot_cpp/classes/resource_uid.hpp>
#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/classes/texture2d.hpp>
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

// Table column 0 is a narrow, title-less revert-to-disk icon column (a
// plain Button, built fresh per row -- see _rebuild_table); table column 1
// is Name (ResourceTableName); each property is table column 2+i.
// sort_column's own numbering (0 == Name, 1+i == properties[i]) is offset
// by one from table column indices as a result -- see
// _on_sort_header_pressed, which is the one place that conversion happens.

// Every column (revert, Name, and each property) starts at this width --
// ResourceTableContainer itself is what keeps a column's width persistent across
// _rebuild_table's repeated clear()+set_columns() calls once the user
// drag-resizes it (see ResourceTableContainer::set_columns' own comment), so this
// is only ever consulted the first time a given column index appears.
constexpr float HEADER_COLUMN_WIDTH = 200.0;

// The addon's own bulk-regeneration script -- see its own doc comment for
// what it does. Loaded and run programmatically here (both for the
// "Generate All" menu action and automatically when a resource is
// added/removed) so that single script stays the one place this logic
// lives, whether it's triggered by hand (File > Run in the Script Editor)
// or from here.
const char *EXPORT_RESOURCE_TABLES_SCRIPT_PATH = "res://addons/ResourceTables/export_resource_tables.gd";

// "New ResourceTable..." writes a new .gd file by substituting
// ${RESOURCE_CLASS_NAME}/${RESOURCE_TABLE_DIR}/${RESOURCE_TABLE_NAME} into a
// copy of this file -- see _on_new_generator_path_selected.
const char *DEFAULT_NEW_GENERATOR_PATH = "res://NewTable.gd";
const char *NEW_GENERATOR_TEMPLATE_PATH = "res://addons/ResourceTables/new_resource_table_generator_template.txt";

} // namespace

void ResourceTablesPlugin::_bind_methods() {
}

void ResourceTablesPlugin::_enter_tree() {
	main_panel = memnew(VBoxContainer);
	main_panel->set_name("Resources");
	main_panel->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	main_panel->set_v_size_flags(Control::SIZE_EXPAND_FILL);

	HBoxContainer *top_row = memnew(HBoxContainer);
	main_panel->add_child(top_row);

	TextureRect *type_icon = memnew(TextureRect);
	type_icon->set_texture(EditorInterface::get_singleton()->get_base_control()->get_theme_icon("Object", "EditorIcons"));
	type_icon->set_stretch_mode(TextureRect::STRETCH_KEEP_CENTERED);
	type_icon->set_tooltip_text("Source");
	top_row->add_child(type_icon);

	type_dropdown = memnew(OptionButton);
	type_dropdown->connect("item_selected", callable_mp(this, &ResourceTablesPlugin::_on_type_selected));
	top_row->add_child(type_dropdown);

	// Creates a new resource of whatever type dropdown 1 currently has
	// selected -- disabled/re-enabled alongside current_resource_class_name
	// (see _refresh_type_dropdown/_on_type_selected).
	new_resource_button = memnew(Button);
	new_resource_button->set_text("Add");
	new_resource_button->set_button_icon(EditorInterface::get_singleton()->get_base_control()->get_theme_icon("Add", "EditorIcons"));
	new_resource_button->set_disabled(true);
	new_resource_button->connect("pressed", callable_mp(this, &ResourceTablesPlugin::_on_new_resource_button_pressed));
	top_row->add_child(new_resource_button);

	// Absorbs the row's leftover width so the labels/dropdowns/buttons stay
	// compacted to their natural size on the left, with the Tools menu
	// pinned to the right edge instead of stretching across the whole panel.
	Control *top_row_spacer = memnew(Control);
	top_row_spacer->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	top_row->add_child(top_row_spacer);

	MenuBar *menu_bar = memnew(MenuBar);
	tools_menu = memnew(PopupMenu);
	tools_menu->set_name("Tools");
	tools_menu->add_icon_item(EditorInterface::get_singleton()->get_base_control()->get_theme_icon("ScriptCreate", "EditorIcons"), "New ResourceTable Generator...", TOOLS_MENU_NEW_GENERATOR);
	tools_menu->add_icon_item(EditorInterface::get_singleton()->get_base_control()->get_theme_icon("Play", "EditorIcons"), "Run Generators", TOOLS_MENU_GENERATE_ALL);
	tools_menu->add_separator();
	tools_menu->add_icon_item(EditorInterface::get_singleton()->get_base_control()->get_theme_icon("Save", "EditorIcons"), "Export to CSV...", TOOLS_MENU_EXPORT_CSV);
	tools_menu->add_icon_item(EditorInterface::get_singleton()->get_base_control()->get_theme_icon("Load", "EditorIcons"), "Import from CSV...", TOOLS_MENU_IMPORT_CSV);
	tools_menu->connect("id_pressed", callable_mp(this, &ResourceTablesPlugin::_on_tools_menu_id_pressed));
	menu_bar->add_child(tools_menu);
	top_row->add_child(menu_bar);

	// The data area -- see src/resource_table_container.h.
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
	GridContainer *new_generator_grid = memnew(GridContainer);
	new_generator_grid->set_columns(3);
	Control *base_control = EditorInterface::get_singleton()->get_base_control();

	Label *new_generator_class_label = memnew(Label);
	new_generator_class_label->set_text("Resource Class:");
	new_generator_grid->add_child(new_generator_class_label);
	new_generator_class_line_edit = memnew(LineEdit);
	new_generator_class_line_edit->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	new_generator_class_line_edit->set_custom_minimum_size(Vector2(300, 0));
	new_generator_grid->add_child(new_generator_class_line_edit);
	Button *new_generator_class_button = memnew(Button);
	new_generator_class_button->set_button_icon(base_control->get_theme_icon("ClassList", "EditorIcons"));
	new_generator_class_button->connect("pressed", callable_mp(this, &ResourceTablesPlugin::_on_new_generator_class_browse_pressed));
	new_generator_grid->add_child(new_generator_class_button);

	Label *new_generator_path_label = memnew(Label);
	new_generator_path_label->set_text("Output Path:");
	new_generator_grid->add_child(new_generator_path_label);
	new_generator_path_line_edit = memnew(LineEdit);
	new_generator_path_line_edit->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	new_generator_grid->add_child(new_generator_path_line_edit);
	Button *new_generator_path_button = memnew(Button);
	new_generator_path_button->set_button_icon(base_control->get_theme_icon("FileBrowse", "EditorIcons"));
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

	// Where to save a brand new resource of the currently selected type
	// (see new_resource_button above).
	new_resource_file_dialog = memnew(EditorFileDialog);
	new_resource_file_dialog->set_access(EditorFileDialog::ACCESS_RESOURCES);
	new_resource_file_dialog->set_file_mode(EditorFileDialog::FILE_MODE_SAVE_FILE);
	new_resource_file_dialog->add_filter("*.tres", "Resource Files");
	new_resource_file_dialog->connect("file_selected", callable_mp(this, &ResourceTablesPlugin::_on_new_resource_path_selected));
	main_panel->add_child(new_resource_file_dialog);

	add_control_to_bottom_panel(main_panel, "Resources");

	// Catches resources being added, removed, or externally modified on disk.
	EditorInterface::get_singleton()->get_resource_filesystem()->connect("filesystem_changed", callable_mp(this, &ResourceTablesPlugin::_on_filesystem_changed));
	// Catches in-place edits made through the regular property editor,
	// whatever object happens to be selected there.
	EditorInterface::get_singleton()->get_inspector()->connect("property_edited", callable_mp(this, &ResourceTablesPlugin::_on_inspector_property_edited));
	// Embedded property-cell edits go through the editor's undo/redo
	// manager (see _apply_property_edit) rather than mutating a resource
	// directly, so an undo/redo needs its own notice to refresh whatever
	// cell it touched -- nothing else tells the table its displayed value
	// just went stale.
	get_undo_redo()->connect("version_changed", callable_mp(this, &ResourceTablesPlugin::_on_undo_redo_version_changed));

	_refresh_type_dropdown();
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
	EditorUndoRedoManager *undo_redo = get_undo_redo();
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

// Each item's metadata is its resource class name; a selection is tracked
// across a refresh by item text.
void ResourceTablesPlugin::_refresh_type_dropdown() {
	String previous_text = type_dropdown->get_selected() >= 0 ? type_dropdown->get_item_text(type_dropdown->get_selected()) : String();

	type_dropdown->clear();
	PackedStringArray type_names = ResourceTableUtils::find_resource_type_names_with_instances();
	for (const String &type_name : type_names) {
		type_dropdown->add_item(type_name);
		type_dropdown->set_item_metadata(type_dropdown->get_item_count() - 1, type_name);
	}

	std::vector<std::pair<String, Variant>> sorted_items;
	for (int i = 0; i < type_dropdown->get_item_count(); i++) {
		sorted_items.emplace_back(type_dropdown->get_item_text(i), type_dropdown->get_item_metadata(i));
	}
	std::stable_sort(sorted_items.begin(), sorted_items.end(), [](const auto &a, const auto &b) {
		return a.first.nocasecmp_to(b.first) < 0;
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

void ResourceTablesPlugin::_apply_type_selection(int p_index, bool p_reset_sort) {
	if (p_reset_sort) {
		sort_column = 0; // Default sort (Name, ascending) for a newly selected entry.
		sort_ascending = true;
	}
	current_resource_class_name = StringName(String(type_dropdown->get_item_metadata(p_index)));
	new_resource_button->set_disabled(false);
	_rebuild_table();
}

void ResourceTablesPlugin::_rebuild_table() {
	// Rebuild-everything on every call -- _on_property_editor_changed is
	// careful to only trigger this once a continuous edit (e.g. a slider
	// drag) is actually over, since this would otherwise destroy the very
	// widget being interacted with mid-drag.
	table->clear();
	current_row_resources = Array();

	if (current_resource_class_name == StringName()) {
		table->set_columns(1, {});
		current_properties = Array();
		return;
	}

	current_properties = ResourceTableUtils::find_properties_of_type(current_resource_class_name);
	int column_count = 2 + current_properties.size();
	std::vector<float> default_widths;
	default_widths.push_back(HEADER_COLUMN_WIDTH); // Reused for every column -- see set_columns' own doc comment.
	table->set_columns(column_count, default_widths);

	table->set_column_header_text(0, ""); // The revert icon column has no title of its own.

	table->set_column_header_text(1, "Name");
	table->set_column_sort_direction(1, sort_column == 0 ? (sort_ascending ? ResourceTableContainer::SORT_ASCENDING : ResourceTableContainer::SORT_DESCENDING) : ResourceTableContainer::SORT_NONE);
	_apply_saved_column_width(1);

	for (int i = 0; i < current_properties.size(); i++) {
		Dictionary info = current_properties[i];
		// The same human-readable capitalization the Inspector uses for a
		// property's label (e.g. "int_val" -> "Int Val") -- the real
		// EditorPropertyNameProcessor the Inspector itself calls isn't
		// exposed to GDExtension, but String::capitalize() implements the
		// same underlying split-on-underscore-and-titlecase transform
		// (see EditorPropertyNameProcessor::_capitalize_name), just without
		// its acronym remap table (e.g. "id" -> "ID") and stop-word
		// lowercasing -- close enough for ordinary property names.
		String title = String(info["name"]).capitalize();
		table->set_column_header_text(2 + i, title);
		table->set_column_sort_direction(2 + i, sort_column == 1 + i ? (sort_ascending ? ResourceTableContainer::SORT_ASCENDING : ResourceTableContainer::SORT_DESCENDING) : ResourceTableContainer::SORT_NONE);
		_apply_saved_column_width(2 + i);
	}

	Array resources = ResourceTableUtils::find_resources_of_type(current_resource_class_name);

	if (sort_column >= 0 && sort_column <= current_properties.size()) {
		resources.sort_custom(callable_mp(this, &ResourceTablesPlugin::_compare_by_sort_column));
	}

	Control *editor_base = EditorInterface::get_singleton()->get_base_control();

	for (int i = 0; i < resources.size(); i++) {
		Ref<Resource> resource = resources[i];
		if (resource.is_null()) {
			continue;
		}
		current_row_resources.push_back(resource);
		int row = table->add_row();

		// Only shown for a resource with unsaved edits (dirty_resource_mtimes,
		// see _mark_resource_dirty), but always built with its icon assigned
		// even when hidden -- a not-dirty row still needs a real Button with a
		// real icon so its own get_combined_minimum_size() includes the same
		// stylebox padding a dirty row's does, so the auto-fit revert column
		// doesn't jitter width the moment a row's dirty state flips.
		Ref<Texture2D> revert_icon = editor_base->get_theme_icon("ReloadSmall", "EditorIcons");
		Button *revert_button = memnew(Button);
		revert_button->set_flat(true);
		revert_button->set_button_icon(revert_icon);
		bool dirty = dirty_resource_mtimes.has(resource->get_path());
		revert_button->set_visible(dirty);
		if (dirty) {
			revert_button->set_tooltip_text("Revert (reload from disk)");
			revert_button->connect("pressed", callable_mp(this, &ResourceTablesPlugin::_on_revert_button_pressed).bind(resource));
		}
		table->set_cell(row, 0, revert_button);

		ResourceTableName *name_cell = memnew(ResourceTableName);
		name_cell->set_text(resource->get_path().get_file().get_basename());
		name_cell->set_resource(resource);
		name_cell->connect("delete_requested", callable_mp(this, &ResourceTablesPlugin::_on_delete_requested));
		table->set_cell(row, 1, name_cell);

		// Every property cell is a live EditorProperty -- the same widget
		// class the Inspector itself would use for this exact property
		// (see EditorInspector::instantiate_property_editor), genuinely a
		// child of the table rather than something summoned on demand, so
		// every Variant type (bool, enum, Color, whatever else) is correct
		// automatically instead of this file hand-rolling one per type.
		for (int j = 0; j < current_properties.size(); j++) {
			Dictionary info = current_properties[j];
			StringName property_name = info["name"];
			Variant::Type type = (Variant::Type)(int64_t)info["type"];
			PropertyHint hint = (PropertyHint)(int64_t)info["hint"];
			String hint_string = info["hint_string"];
			uint32_t usage = (uint32_t)(int64_t)info["usage"];

			EditorProperty *editor = EditorInspector::instantiate_property_editor(resource.ptr(), type, property_name, hint, hint_string, usage, false);
			if (!editor) {
				// No editor for this type/hint -- fall back to a plain display.
				Label *fallback = memnew(Label);
				fallback->set_text(resource->get(property_name).stringify());
				table->set_cell(row, 2 + j, fallback);
				continue;
			}

			editor->set_object_and_property(resource.ptr(), property_name);
			editor->set_draw_label(false); // The column header already names this property.
			editor->update_property();
			editor->connect("property_changed", callable_mp(this, &ResourceTablesPlugin::_on_property_editor_changed).bind(resource));
			table->set_cell(row, 2 + j, editor);
		}
	}
}

// Column 0 is the revert-icon column (no property of its own, not worth
// remembering a width for); column 1 is Name; column 2+i is
// current_properties[i]'s own export var name (not its capitalized title,
// so a rename of the display label alone doesn't orphan a saved width).
String ResourceTablesPlugin::_column_property_name(int p_column) const {
	if (p_column == 1) {
		return "Name";
	}
	if (p_column >= 2 && p_column - 2 < current_properties.size()) {
		Dictionary info = current_properties[p_column - 2];
		return info["name"];
	}
	return String();
}

// Column widths are remembered per resource type + property, in the
// project's own EditorSettings metadata (EditorSettings::set/get_project_metadata
// -- the same per-project, not-committed-to-VCS store the editor's own dock
// layouts/inspector folding use), keyed by (current_resource_class_name,
// property name) so switching between types never mixes up one type's own
// remembered width with another's column at the same index.
void ResourceTablesPlugin::_apply_saved_column_width(int p_column) {
	String property_name = _column_property_name(p_column);
	if (property_name.is_empty()) {
		return;
	}
	// -1, not Variant() -- ConfigFile::get_value (which EditorSettings'
	// project metadata is backed by) logs an ERROR whenever the key is
	// missing and the default passed is specifically a NIL Variant, on the
	// assumption that means "no default was given" rather than "this is the
	// sentinel for not-found" (see core/io/config_file.cpp). A column width
	// is never negative, so -1 is a safe, silent "not saved yet" sentinel.
	int saved = EditorInterface::get_singleton()->get_editor_settings()->get_project_metadata(current_resource_class_name, property_name, -1);
	if (saved >= 0) {
		table->set_column_width(p_column, saved);
	}
}

void ResourceTablesPlugin::_on_column_resized(int p_column, int p_width) {
	String property_name = _column_property_name(p_column);
	if (property_name.is_empty()) {
		return;
	}
	EditorInterface::get_singleton()->get_editor_settings()->set_project_metadata(current_resource_class_name, property_name, p_width);
}

PackedStringArray ResourceTablesPlugin::_get_visible_resource_paths() const {
	PackedStringArray paths;
	for (int i = 0; i < current_row_resources.size(); i++) {
		Ref<Resource> resource = current_row_resources[i];
		if (resource.is_valid()) {
			paths.push_back(resource->get_path());
		}
	}
	return paths;
}

// p_table_column is the container's own column index (0 == the revert
// icon column, 1 == Name, 2+i == properties[i] -- see this file's own
// top-of-file comment), not sort_column's numbering (offset by one, with
// no entry for the un-sortable revert column at all); a click on the
// revert column itself is simply ignored.
void ResourceTablesPlugin::_on_sort_header_pressed(int p_table_column) {
	if (p_table_column == 0) {
		return;
	}
	int clicked_sort_column = p_table_column - 1;
	if (sort_column == clicked_sort_column) {
		sort_ascending = !sort_ascending;
	} else {
		sort_column = clicked_sort_column;
		sort_ascending = true;
	}
	_rebuild_table();
}

// Applies a real property edit (skipped entirely if p_new_value is no
// different from p_old_value) through the editor's own undo/redo manager --
// the same mechanism the Inspector uses for property edits -- instead of a
// direct resource->set() call, so Ctrl+Z/Ctrl+Y works. commit_action()
// (p_execute defaults to true) performs the add_do_property call itself.
// p_old_value is passed in explicitly rather than read from p_resource here
// because by the time a continuous edit (see _on_property_editor_changed)
// commits, the resource's live value has already been updated to the new
// value for in-progress preview -- reading "old" at commit time would just
// see the new value and treat every drag as a no-op.
void ResourceTablesPlugin::_apply_property_edit(const Ref<Resource> &p_resource, const StringName &p_property_name, const Variant &p_old_value, const Variant &p_new_value) {
	if (p_old_value == p_new_value) {
		return;
	}

	EditorUndoRedoManager *undo_redo = get_undo_redo();
	undo_redo->create_action("Set " + String(p_property_name));
	undo_redo->add_do_property(p_resource.ptr(), p_property_name, p_new_value);
	undo_redo->add_undo_property(p_resource.ptr(), p_property_name, p_old_value);
	undo_redo->commit_action();

	_mark_resource_dirty(p_resource);
}

// See property_edit_session_resource's own comment (resource_tables_plugin.h)
// for why this can't treat p_changing alone as "safe to commit now" --
// every tick just applies the value live and remembers whether this
// specific tick called itself "changing"; _process (running only while a
// session is open) is what actually decides when the session is over.
void ResourceTablesPlugin::_on_property_editor_changed(StringName p_property, Variant p_value, StringName p_field, bool p_changing, Ref<Resource> p_resource) {
	bool is_new_session = property_edit_session_resource != p_resource || property_edit_session_property != p_property;
	if (is_new_session) {
		property_edit_session_resource = p_resource;
		property_edit_session_property = p_property;
		property_edit_session_old_value = p_resource->get(p_property);
		set_process(true);
	}

	property_edit_session_changing = p_changing;
	p_resource->set(p_property, p_value);
}

void ResourceTablesPlugin::_process(double p_delta) {
	if (property_edit_session_resource.is_null()) {
		set_process(false); // Shouldn't happen (see _finish_property_edit_session), but don't spin forever if it does.
		return;
	}

	// Still going if the most recent tick called itself "changing" (a
	// property type, e.g. a text field, that legitimately reports that
	// while still active) or the mouse's left button is still held (an
	// EditorSpinSlider-backed numeric property, which -- see this
	// member's own comment -- never reports changing=true at all, and
	// critically fires no signal whatsoever exactly at the moment the
	// button is released, which is why this has to poll rather than only
	// ever reacting to _on_property_editor_changed).
	if (property_edit_session_changing || DisplayServer::get_singleton()->mouse_get_button_state().has_flag(MOUSE_BUTTON_MASK_LEFT)) {
		return;
	}

	_finish_property_edit_session();
}

void ResourceTablesPlugin::_finish_property_edit_session() {
	Ref<Resource> resource = property_edit_session_resource;
	StringName property = property_edit_session_property;
	Variant old_value = property_edit_session_old_value;

	property_edit_session_resource = Ref<Resource>();
	set_process(false);

	// The resource's live value already equals the final tick's value --
	// every tick along the way applied it directly (see
	// _on_property_editor_changed) -- so there's no separate "new value"
	// to thread through from the signal itself.
	_apply_property_edit(resource, property, old_value, resource->get(property));

	callable_mp(this, &ResourceTablesPlugin::_rebuild_table).call_deferred();
}

void ResourceTablesPlugin::_on_inspector_property_edited(String p_property) {
	// Whatever object the Inspector currently has open -- it doesn't have to
	// be one of the currently-displayed resources (e.g. it could be a brand
	// new resource not yet saved to disk, or a value being tweaked before
	// this type was even selected).
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

	_mark_resource_dirty(resource);

	// Deferred: this can fire from within the Inspector's own signal
	// dispatch, and a synchronous rebuild there is asking for trouble.
	callable_mp(this, &ResourceTablesPlugin::_rebuild_table).call_deferred();
}

void ResourceTablesPlugin::_on_delete_requested() {
	PackedInt32Array selected = table->get_selected_rows();
	if (selected.is_empty()) {
		return;
	}
	String text;
	if (selected.size() == 1) {
		Ref<Resource> resource = current_row_resources[selected[0]];
		text = "Delete '" + resource->get_path() + "'?";
	} else {
		text = "Delete " + String::num_int64(selected.size()) + " resources?";
	}
	delete_confirm_dialog->set_text(text + "\nThis cannot be undone.");
	delete_confirm_dialog->popup_centered();
}

void ResourceTablesPlugin::_on_delete_confirmed() {
	PackedInt32Array selected = table->get_selected_rows();
	for (int row : selected) {
		if (row < 0 || row >= current_row_resources.size()) {
			continue;
		}
		Ref<Resource> resource = current_row_resources[row];
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
	// Deferred and queued in this order since this can fire from within the
	// editor's own filesystem-scan dispatch: any ResourceTable files
	// _run_generate_all creates/updates must already be on disk by the time
	// the dropdowns re-scan res://. There's no more specific "a Resource was
	// added or deleted" signal to hook instead of this one.
	callable_mp(this, &ResourceTablesPlugin::_check_dirty_resources_saved).call_deferred();
	callable_mp(this, &ResourceTablesPlugin::_run_generate_all).call_deferred();
	callable_mp(this, &ResourceTablesPlugin::_refresh_type_dropdown).call_deferred();
}

void ResourceTablesPlugin::_on_undo_redo_version_changed() {
	// Fires for every undo/redo action in the editor, not just ones
	// touching a currently-displayed resource -- rebuilding is cheap enough
	// that filtering which action this was isn't worth the complexity.
	callable_mp(this, &ResourceTablesPlugin::_rebuild_table).call_deferred();
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

void ResourceTablesPlugin::_on_tools_menu_id_pressed(int p_id) {
	switch (p_id) {
		case TOOLS_MENU_NEW_GENERATOR: {
			_on_new_generator_button_pressed();
			break;
		}
		case TOOLS_MENU_GENERATE_ALL: {
			_run_generate_all();
			_rebuild_table();
			break;
		}
		case TOOLS_MENU_EXPORT_CSV: {
			csv_dialog_is_export = true;
			csv_dialog->set_file_mode(EditorFileDialog::FILE_MODE_SAVE_FILE);
			csv_dialog->set_title("Export to CSV");
			csv_dialog->set_current_file(String(current_resource_class_name) + ".csv");
			csv_dialog->popup_centered_ratio(0.5);
			break;
		}
		case TOOLS_MENU_IMPORT_CSV: {
			csv_dialog_is_export = false;
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
		Ref<Script> script = ResourceLoader::get_singleton()->load(p_class_name);
		StringName global_name = script.is_valid() ? script->get_global_name() : StringName();
		p_class_name = global_name != StringName() ? String(global_name) : p_class_name.get_file().get_basename();
	}
	new_generator_class_line_edit->set_text(p_class_name);
}

void ResourceTablesPlugin::_on_new_generator_path_browse_pressed() {
	String path = new_generator_path_line_edit->get_text().strip_edges();
	new_generator_file_dialog->set_current_path(path.is_empty() ? String(DEFAULT_NEW_GENERATOR_PATH) : path);
	new_generator_file_dialog->popup_centered_ratio(0.5);
}

void ResourceTablesPlugin::_on_new_generator_path_browsed(String p_path) {
	new_generator_path_line_edit->set_text(p_path);
}

void ResourceTablesPlugin::_on_new_generator_confirmed() {
	String resource_class_name = new_generator_class_line_edit->get_text().strip_edges();
	String path = new_generator_path_line_edit->get_text().strip_edges();
	if (resource_class_name.is_empty() || path.is_empty()) {
		return;
	}
	if (path.get_extension().is_empty()) {
		path += ".gd";
	}
	_create_generator_script(resource_class_name, path);
}

void ResourceTablesPlugin::_on_csv_file_selected(String p_path) {
	if (csv_dialog_is_export) {
		ResourceTableUtils::export_csv(current_resource_class_name, _get_visible_resource_paths(), p_path);
		return;
	}

	PackedStringArray visible_paths = _get_visible_resource_paths();
	ResourceTableUtils::ImportPreview preview = ResourceTableUtils::preview_import_csv(current_resource_class_name, visible_paths, p_path);
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
	pending_import_resource_paths = visible_paths;
	import_confirm_dialog->set_text(
			"Importing this CSV will:\n"
			"  " + String::num_int64(adds) + " resource(s) added\n" +
			"  " + String::num_int64(updates) + " resource(s) updated\n" +
			"  " + String::num_int64(deletes) + " resource(s) deleted\n\n"
			"This cannot be undone. Continue?");
	import_confirm_dialog->popup_centered();
}

void ResourceTablesPlugin::_on_import_confirmed() {
	ResourceTableUtils::import_csv(current_resource_class_name, pending_import_resource_paths, pending_import_csv_path);
	_rebuild_table();
}

// Writes NEW_GENERATOR_TEMPLATE_PATH to p_path with its own
// ${RESOURCE_CLASS_NAME}/${RESOURCE_TABLE_DIR}/${RESOURCE_TABLE_NAME}
// placeholders substituted -- see that template's own comments for what
// each one means and expands to.
void ResourceTablesPlugin::_create_generator_script(const String &p_resource_class_name, const String &p_path) {

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
	String table_name = p_path.get_file().get_basename();

	content = content.replace("${RESOURCE_CLASS_NAME}", p_resource_class_name);
	content = content.replace("${RESOURCE_TABLE_DIR}", table_dir);
	content = content.replace("${RESOURCE_TABLE_NAME}", table_name);

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

void ResourceTablesPlugin::_on_new_resource_button_pressed() {
	if (current_resource_class_name == StringName()) {
		return;
	}

	new_resource_file_dialog->set_current_file(String(current_resource_class_name) + ".tres");
	new_resource_file_dialog->popup_centered_ratio(0.5);
}

void ResourceTablesPlugin::_on_new_resource_path_selected(String p_path) {
	if (current_resource_class_name == StringName()) {
		return;
	}

	Ref<Resource> resource = ResourceTableUtils::instantiate_resource_of_type(current_resource_class_name);
	if (resource.is_null()) {
		return;
	}

	Error save_err = ResourceSaver::get_singleton()->save(resource, p_path);
	if (save_err != OK) {
		UtilityFunctions::push_error("ResourceTablesPlugin: failed to save '", p_path, "' (error ", (int64_t)save_err, ").");
		return;
	}

	EditorInterface::get_singleton()->get_resource_filesystem()->scan();
	EditorInterface::get_singleton()->edit_resource(resource);
}

void ResourceTablesPlugin::_run_generate_all() {
	Ref<Script> script = ResourceLoader::get_singleton()->load(EXPORT_RESOURCE_TABLES_SCRIPT_PATH);
	if (script.is_null() || !script->can_instantiate()) {
		return;
	}

	Ref<EditorScript> editor_script = Ref<EditorScript>(script->call("new"));
	if (editor_script.is_valid()) {
		editor_script->_run();
	}
}

void ResourceTablesPlugin::_mark_resource_dirty(const Ref<Resource> &p_resource) {
	String path = p_resource->get_path();
	if (path.is_empty() || dirty_resource_mtimes.has(path)) {
		return; // No path to revert to, or already showing the revert button.
	}
	// Bookkeeping only -- callers decide whether/when it's safe to actually
	// rebuild the table to show the resulting revert button (see
	// _on_property_editor_changed, which must not do so mid-drag).
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
	String path = p_resource->get_path();
	dirty_resource_mtimes.erase(path);

	// Not just CACHE_MODE_REPLACE: the .tres saver skips writing any
	// property that equals its own default at save time (see
	// resource_format_text.cpp's ResourceFormatSaverText::save, which
	// compares against PropertyUtils::get_property_default_value and skips
	// a match), so a property left at its default when the file was last
	// saved never appears in the file at all. CACHE_MODE_REPLACE only
	// re-applies whatever properties the file actually contains, so it
	// silently leaves a since-edited in-memory value in place for any
	// property the file omitted -- e.g. edit a property away from its
	// default, then back to its default, then revert: CACHE_MODE_REPLACE
	// alone never touches it again, since the file never mentioned it.
	// Loading a second, independent copy (CACHE_MODE_IGNORE) and explicitly
	// copying every displayed property across covers that gap, since get()
	// on the fresh copy correctly returns the real default for anything the
	// file left out.
	Ref<Resource> fresh = ResourceLoader::get_singleton()->load(path, "", ResourceLoader::CACHE_MODE_IGNORE);
	if (fresh.is_valid()) {
		for (int i = 0; i < current_properties.size(); i++) {
			Dictionary info = current_properties[i];
			StringName property_name = info["name"];
			p_resource->set(property_name, fresh->get(property_name));
		}
	}
	_rebuild_table();
}
