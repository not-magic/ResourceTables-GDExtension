#pragma once

#include <godot_cpp/classes/container.hpp>
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/h_scroll_bar.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/style_box_flat.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/v_scroll_bar.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/rect2.hpp>
#include <godot_cpp/variant/string.hpp>

#include <set>
#include <vector>

namespace godot {

class ResourceTableContainer : public Container {
	GDCLASS(ResourceTableContainer, Container) // NOLINT

public:
	enum SortDirection {
		SORT_NONE,
		SORT_ASCENDING,
		SORT_DESCENDING,
	};

	struct Column {
		String text;
		SortDirection sort_direction = SORT_NONE;
		int x = 0;
		int width = 0;
		int min_width = 0;
		int max_width = -1; // -1 == uncapped
		bool is_width_auto = true;
		bool is_resizable = true;
	};

	struct RowData {
		std::vector<Control *> cells;
		int y = 0;
		int height = 0;
	};

private:
	static constexpr int DEFAULT_HEADER_HEIGHT = 26;

	std::vector<Column> columns;
	std::vector<RowData> rows;
	std::set<int> selected_rows;

	Control *header_clip = nullptr;
	Control *body = nullptr;
	Control *body_clip = nullptr;
	Control *scroll_hint_overlay = nullptr;
	VScrollBar *v_scroll = nullptr;
	HScrollBar *h_scroll = nullptr;

	Ref<StyleBoxFlat> body_background_style;
	Ref<StyleBoxFlat> header_background_style;
	Color row_hover_color = Color(1, 1, 1, 0.12); // not the editor theme's own accent-colored highlight_color
	Color row_selected_color = Color(1, 1, 1, 0.08);
	Color grid_line_color = Color(1, 1, 1, 0.1);
	Color resize_handle_hover_color = Color(1, 1, 1, 0.5);
	int left_margin = 1;
	int right_margin = 1;
	int top_margin = 1;
	int bottom_margin = 1;
	int border_x = 1;
	int border_y = 1;
	int header_height = DEFAULT_HEADER_HEIGHT;

	Vector2 pan_drag_start_mouse;
	int pan_drag_start_h_value = 0;
	int pan_drag_start_v_value = 0;
	int cached_total_width = 0;
	int cached_total_body_height = 0;
	int cached_h_offset = 0;
	int cached_v_offset = 0;
	bool is_updating_scroll = false;
	bool is_pan_dragging = false;

	Vector2 hover_last_local_mouse;
	int hovered_row_index = -1;
	int selection_anchor_index = -1;
	int hover_last_h_offset = 0;
	int hover_last_v_offset = 0;
	bool is_hover_last_valid = false;

	// Polled in _update_hover() rather than driven by signals, so a release outside the editor window can't stick.
	int hovered_separator_index = -1;
	int dragging_separator_index = -1;
	int drag_start_mouse_x = 0;
	int drag_start_width = 0;
	int press_column_index = -1;

	void _resort();
	void _update_hover();
	void _set_selection(const std::set<int> &p_rows);
	void _on_body_draw();
	void _on_header_draw();
	void _draw_header_column(int p_index, int p_x, int p_width, int p_height);
	void _on_scroll_hint_draw();
	void _update_scrollbars();
	void _update_scroll(double p_value);
	void _on_pan_input(const Ref<InputEvent> &p_event);

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	ResourceTableContainer();

	void _input(const Ref<InputEvent> &p_event) override;
	void _gui_input(const Ref<InputEvent> &p_event) override;
	void _draw() override;

	void set_columns(int p_column_total, const std::vector<float> &p_default_widths);

	void set_column_width(int p_column_index, float p_width);

	void set_column_header_text(int p_column_index, const String &p_text);
	void set_column_sort_direction(int p_column_index, SortDirection p_direction);

	int add_row();
	void set_cell(int p_row_index, int p_column_index, Control *p_cell);

	PackedInt32Array get_selected_rows() const;
	void clear_selection();

	void clear();
};

} // namespace godot

VARIANT_ENUM_CAST(godot::ResourceTableContainer::SortDirection);
