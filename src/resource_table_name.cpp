#include "resource_table_name.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/file_system_dock.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/resource_saver.hpp>
#include <godot_cpp/classes/resource_uid.hpp>
#include <godot_cpp/classes/texture_rect.hpp>
#include <godot_cpp/classes/v_box_container.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

namespace {

constexpr float MAX_RENAME_EDIT_WIDTH = 200.0f;

float measure_rename_edit_min_width(const LineEdit *p_edit, const String &p_text) {
	Ref<Font> font = p_edit->get_theme_font(StringName("font"));
	if (!font.is_valid()) {
		font = p_edit->get_theme_default_font();
	}
	if (!font.is_valid()) {
		return 0.0f;
	}
	int font_size = p_edit->get_theme_font_size(StringName("font_size"));
	if (font_size <= 0) {
		font_size = p_edit->get_theme_default_font_size();
	}
	const float text_width = font->get_string_size(p_text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size).x;
	return MIN(MAX_RENAME_EDIT_WIDTH, text_width);
}

} // namespace

void ResourceTableName::_bind_methods() {
	ClassDB::bind_method(D_METHOD("calc_natural_width"), &ResourceTableName::calc_natural_width);

	ADD_SIGNAL(MethodInfo("delete_requested"));
}

ResourceTableName::ResourceTableName() {
	name_label = memnew(Label);
	name_label->set_clip_text(true);
	name_label->set_mouse_filter(Control::MOUSE_FILTER_PASS);
	name_label->connect("gui_input", callable_mp(this, &ResourceTableName::_on_name_gui_input));
	add_child(name_label);

	rename_edit = memnew(LineEdit);
	rename_edit->set_visible(false);
	rename_edit->connect("text_submitted", callable_mp(this, &ResourceTableName::_on_rename_text_submitted));
	rename_edit->connect("focus_exited", callable_mp(this, &ResourceTableName::_on_rename_focus_exited));
	rename_edit->connect("gui_input", callable_mp(this, &ResourceTableName::_on_rename_gui_input));
	add_child(rename_edit);

	// Icons mirror filesystem_dock.cpp. No Edit Dependencies/View Owners: DependencyEditor(Owners)
	// bind no methods, so they can't be driven from a GDExtension. Open in Inspector is our own addition.
	const Control *const editor_base = EditorInterface::get_singleton()->get_base_control();
	context_menu = memnew(PopupMenu);
	context_menu->add_item("Open in Inspector", CONTEXT_OPEN);
	// Same call as EditorResourcePicker's handler, via EditorInterface (FileSystemDock::get_singleton() isn't exposed).
	context_menu->add_icon_item(editor_base->get_theme_icon("ShowInFileSystem", "EditorIcons"), "Show in FileSystem", CONTEXT_SHOW_IN_FILESYSTEM);
	context_menu->add_separator();
	context_menu->add_icon_item(editor_base->get_theme_icon("ActionCopy", "EditorIcons"), "Copy Path", CONTEXT_COPY_PATH);
	context_menu->add_item("Copy Absolute Path", CONTEXT_COPY_ABSOLUTE_PATH);
	context_menu->add_icon_item(editor_base->get_theme_icon("Instance", "EditorIcons"), "Copy UID", CONTEXT_COPY_UID);
	context_menu->add_icon_item(editor_base->get_theme_icon("Rename", "EditorIcons"), "Rename", CONTEXT_RENAME);
	context_menu->add_icon_item(editor_base->get_theme_icon("Duplicate", "EditorIcons"), "Duplicate...", CONTEXT_DUPLICATE);
	context_menu->add_icon_item(editor_base->get_theme_icon("MoveUp", "EditorIcons"), "Move/Duplicate To...", CONTEXT_MOVE_TO);
	context_menu->add_icon_item(editor_base->get_theme_icon("Remove", "EditorIcons"), "Delete", CONTEXT_DELETE);
	context_menu->connect("id_pressed", callable_mp(this, &ResourceTableName::_on_context_menu_id_pressed));
	add_child(context_menu);

	duplicate_or_move_dialog = memnew(EditorFileDialog);
	duplicate_or_move_dialog->set_access(EditorFileDialog::ACCESS_RESOURCES);
	duplicate_or_move_dialog->set_file_mode(EditorFileDialog::FILE_MODE_SAVE_FILE);
	duplicate_or_move_dialog->connect("file_selected", callable_mp(this, &ResourceTableName::_on_duplicate_or_move_file_selected));
	add_child(duplicate_or_move_dialog);
}

