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
	GDCLASS(ResourceTableContainer, Container)

public:
	enum SortDirection {
		SORT_NONE,
		SORT_ASCENDING,
		SORT_DESCENDING,
	};

private:
	// The single authoritative record of a column's state.
	struct Column {
		String text;
		SortDirection sort_direction = SORT_NONE;
		int x = 0; // left edge, content-space (unscrolled); recomputed by _recompute_column_x()
		int width = 0;
		bool width_is_auto = true;
		int min_width = 0;
		int max_width = -1; // -1 == uncapped
		bool resizable = true; // false hides this column's own right-edge resize handle
	};

	struct RowData {
		std::vector<Control *> cells;
		int y = 0;
		int height = 0;
	};

	std::vector<Column> columns;
	std::vector<RowData> rows;

	int left_margin = 1;
	int right_margin = 1;
	int top_margin = 1;
	int bottom_margin = 1;
	int border_x = 1;
	int border_y = 1;

	int header_height = DEFAULT_HEADER_HEIGHT;
	Control *header_clip = nullptr;

	Control *body = nullptr;
	Control *body_clip = nullptr;
	Control *scroll_hint_overlay = nullptr;

	VScrollBar *v_scroll = nullptr;
	HScrollBar *h_scroll = nullptr;
	bool updating_scroll = false;

	bool pan_dragging = false;
	Vector2 pan_drag_start_mouse;
	int pan_drag_start_h_value = 0;
	int pan_drag_start_v_value = 0;

	int cached_total_width = 0;
	int cached_total_body_height = 0;
	int cached_h_offset = 0;
	int cached_v_offset = 0;

	int hovered_row = -1;
	std::set<int> selected_rows;
	int selection_anchor = -1;
	Vector2 hover_last_local_mouse;
	bool hover_last_valid = false;
	int hover_last_h_offset = 0;
	int hover_last_v_offset = 0;

	// Header column resize-handle hover/drag, and column-body press (for a
	// sort click) -- polled every NOTIFICATION_INTERNAL_PROCESS tick in
	// _update_hover() the same way hovered_row is, rather than from discrete
	// enter/exit/release signals, so a release outside the whole editor
	// window can't leave a highlight or a drag stuck active.
	int hovered_separator = -1;
	int dragging_separator = -1;
	int drag_start_mouse_x = 0;
	int drag_start_width = 0;
	int press_column = -1;

	Color row_hover_color = Color(1, 1, 1, 0.12); // not the editor theme's own accent-colored highlight_color
	Color row_selected_color = Color(1, 1, 1, 0.08);
	Color grid_line_color = Color(1, 1, 1, 0.1);
	Color resize_handle_hover_color = Color(1, 1, 1, 0.5);

	Ref<StyleBoxFlat> body_background_style;
	Ref<StyleBoxFlat> header_background_style;

	static constexpr int MIN_COLUMN_WIDTH = 16;
	static constexpr int MIN_ROW_HEIGHT = 20;
	static constexpr int DEFAULT_HEADER_HEIGHT = 26;
	static constexpr int SCROLLBAR_WIDTH = 14;
	static constexpr int GRID_LINE_MARGIN = 3;
	static constexpr float HEADER_LIGHTEN_AMOUNT = 0.05f;
	static constexpr int RESIZE_HANDLE_WIDTH = 8;
	static constexpr int HEADER_TEXT_PADDING = 6;
	static constexpr int HEADER_ICON_SIZE = 16;
	static constexpr int HEADER_ICON_GAP = 4;
	static constexpr int HEADER_VERTICAL_PADDING = 6;

	void _resort();
	void _recompute_column_x();
	int _separator_at_x(int p_x) const;
	int _column_at_x(int p_x) const;
	void _update_hover();
	int _row_at_global_position(const Vector2 &p_pos) const;
	void _set_selection(const std::set<int> &p_rows);
	void _on_body_draw();
	void _on_header_draw();
	void _draw_header_column(int p_index, int p_x, int p_width, int p_height);
	void _on_scroll_hint_draw();
	void _update_scrollbars();
	void _update_scroll(double p_value);
	void _on_pan_input(const Ref<InputEvent> &p_event);
	int _get_preferred_header_height() const;
	int _get_column_natural_width(int p_index) const;

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	ResourceTableContainer();

	void _input(const Ref<InputEvent> &p_event) override;
	void _gui_input(const Ref<InputEvent> &p_event) override;
	void _draw() override;

	void set_columns(int p_columns, const std::vector<float> &p_default_widths);

	// used for restoring column width
	void set_column_width(int p_column, float p_width);

	void set_column_header_text(int p_column, const String &p_text);
	void set_column_sort_direction(int p_column, SortDirection p_direction);

	int add_row();
	void set_cell(int p_row, int p_column, Control *p_cell);

	PackedInt32Array get_selected_rows() const;
	void clear_selection();

	void clear();
};

} // namespace godot

VARIANT_ENUM_CAST(godot::ResourceTableContainer::SortDirection);
