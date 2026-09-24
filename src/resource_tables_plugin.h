#pragma once

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/editor_plugin.hpp>
#include <godot_cpp/classes/option_button.hpp>
#include <godot_cpp/classes/tree.hpp>
#include <godot_cpp/variant/array.hpp>

namespace godot {

// Adds a "Resources" bottom panel, next to Output/Debugger/Shader Editor.
class ResourceTablesPlugin : public EditorPlugin {
	GDCLASS(ResourceTablesPlugin, EditorPlugin)

private:
	Control *main_panel = nullptr;
	OptionButton *class_dropdown = nullptr;
	Tree *table_tree = nullptr;

	StringName current_class_name;
	Array current_properties; // the filtered (exported) script_property_list for current_class_name
	int sort_column = 0; // 0 == Name (the default sort); N == properties[N - 1]
	bool sort_ascending = true;

	void _on_class_selected(int p_index);
	void _rebuild_table(const StringName &p_class_name);
	void _on_row_selected();
	void _on_item_edited();
	void _on_column_title_clicked(int p_column, int p_mouse_button_index);
	bool _compare_by_sort_column(Variant p_a, Variant p_b);
	void _on_inspector_property_edited(String p_property);
	void _on_filesystem_changed();

protected:
	static void _bind_methods();

public:
	void _enter_tree() override;
	void _exit_tree() override;
};

} // namespace godot
