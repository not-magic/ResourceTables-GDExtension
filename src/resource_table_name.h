#pragma once

#include <godot_cpp/classes/confirmation_dialog.hpp>
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/editor_file_dialog.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/line_edit.hpp>
#include <godot_cpp/classes/popup_menu.hpp>
#include <godot_cpp/classes/resource.hpp>

namespace godot {

// ResourceTableContainer's Name cell. Owns its own right-click context menu
// (mirroring the FileSystem dock's own per-file menu as closely as the
// exposed editor API allows) and performs every action itself, inline
// rename included -- each one's own EditorFileSystem::scan() call is enough
// to reach ResourceTablesPlugin's existing "filesystem_changed" listener
// (which already rebuilds the table and reconciles the dirty-mtime map for
// edits made outside this class entirely, e.g. via the FileSystem dock), so
// no signal back to the plugin is needed for any of them -- except Delete,
// which emits "delete_requested" since it acts on the table's whole selection.
class ResourceTableName : public Control {
	GDCLASS(ResourceTableName, Control)

private:
	enum ContextMenuId {
		CONTEXT_OPEN,
		CONTEXT_SHOW_IN_FILESYSTEM,
		CONTEXT_COPY_PATH,
		CONTEXT_COPY_ABSOLUTE_PATH,
		CONTEXT_COPY_UID,
		CONTEXT_RENAME,
		CONTEXT_DUPLICATE,
		CONTEXT_MOVE_TO,
		CONTEXT_DELETE,
	};

	Label *name_label = nullptr;
	LineEdit *rename_edit = nullptr;
	Ref<Resource> resource; // set via set_resource(); drag data, menu actions, and rename all key off this

	PopupMenu *context_menu = nullptr;
	EditorFileDialog *duplicate_or_move_dialog = nullptr;
	bool duplicate_or_move_is_move = false;

	static constexpr float MAX_RENAME_EDIT_WIDTH = 200.0f;

	float _measure_rename_edit_min_width(const String &p_text) const;
	void _on_name_gui_input(const Ref<InputEvent> &p_event);
	void _on_rename_text_submitted(String p_new_text);
	void _on_rename_focus_exited();
	void _on_rename_gui_input(const Ref<InputEvent> &p_event);
	void _commit_rename();
	void _cancel_rename();

	void _popup_context_menu(const Vector2 &p_screen_position);
	void _on_context_menu_id_pressed(int p_id);
	void _on_duplicate_or_move_file_selected(String p_path);

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	ResourceTableName();

	// Only a Container subclass aggregates children's sizes automatically;
	// without this override, ResourceTableContainer's own row-height/
	// vertical-centering math (get_combined_minimum_size()) sees (0,0).
	Vector2 _get_minimum_size() const override;

	// name_label needs MOUSE_FILTER_PASS (not its Label default of IGNORE)
	// for Viewport's drag-source walk-up to ever reach this override.
	Variant _get_drag_data(const Vector2 &p_at_position) override;

	void set_text(const String &p_text);
	String get_text() const;

	void set_resource(const Ref<Resource> &p_resource);

	void begin_rename();
	bool is_renaming() const;

	// Called by ResourceTableContainer's own auto-fit sizing via a
	// duck-typed "if this cell has get_natural_width(), call it" check.
	float get_natural_width() const;
};

} // namespace godot
