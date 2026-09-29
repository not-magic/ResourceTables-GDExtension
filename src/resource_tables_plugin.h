#pragma once

#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/confirmation_dialog.hpp>
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/editor_file_dialog.hpp>
#include <godot_cpp/classes/editor_plugin.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/line_edit.hpp>
#include <godot_cpp/classes/option_button.hpp>
#include <godot_cpp/classes/popup_menu.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/vector2.hpp>

#include "resource_table_name.h"
#include "resource_table_container.h"

namespace godot {

// Adds a "Resources" bottom panel, next to Output/Debugger/Shader Editor.
class ResourceTablesPlugin : public EditorPlugin {
	GDCLASS(ResourceTablesPlugin, EditorPlugin)

private:
	Control *main_panel = nullptr;
	PopupMenu *tools_menu = nullptr;
	OptionButton *type_dropdown = nullptr;
	Button *new_resource_button = nullptr;

	// The data area -- see _rebuild_table for how this file populates it.
	ResourceTableContainer *table = nullptr;
	Array current_row_resources; // resources currently displayed, in display order

	EditorFileDialog *csv_dialog = nullptr;
	bool csv_dialog_is_export = false;
	ConfirmationDialog *import_confirm_dialog = nullptr;
	ConfirmationDialog *delete_confirm_dialog = nullptr;
	String pending_import_csv_path;
	PackedStringArray pending_import_resource_paths;

	ConfirmationDialog *new_generator_dialog = nullptr;
	LineEdit *new_generator_class_line_edit = nullptr;
	LineEdit *new_generator_path_line_edit = nullptr;
	EditorFileDialog *new_generator_file_dialog = nullptr; // "browse" helper for the path field

	EditorFileDialog *new_resource_file_dialog = nullptr;

	// Tracks an in-progress continuous edit (e.g. dragging a slider): every
	// tick applies the value live with no undo/redo or rebuild (which would
	// destroy the widget being dragged); only once the session is over does
	// this commit one undo/redo action and rebuild the table. "Over" isn't
	// simply "the tick that reported changing=false" -- EditorPropertyInteger/
	// EditorPropertyFloat call EditorProperty::emit_changed() with p_changing
	// always defaulting to false, never true, on every drag tick (confirmed
	// in editor/inspector/editor_properties.cpp), so _process instead polls
	// once a frame: a session is over once the most recent tick's own
	// p_changing was false AND the mouse's left button isn't currently held.
	// A live DisplayServer query, not Input's own cached button mask, for the
	// same reason ResourceTableContainer's own drag-safety-net uses one -- a
	// release outside this editor window entirely updates neither.
	Ref<Resource> property_edit_session_resource;
	StringName property_edit_session_property;
	Variant property_edit_session_old_value;
	bool property_edit_session_changing = false;

	// Resources with unsaved edits (made via an embedded property editor or
	// the Inspector) -- see _mark_resource_dirty. Keyed by resource path,
	// valued by the file's on-disk modified-time at the moment it was
	// marked dirty, so _on_filesystem_changed can tell whether a later save
	// (Ctrl+S, the Inspector's own save button, ...) has since written it
	// back out.
	HashMap<String, uint64_t> dirty_resource_mtimes;

	StringName current_resource_class_name; // the dropdown's selection
	Array current_properties; // the filtered (exported) script_property_list for current_resource_class_name
	int sort_column = 0; // 0 == Name (the default sort); N == properties[N - 1]
	bool sort_ascending = true;

	void _on_type_selected(int p_index);
	void _apply_type_selection(int p_index, bool p_reset_sort);
	void _refresh_type_dropdown();
	void _rebuild_table();
	String _column_property_name(int p_column) const;
	void _apply_saved_column_width(int p_column);
	void _on_column_resized(int p_column, int p_width);
	PackedStringArray _get_visible_resource_paths() const;
	void _on_sort_header_pressed(int p_table_column);
	void _on_delete_requested();
	void _on_delete_confirmed();
	bool _compare_by_sort_column(Variant p_a, Variant p_b);
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
	void _create_generator_script(const String &p_resource_class_name, const String &p_path);
	void _on_new_resource_button_pressed();
	void _on_new_resource_path_selected(String p_path);
	void _run_generate_all();
	void _mark_resource_dirty(const Ref<Resource> &p_resource);
	void _check_dirty_resources_saved();
	void _on_revert_button_pressed(Ref<Resource> p_resource);
	void _apply_property_edit(const Ref<Resource> &p_resource, const StringName &p_property_name, const Variant &p_old_value, const Variant &p_new_value);
	void _on_property_editor_changed(StringName p_property, Variant p_value, StringName p_field, bool p_changing, Ref<Resource> p_resource);
	void _finish_property_edit_session();

protected:
	static void _bind_methods();

public:
	void _enter_tree() override;
	void _exit_tree() override;
	// Polls property_edit_session_resource -- see that member's own
	// comment for why this can't just react to _on_property_editor_changed
	// synchronously. Only actually running (set_process(true)) while a
	// session is open.
	void _process(double p_delta) override;
};

} // namespace godot