void ResourceTableName::_notification(int p_what) {
	if (p_what == NOTIFICATION_RESIZED) {
		const Vector2 size = get_size();
		name_label->set_position(Vector2());
		name_label->set_size(size);
		rename_edit->set_position(Vector2());
		rename_edit->set_size(size);
	}
}

Vector2 ResourceTableName::_get_minimum_size() const {
	return name_label->get_combined_minimum_size();
}

Variant ResourceTableName::_get_drag_data(const Vector2 &p_at_position) {
	if (resource.is_null()) {
		return Variant();
	}

	VBoxContainer *const preview = memnew(VBoxContainer);
	TextureRect *const icon = memnew(TextureRect);
	icon->set_custom_minimum_size(Vector2(48, 48));
	icon->set_stretch_mode(TextureRect::STRETCH_KEEP_CENTERED);
	icon->set_texture(EditorInterface::get_singleton()->get_base_control()->get_theme_icon("FileBigThumb", "EditorIcons"));
	preview->add_child(icon);
	Label *const preview_label = memnew(Label);
	preview_label->set_text(get_text());
	preview_label->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	preview->add_child(preview_label);
	set_drag_preview(preview);

	Dictionary drag_data;
	drag_data["type"] = "resource";
	drag_data["resource"] = resource;
	drag_data["from"] = name_label;
	return drag_data;
}

void ResourceTableName::set_text(const String &p_text) {
	name_label->set_text(p_text);
}

String ResourceTableName::get_text() const {
	return name_label->get_text();
}

void ResourceTableName::set_resource(const Ref<Resource> &p_resource) {
	resource = p_resource;
}

void ResourceTableName::begin_rename() {
	const String text = name_label->get_text();
	rename_edit->set_text(text);
	rename_edit->set_custom_minimum_size(Vector2(measure_rename_edit_min_width(rename_edit, text), 0));
	name_label->set_visible(false);
	rename_edit->set_visible(true);
	rename_edit->grab_focus();
	rename_edit->select_all();
}

bool ResourceTableName::is_renaming() const {
	return rename_edit->is_visible();
}

float ResourceTableName::calc_natural_width() const {
	name_label->set_clip_text(false);
	const float width = name_label->get_combined_minimum_size().x;
	name_label->set_clip_text(true);
	return width;
}

void ResourceTableName::_on_name_gui_input(const Ref<InputEvent> &p_event) {
	const Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_valid() && mb->is_pressed() && mb->get_button_index() == MOUSE_BUTTON_RIGHT) {
		_popup_context_menu(name_label->get_screen_position() + mb->get_position());
	}
}

void ResourceTableName::_on_rename_text_submitted(String p_new_text) {
	_commit_rename();
}

void ResourceTableName::_on_rename_focus_exited() {
	// Also fires as a side effect of _cancel_rename()'s own release_focus();
	// harmless, since rename_edit is already hidden by then and this no-ops.
	if (is_renaming()) {
		_commit_rename();
	}
}

void ResourceTableName::_on_rename_gui_input(const Ref<InputEvent> &p_event) {
	const Ref<InputEventKey> key = p_event;
	if (key.is_valid() && key->is_pressed() && key->get_keycode() == KEY_ESCAPE) {
		_cancel_rename();
	}
}

