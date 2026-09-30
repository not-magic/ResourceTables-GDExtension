#pragma once

#include "resource_table_export_plugin.h"

#include <godot_cpp/classes/editor_plugin.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/vector2.hpp>

namespace godot {

class Button;
class ConfirmationDialog;
class Control;
class EditorFileDialog;
class LineEdit;
class OptionButton;
class PopupMenu;
class ResourceTableContainer;

class ResourceTablesPlugin : public EditorPlugin {
	GDCLASS(ResourceTablesPlugin, EditorPlugin) // NOLINT

private:
	Control *main_panel = nullptr;
	PopupMenu *tools_menu = nullptr;
	OptionButton *type_dropdown = nullptr;
	Button *new_resource_button = nullptr;
	ResourceTableContainer *table = nullptr;

	EditorFileDialog *csv_dialog = nullptr;
	ConfirmationDialog *import_confirm_dialog = nullptr;
	ConfirmationDialog *delete_confirm_dialog = nullptr;
	ConfirmationDialog *new_generator_dialog = nullptr;
	LineEdit *new_generator_class_line_edit = nullptr;
	LineEdit *new_generator_path_line_edit = nullptr;
	EditorFileDialog *new_generator_file_dialog = nullptr;
	EditorFileDialog *new_resource_file_dialog = nullptr;
	String pending_import_csv_path;
	PackedStringArray pending_import_resource_paths;
	bool is_csv_dialog_export = false;

	Array current_row_resources;
	Array current_properties;
	StringName current_resource_class_name;
	int sort_column_index = 0; // 0 == Name (the default sort); N == properties[N - 1]
	bool is_sort_ascending = true;

	// Tracks a continuous edit (e.g. slider drag): ticks apply live with no undo/rebuild; the session
	// commits once the last tick wasn't changing AND the left button is up. Number editors never report
	// changing=true, so _process polls DisplayServer (a release outside the window updates no Input state).
	Ref<Resource> property_edit_session_resource;
	StringName property_edit_session_property;
	Variant property_edit_session_old_value;
	bool is_property_edit_session_changing = false;

	HashMap<String, uint64_t> dirty_resource_mtimes;

	Dictionary generator_snapshots;
	Ref<ResourceTableExportPlugin> export_plugin;
	bool is_generating = false;

	void _on_type_selected(int p_index);
	void _apply_type_selection(int p_index, bool p_is_sort_reset);
	void _refresh_type_dropdown();
	void _rebuild_table();
	void _apply_saved_column_width(int p_column_index);
	void _on_column_resized(int p_column_index, int p_width);
	void _on_sort_header_pressed(int p_table_column_index);
	void _on_delete_requested();
	void _on_delete_confirmed();
	void _on_inspector_property_edited(String p_property);
	void _on_filesystem_changed();
	void _on_undo_redo_version_changed();
	void _on_tools_menu_id_pressed(int p_id);
	void _on_csv_file_selected(String p_path);
	void _on_import_confirmed();
	void _on_new_generator_button_pressed();
	void _on_new_generator_class_browse_pressed();
	void _on_new_generator_class_picked(String p_class_name);
	void _on_new_generator_path_browse_pressed();
	void _on_new_generator_path_browsed(String p_path);
	void _on_new_generator_confirmed();
	void _on_new_resource_button_pressed();
	void _on_new_resource_path_selected(String p_path);
	void _run_export_tables(const String &p_modified_resource_class);
	void _sync_generators(bool p_should_generate);
	void _mark_resource_dirty(const Ref<Resource> &p_resource);
	void _check_dirty_resources_saved();
	void _on_revert_button_pressed(Ref<Resource> p_resource);
	void _apply_property_edit(const Ref<Resource> &p_resource, const StringName &p_property_name, const Variant &p_old_value, const Variant &p_new_value);
	void _on_property_editor_changed(StringName p_property, Variant p_value, StringName p_field, bool p_is_changing, Ref<Resource> p_resource);
	void _finish_property_edit_session();

protected:
	static void _bind_methods();

public:
	void _enter_tree() override;
	void _exit_tree() override;
	bool _build() override;
	void _process(double p_delta) override;
};

} // namespace godot