void ResourceTableName::_commit_rename() {
	const String new_text = rename_edit->get_text();
	rename_edit->set_visible(false);
	name_label->set_text(new_text);
	name_label->set_visible(true);

	if (resource.is_null()) {
		return;
	}
	String old_path = resource->get_path();
	String new_basename = new_text.strip_edges();
	if (new_basename.is_empty() || new_basename == old_path.get_file().get_basename()) {
		return;
	}

	String new_path = old_path.get_base_dir().path_join(new_basename + "." + old_path.get_extension());
	if (ResourceLoader::get_singleton()->exists(new_path)) {
		UtilityFunctions::push_error("ResourceTableName: cannot rename to '", new_path, "' -- a file already exists there.");
		return;
	}

	const Error err = DirAccess::rename_absolute(old_path, new_path);
	if (err != OK) {
		UtilityFunctions::push_error("ResourceTableName: failed to rename '", old_path, "' to '", new_path, "' (error ", (int64_t)err, ").");
		return;
	}
	if (FileAccess::file_exists(old_path + ".uid")) {
		DirAccess::rename_absolute(old_path + ".uid", new_path + ".uid");
	}

	resource->take_over_path(new_path);
	EditorInterface::get_singleton()->get_resource_filesystem()->scan();
}

void ResourceTableName::_cancel_rename() {
	rename_edit->set_visible(false);
	name_label->set_visible(true);
	rename_edit->release_focus();
}

void ResourceTableName::_popup_context_menu(const Vector2 &p_screen_position) {
	context_menu->set_position(Vector2i(p_screen_position));
	context_menu->reset_size();
	context_menu->popup();
}

void ResourceTableName::_on_context_menu_id_pressed(int p_id) {
	if (resource.is_null()) {
		return;
	}
	const String path = resource->get_path();

	switch (p_id) {
		case CONTEXT_OPEN: {
			EditorInterface::get_singleton()->edit_resource(resource);
			break;
		}
		case CONTEXT_SHOW_IN_FILESYSTEM: {
			EditorInterface::get_singleton()->get_file_system_dock()->navigate_to_path(path);
			break;
		}
		case CONTEXT_COPY_PATH: {
			DisplayServer::get_singleton()->clipboard_set(path);
			break;
		}
		case CONTEXT_COPY_ABSOLUTE_PATH: {
			DisplayServer::get_singleton()->clipboard_set(ProjectSettings::get_singleton()->globalize_path(path));
			break;
		}
		case CONTEXT_COPY_UID: {
			const int64_t uid = ResourceLoader::get_singleton()->get_resource_uid(path);
			if (uid != ResourceUID::INVALID_ID) {
				DisplayServer::get_singleton()->clipboard_set(ResourceUID::get_singleton()->id_to_text(uid));
			}
			break;
		}
		case CONTEXT_RENAME: {
			begin_rename();
			break;
		}
		case CONTEXT_DUPLICATE: {
			is_move_mode = false;
			duplicate_or_move_dialog->set_title("Duplicate Resource");
			duplicate_or_move_dialog->set_current_file(path.get_file().get_basename() + "_copy." + path.get_extension());
			duplicate_or_move_dialog->popup_centered_ratio(0.5);
			break;
		}
		case CONTEXT_MOVE_TO: {
			is_move_mode = true;
			duplicate_or_move_dialog->set_title("Move/Duplicate To...");
			duplicate_or_move_dialog->set_current_path(path);
			duplicate_or_move_dialog->popup_centered_ratio(0.5);
			break;
		}
		case CONTEXT_DELETE: {
			emit_signal("delete_requested");
			break;
		}
	}
}

void ResourceTableName::_on_duplicate_or_move_file_selected(String p_path) {
	if (resource.is_null()) {
		return;
	}
	String old_path = resource->get_path();
	if (p_path == old_path) {
		return;
	}

	const Ref<Resource> to_save = is_move_mode ? resource : resource->duplicate(true);
	const Error save_err = ResourceSaver::get_singleton()->save(to_save, p_path);
	if (save_err != OK) {
		UtilityFunctions::push_error("ResourceTableName: failed to save '", p_path, "' (error ", (int64_t)save_err, ").");
		return;
	}

	if (is_move_mode) {
		DirAccess::remove_absolute(old_path);
		if (FileAccess::file_exists(old_path + ".uid")) {
			DirAccess::remove_absolute(old_path + ".uid");
		}
		resource->take_over_path(p_path);
	}

	EditorInterface::get_singleton()->get_resource_filesystem()->scan();
}
