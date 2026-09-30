#include "resource_table_container.h"

#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_settings.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/line_edit.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/style_box_empty.hpp>
#include <godot_cpp/classes/text_edit.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/theme.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/rect2.hpp>

#include <cmath>

using namespace godot;

namespace {

constexpr int MIN_COLUMN_WIDTH = 16;
constexpr int MIN_ROW_HEIGHT = 20;
constexpr int SCROLLBAR_WIDTH = 14;
constexpr int GRID_LINE_MARGIN = 3;
constexpr float HEADER_LIGHTEN_AMOUNT = 0.05f;
constexpr int RESIZE_HANDLE_WIDTH = 8;
constexpr int HEADER_TEXT_PADDING = 6;
constexpr int HEADER_ICON_SIZE = 16;
constexpr int HEADER_ICON_GAP = 4;
constexpr int HEADER_VERTICAL_PADDING = 6;

// A clip-text Button's minimum size excludes its text width; toggling
// clip_text off briefly recovers the real, text-driven width.
float measure_cell_natural_width(Control *p_cell) {
	if (p_cell->has_method("calc_natural_width")) {
		return p_cell->call("calc_natural_width");
	}
	float natural = p_cell->get_combined_minimum_size().x;
	Button *const button = Object::cast_to<Button>(p_cell);
	if (button && button->get_clip_text()) {
		button->set_clip_text(false);
		natural = MAX(natural, p_cell->get_combined_minimum_size().x);
		button->set_clip_text(true);
	}
	return natural;
}

// get_custom_maximum_size() isn't exposed as a typed Control method by this
// godot-cpp snapshot; -1 means uncapped.
float get_cell_custom_max_width(Control *p_cell) {
	const Vector2 custom_max = p_cell->call("get_custom_maximum_size");
	return custom_max.x;
}

void recompute_column_x(std::vector<ResourceTableContainer::Column> &r_columns, int p_border_x) {
	int x = 0;
	for (size_t i = 0; i < r_columns.size(); i++) {
		r_columns[i].x = x;
		x += r_columns[i].width + p_border_x;
	}
}

int separator_at_x(const std::vector<ResourceTableContainer::Column> &p_columns, int p_border_x, int p_x) {
	for (int i = 0; i < (int)p_columns.size(); i++) {
		if (!p_columns[i].is_resizable) {
			continue;
		}
		const int center = p_columns[i].x + p_columns[i].width + p_border_x / 2;
		if (p_x >= center - RESIZE_HANDLE_WIDTH / 2 && p_x <= center + RESIZE_HANDLE_WIDTH / 2) {
			return i;
		}
	}
	return -1;
}

int column_at_x(const std::vector<ResourceTableContainer::Column> &p_columns, int p_x) {
	for (int i = 0; i < (int)p_columns.size(); i++) {
		if (p_x >= p_columns[i].x && p_x < p_columns[i].x + p_columns[i].width) {
			return i;
		}
	}
	return -1;
}

int row_at_global_position(const std::vector<ResourceTableContainer::RowData> &p_rows, const Control *p_body_clip, const Control *p_v_scroll, const Control *p_h_scroll, int p_v_offset, const Vector2 &p_pos) {
	if (!p_body_clip->get_global_rect().has_point(p_pos)) {
		return -1;
	}
	if ((p_v_scroll->is_visible() && p_v_scroll->get_global_rect().has_point(p_pos)) ||
			(p_h_scroll->is_visible() && p_h_scroll->get_global_rect().has_point(p_pos))) {
		return -1;
	}
	const int y = (int)(p_pos.y - p_body_clip->get_global_position().y) + p_v_offset;
	for (size_t r = 0; r < p_rows.size(); r++) {
		if (y >= p_rows[r].y && y < p_rows[r].y + p_rows[r].height) {
			return (int)r;
		}
	}
	return -1;
}

int calc_preferred_header_height(const Control *p_control) {
	Ref<Font> font = p_control->get_theme_font(StringName("font"), StringName("Label"));
	if (!font.is_valid()) {
		font = p_control->get_theme_default_font();
	}
	int font_size = p_control->get_theme_font_size(StringName("font_size"), StringName("Label"));
	if (font_size <= 0) {
		font_size = p_control->get_theme_default_font_size();
	}
	if (!font.is_valid()) {
		return HEADER_VERTICAL_PADDING * 2;
	}
	return (int)Math::ceil(font->get_ascent(font_size) + font->get_descent(font_size)) + HEADER_VERTICAL_PADDING * 2;
}

int calc_column_natural_width(const Control *p_control, const ResourceTableContainer::Column &p_column) {
	Ref<Font> font = p_control->get_theme_font(StringName("font"), StringName("Label"));
	if (!font.is_valid()) {
		font = p_control->get_theme_default_font();
	}
	int font_size = p_control->get_theme_font_size(StringName("font_size"), StringName("Label"));
	if (font_size <= 0) {
		font_size = p_control->get_theme_default_font_size();
	}

	const float text_width = font.is_valid() ? font->get_string_size(p_column.text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size).x : 0.0f;
	const int icon_reserve = p_column.sort_direction != ResourceTableContainer::SORT_NONE ? HEADER_ICON_SIZE + HEADER_ICON_GAP : 0;
	return (int)Math::ceil(text_width) + HEADER_TEXT_PADDING * 2 + icon_reserve;
}

} // namespace

void ResourceTableContainer::_bind_methods() {
	BIND_ENUM_CONSTANT(SORT_NONE);
	BIND_ENUM_CONSTANT(SORT_ASCENDING);
	BIND_ENUM_CONSTANT(SORT_DESCENDING);

	ClassDB::bind_method(D_METHOD("get_selected_rows"), &ResourceTableContainer::get_selected_rows);
	ClassDB::bind_method(D_METHOD("clear_selection"), &ResourceTableContainer::clear_selection);

	ADD_SIGNAL(MethodInfo("selection_changed"));
	ADD_SIGNAL(MethodInfo("delete_requested"));
	ADD_SIGNAL(MethodInfo("sort_requested", PropertyInfo(Variant::INT, "column")));
	ADD_SIGNAL(MethodInfo("column_resizing", PropertyInfo(Variant::INT, "column"), PropertyInfo(Variant::INT, "width")));
	ADD_SIGNAL(MethodInfo("column_resize_ended", PropertyInfo(Variant::INT, "column"), PropertyInfo(Variant::INT, "width")));
}

ResourceTableContainer::ResourceTableContainer() {
	// No self-clip: header_clip/body_clip each clip their own subtree
	// already, and clipping this control's own _draw() would cut off the
	// anti-aliased edge of the rounded corners below.
	Color body_bg_color = Color(0.14, 0.14, 0.14);
	Color header_color = Color(0.2, 0.2, 0.2);
	int corner_radius = 6;

	const EditorInterface *const editor_interface = EditorInterface::get_singleton();
	if (editor_interface) {
		const Ref<Theme> editor_theme = editor_interface->get_editor_theme();
		body_bg_color = editor_theme->get_color("dark_color_2", "Editor").lerp(editor_theme->get_color("dark_color_3", "Editor"), 0.5);
		header_color = editor_theme->get_color("base_color", "Editor");
		grid_line_color = editor_theme->get_color("separator_color", "Editor");
		resize_handle_hover_color = editor_theme->get_color("highlight_color", "Editor");

		const Ref<EditorSettings> editor_settings = editor_interface->get_editor_settings();
		const int corner_radius_setting = editor_settings->get_setting("interface/theme/corner_radius");
		corner_radius = (int)std::round(corner_radius_setting * editor_interface->get_editor_scale());
	}
	body_background_style.instantiate();
	body_background_style->set_bg_color(body_bg_color);
	body_background_style->set_corner_radius(CORNER_BOTTOM_LEFT, corner_radius);
	body_background_style->set_corner_radius(CORNER_BOTTOM_RIGHT, corner_radius);

	header_background_style.instantiate();
	header_background_style->set_bg_color(header_color.lightened(HEADER_LIGHTEN_AMOUNT));
	header_background_style->set_corner_radius(CORNER_TOP_LEFT, corner_radius);
	header_background_style->set_corner_radius(CORNER_TOP_RIGHT, corner_radius);

	header_clip = memnew(Control);
	header_clip->set_clip_contents(true);
	// IGNORE: header mouse events fall through to this control's _gui_input() in content-space.
	header_clip->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	header_clip->connect("draw", callable_mp(this, &ResourceTableContainer::_on_header_draw));
	add_child(header_clip);

	body_clip = memnew(Control);
	body_clip->set_clip_contents(true);
	body_clip->connect("draw", callable_mp(this, &ResourceTableContainer::_on_body_draw));
	body_clip->connect("gui_input", callable_mp(this, &ResourceTableContainer::_on_pan_input));
	add_child(body_clip);

	body = memnew(Control);
	body->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	body_clip->add_child(body);

	scroll_hint_overlay = memnew(Control);
	scroll_hint_overlay->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	scroll_hint_overlay->connect("draw", callable_mp(this, &ResourceTableContainer::_on_scroll_hint_draw));
	body_clip->add_child(scroll_hint_overlay);

	v_scroll = memnew(VScrollBar);
	v_scroll->connect("value_changed", callable_mp(this, &ResourceTableContainer::_update_scroll));
	add_child(v_scroll);

	h_scroll = memnew(HScrollBar);
	h_scroll->connect("value_changed", callable_mp(this, &ResourceTableContainer::_update_scroll));
	add_child(h_scroll);

	// Fixed padding: the ambient theme's could leave no room for the grabber.
	v_scroll->add_theme_constant_override("padding_left", 4);
	v_scroll->add_theme_constant_override("padding_right", 4);
	h_scroll->add_theme_constant_override("padding_top", 4);
	h_scroll->add_theme_constant_override("padding_bottom", 4);

	set_process_internal(true);
	set_process_input(true);
}

void ResourceTableContainer::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_SORT_CHILDREN: {
			_resort();
		} break;

		case NOTIFICATION_INTERNAL_PROCESS: {
			_update_hover();
		} break;
	}
}

void ResourceTableContainer::set_columns(int p_columns, const std::vector<float> &p_default_widths) {
	ERR_FAIL_COND(p_columns < 1);

	const int old_size = (int)columns.size();
	columns.resize(p_columns);
	for (int i = old_size; i < p_columns; i++) {
		const float default_width = p_default_widths.empty() ? 100.0f : p_default_widths[MIN((size_t)i, p_default_widths.size() - 1)];
		columns[i].width = MAX(MIN_COLUMN_WIDTH, (int)std::round(default_width));
	}
	recompute_column_x(columns, border_x);

	queue_sort();
}

void ResourceTableContainer::set_column_width(int p_column, float p_width) {
	ERR_FAIL_INDEX(p_column, (int)columns.size());
	columns[p_column].width = MAX(MIN_COLUMN_WIDTH, (int)std::round(p_width));
	columns[p_column].is_width_auto = false;
	recompute_column_x(columns, border_x);
	queue_sort();
}

void ResourceTableContainer::set_column_header_text(int p_column, const String &p_text) {
	ERR_FAIL_INDEX(p_column, (int)columns.size());
	columns[p_column].text = p_text;
	header_clip->queue_redraw();
}

void ResourceTableContainer::set_column_sort_direction(int p_column, SortDirection p_direction) {
	ERR_FAIL_INDEX(p_column, (int)columns.size());
	columns[p_column].sort_direction = p_direction;
	header_clip->queue_redraw();
}

int ResourceTableContainer::add_row() {
	RowData row;
	row.cells.assign(columns.size(), nullptr);
	rows.push_back(row);
	queue_sort();
	return (int)rows.size() - 1;
}

void ResourceTableContainer::set_cell(int p_row, int p_column, Control *p_cell) {
	ERR_FAIL_INDEX(p_row, (int)rows.size());
	ERR_FAIL_INDEX(p_column, (int)rows[p_row].cells.size());
	if (rows[p_row].cells[p_column]) {
		body->remove_child(rows[p_row].cells[p_column]);
		rows[p_row].cells[p_column]->queue_free();
	}
	rows[p_row].cells[p_column] = p_cell;
	if (p_cell) {
		body->add_child(p_cell);
	}
	queue_sort();
}

void ResourceTableContainer::clear() {
	for (const RowData &row : rows) {
		for (Control *cell : row.cells) {
			if (cell) {
				body->remove_child(cell);
				cell->queue_free();
			}
		}
	}
	rows.clear();

	hovered_row = -1;
	queue_sort();
}

void ResourceTableContainer::_resort() {
	header_height = MAX(DEFAULT_HEADER_HEIGHT, calc_preferred_header_height(this));
	const int column_count = (int)columns.size();

	for (int i = 0; i < column_count; i++) {
		int cell_min = 0;
		float cell_max = -1.0f; // -1 == uncapped
		float natural_width = 0.0f;
		const bool is_width_auto = columns[i].is_width_auto;

		for (const RowData &row : rows) {
			Control *const cell = row.cells[i];
			if (!cell) {
				continue;
			}
			cell_min = MAX(cell_min, (int)std::ceil(cell->get_combined_minimum_size().x));
			const float max_candidate = get_cell_custom_max_width(cell);
			if (max_candidate >= 0.0f) {
				cell_max = cell_max < 0.0f ? max_candidate : MIN(cell_max, max_candidate);
			}
			if (is_width_auto) {
				natural_width = MAX(natural_width, measure_cell_natural_width(cell));
			}
		}

		const int margin_x = left_margin + right_margin;
		const int min_slot = MAX(MIN_COLUMN_WIDTH, cell_min + margin_x);
		const int max_slot = cell_max >= 0.0f ? MAX(min_slot, (int)std::ceil(cell_max) + margin_x) : -1;

		int slot = columns[i].width;
		if (is_width_auto) {
			const int header_natural = calc_column_natural_width(this, columns[i]);
			const int content_natural = (int)std::ceil(MAX(natural_width, (float)MAX(0, header_natural - margin_x)));
			slot = content_natural > 0 ? content_natural + margin_x : slot;
		}
		slot = MAX(slot, min_slot);
		if (max_slot >= 0) {
			slot = MIN(slot, max_slot);
		}

		columns[i].width = slot;
		columns[i].min_width = min_slot;
		columns[i].max_width = max_slot;
		columns[i].is_resizable = !(max_slot >= 0 && max_slot <= min_slot);
	}
	recompute_column_x(columns, border_x);

	cached_total_width = column_count > 0 ? columns[column_count - 1].x + columns[column_count - 1].width + border_x : 0;

	const int margin_y = top_margin + bottom_margin;
	int y = 0;
	for (size_t r = 0; r < rows.size(); r++) {
		int content_h = 0;
		for (const Control *cell : rows[r].cells) {
			if (cell) {
				content_h = MAX(content_h, (int)std::ceil(cell->get_combined_minimum_size().y));
			}
		}
		rows[r].height = MAX(MIN_ROW_HEIGHT, content_h + margin_y);
		rows[r].y = y;
		y += rows[r].height + border_y;
	}
	cached_total_body_height = y;

	const Vector2 size = get_size();
	const int size_x = (int)size.x;
	const int visible_body_height = MAX(0, (int)size.y - header_height);

	_update_scrollbars();

	header_clip->set_position(Vector2(0, 0));
	header_clip->set_size(Vector2(size_x, header_height));

	const int body_width = v_scroll->is_visible() ? (int)std::round(v_scroll->get_position().x) : size_x;
	const int body_height = h_scroll->is_visible() ? (int)std::round(h_scroll->get_position().y) - header_height : visible_body_height;

	body_clip->set_position(Vector2(0, header_height));
	body_clip->set_size(Vector2(body_width, body_height));
	body->set_position(Vector2(-cached_h_offset, -cached_v_offset));
	body->set_size(Vector2(cached_total_width, cached_total_body_height));

	scroll_hint_overlay->set_position(Vector2(0, 0));
	scroll_hint_overlay->set_size(Vector2(body_width, body_height));

	for (size_t r = 0; r < rows.size(); r++) {
		const int row_y = rows[r].y;
		const int row_h = rows[r].height;
		for (int c = 0; c < column_count; c++) {
			Control *const cell = rows[r].cells[c];
			if (!cell) {
				continue;
			}
			const int slot_x = columns[c].x;
			const int slot_w = columns[c].width;
			const int cell_w = MAX(0, slot_w - left_margin - right_margin);
			const int cell_h = MAX(0, row_h - top_margin - bottom_margin);
			cell->set_position(Vector2(slot_x + left_margin, row_y + top_margin));
			cell->set_size(Vector2(cell_w, cell_h));
		}
	}

	header_clip->queue_redraw();
	body_clip->queue_redraw();
	scroll_hint_overlay->queue_redraw();
	queue_redraw();
}

void ResourceTableContainer::_update_hover() {
	const Vector2 local = body_clip->get_local_mouse_position();
	const bool has_mouse_moved = !is_hover_last_valid || local != hover_last_local_mouse;
	const bool has_content_scrolled = cached_h_offset != hover_last_h_offset || cached_v_offset != hover_last_v_offset;
	hover_last_local_mouse = local;
	hover_last_h_offset = cached_h_offset;
	hover_last_v_offset = cached_v_offset;
	is_hover_last_valid = true;

	int new_hover = -1;
	if (!(has_content_scrolled && !has_mouse_moved) && !rows.empty()) {
		if (Rect2(Vector2(), body_clip->get_size()).has_point(local)) {
			const int y = (int)local.y + cached_v_offset;
			for (size_t r = 0; r < rows.size(); r++) {
				if (y >= rows[r].y && y < rows[r].y + rows[r].height) {
					new_hover = (int)r;
					break;
				}
			}
		}
	}
	if (new_hover != hovered_row) {
		hovered_row = new_hover;
		body_clip->queue_redraw();
	}

	const Vector2 header_local = header_clip->get_local_mouse_position();
	const int new_hovered_separator = (header_local.y >= 0 && header_local.y < header_clip->get_size().y) ? separator_at_x(columns, border_x, (int)header_local.x + cached_h_offset) : -1;
	if (new_hovered_separator != hovered_separator) {
		hovered_separator = new_hovered_separator;
		header_clip->queue_redraw();
	}
	set_default_cursor_shape(hovered_separator >= 0 || dragging_separator >= 0 ? Control::CURSOR_HSPLIT : Control::CURSOR_ARROW);

	// DisplayServer, not Input: a button release outside the whole editor
	// window never updates Input's own cached mask.
	const bool is_left_pressed = DisplayServer::get_singleton()->mouse_get_button_state().has_flag(MOUSE_BUTTON_MASK_LEFT);
	if ((dragging_separator != -1 || press_column != -1) && !is_left_pressed) {
		if (dragging_separator != -1) {
			const int column = dragging_separator;
			dragging_separator = -1;
			emit_signal("column_resize_ended", column, columns[column].width);
		}
		press_column = -1;
	}

	// A pan release outside the whole editor window never reaches _input().
	if (is_pan_dragging && !DisplayServer::get_singleton()->mouse_get_button_state().has_flag(MOUSE_BUTTON_MASK_MIDDLE)) {
		is_pan_dragging = false;
	}
}

void ResourceTableContainer::_draw() {
	const Vector2 size = get_size();
	draw_style_box(header_background_style, Rect2(Vector2(0, 0), Vector2(size.x, header_height)));
	draw_style_box(body_background_style, Rect2(Vector2(0, header_height), Vector2(size.x, size.y - header_height)));
}

void ResourceTableContainer::_on_header_draw() {
	const int h = (int)header_clip->get_size().y;

	for (int i = 0; i < (int)columns.size(); i++) {
		const int x = columns[i].x - cached_h_offset;
		_draw_header_column(i, x, columns[i].width, h);

		const int gap_x = x + columns[i].width;
		header_clip->draw_rect(Rect2(Vector2(gap_x, 0), Vector2(border_x, h)), grid_line_color);
		if (columns[i].is_resizable && (hovered_separator == i || dragging_separator == i)) {
			const int center = gap_x + border_x / 2;
			header_clip->draw_rect(Rect2(Vector2(center - RESIZE_HANDLE_WIDTH / 2, 0), Vector2(RESIZE_HANDLE_WIDTH, h)), resize_handle_hover_color);
		}
	}
}

void ResourceTableContainer::_draw_header_column(int p_index, int p_x, int p_width, int p_height) {
	const Column &column = columns[p_index];

	Ref<Font> font = get_theme_font(StringName("font"), StringName("Label"));
	if (!font.is_valid()) {
		font = get_theme_default_font();
	}
	int font_size = get_theme_font_size(StringName("font_size"), StringName("Label"));
	if (font_size <= 0) {
		font_size = get_theme_default_font_size();
	}
	const Color text_color = get_theme_color(StringName("font_color"), StringName("Label"));

	const bool has_icon = column.sort_direction != SORT_NONE;
	const int icon_reserve = has_icon ? HEADER_ICON_SIZE + HEADER_ICON_GAP : 0;
	const int text_area_width = MAX(0, p_width - HEADER_TEXT_PADDING * 2 - icon_reserve);

	const float ascent = font->get_ascent(font_size);
	const float descent = font->get_descent(font_size);
	const float baseline_y = (p_height - (ascent + descent)) * 0.5f + ascent;

	header_clip->draw_string(font, Vector2(p_x + HEADER_TEXT_PADDING, baseline_y), column.text, HORIZONTAL_ALIGNMENT_CENTER, text_area_width, font_size, text_color,
			TextServer::JUSTIFICATION_WORD_BOUND | TextServer::JUSTIFICATION_TRIM_EDGE_SPACES | TextServer::JUSTIFICATION_CONSTRAIN_ELLIPSIS);

	if (has_icon) {
		const Ref<Texture2D> icon = get_theme_icon(column.sort_direction == SORT_ASCENDING ? StringName("GuiTreeArrowDown") : StringName("GuiArrowUp"), StringName("EditorIcons"));
		if (icon.is_valid()) {
			const int icon_x = p_x + p_width - HEADER_TEXT_PADDING - HEADER_ICON_SIZE;
			const int icon_y = (p_height - HEADER_ICON_SIZE) / 2;
			header_clip->draw_texture_rect(icon, Rect2(Vector2(icon_x, icon_y), Vector2(HEADER_ICON_SIZE, HEADER_ICON_SIZE)), false);
		}
	}
}

void ResourceTableContainer::_on_body_draw() {
	const Vector2 clip_size = body_clip->get_size();
	const int bg_width = MAX(cached_total_width, (int)clip_size.x);
	const int bg_height = MAX(cached_total_body_height, (int)clip_size.y);

	for (const int r : selected_rows) {
		if (r >= (int)rows.size()) {
			break;
		}
		const int row_y = rows[r].y - cached_v_offset;
		if (row_y + rows[r].height >= 0 && row_y <= clip_size.y) {
			body_clip->draw_rect(Rect2(Vector2(-cached_h_offset, row_y), Vector2(bg_width, rows[r].height)), row_selected_color);
		}
	}

	if (hovered_row >= 0 && hovered_row < (int)rows.size()) {
		const int row_y = rows[hovered_row].y - cached_v_offset;
		const int row_h = rows[hovered_row].height;
		if (row_y + row_h >= 0 && row_y <= clip_size.y) {
			body_clip->draw_rect(Rect2(Vector2(-cached_h_offset, row_y), Vector2(bg_width, row_h)), row_hover_color);
		}
	}

	for (int i = 0; i < (int)columns.size(); i++) {
		const int gap_x = columns[i].x + columns[i].width - cached_h_offset;
		body_clip->draw_rect(Rect2(Vector2(gap_x, 0), Vector2(border_x, bg_height)), grid_line_color);
	}
	const int row_line_width = MAX(0, bg_width - GRID_LINE_MARGIN * 2);
	for (size_t r = 0; r < rows.size(); r++) {
		const int gap_y = rows[r].y + rows[r].height - cached_v_offset;
		body_clip->draw_rect(Rect2(Vector2(-cached_h_offset + GRID_LINE_MARGIN, gap_y), Vector2(row_line_width, border_y)), grid_line_color);
	}
}

void ResourceTableContainer::_on_scroll_hint_draw() {
	if (cached_v_offset <= 0) {
		return;
	}
	const Ref<Texture2D> scroll_hint = scroll_hint_overlay->get_theme_icon("scroll_hint", "Tree");
	if (!scroll_hint.is_valid()) {
		return;
	}
	const Color scroll_hint_color = scroll_hint_overlay->get_theme_color("scroll_hint_color", "Tree");
	const int hint_height = scroll_hint->get_height();
	scroll_hint_overlay->draw_texture_rect(scroll_hint, Rect2(Vector2(0, 0), Vector2(scroll_hint_overlay->get_size().x, hint_height)), false, scroll_hint_color);
}

void ResourceTableContainer::_update_scrollbars() {
	is_updating_scroll = true;

	const Vector2 size = get_size();
	const int visible_width = (int)size.x;
	const int visible_body_height = MAX(0, (int)size.y - header_height);

	const bool is_v_needed = cached_total_body_height > visible_body_height;
	const bool is_h_needed = cached_total_width > visible_width;

	v_scroll->set_visible(is_v_needed);
	h_scroll->set_visible(is_h_needed);

	const int v_length = visible_body_height - (is_h_needed ? SCROLLBAR_WIDTH : 0);
	v_scroll->set_position(Vector2(visible_width - SCROLLBAR_WIDTH, header_height));
	v_scroll->set_size(Vector2(SCROLLBAR_WIDTH, MAX(0, v_length)));

	const int h_length = visible_width - (is_v_needed ? SCROLLBAR_WIDTH : 0);
	h_scroll->set_position(Vector2(0, header_height + visible_body_height - SCROLLBAR_WIDTH));
	h_scroll->set_size(Vector2(MAX(0, h_length), SCROLLBAR_WIDTH));

	if (is_v_needed) {
		v_scroll->set_max(cached_total_body_height);
		v_scroll->set_page(visible_body_height);
	}
	if (is_h_needed) {
		h_scroll->set_max(cached_total_width);
		h_scroll->set_page(visible_width);
	}

	cached_h_offset = is_h_needed ? (int)std::round(h_scroll->get_value()) : 0;
	cached_v_offset = is_v_needed ? (int)std::round(v_scroll->get_value()) : 0;

	is_updating_scroll = false;
}

void ResourceTableContainer::_update_scroll(double p_value) {
	if (is_updating_scroll) {
		return;
	}
	queue_sort();
}

void ResourceTableContainer::_set_selection(const std::set<int> &p_rows) {
	if (p_rows == selected_rows) {
		return;
	}
	selected_rows = p_rows;
	body_clip->queue_redraw();
	emit_signal("selection_changed");
}

PackedInt32Array ResourceTableContainer::get_selected_rows() const {
	PackedInt32Array result;
	for (const int r : selected_rows) {
		result.push_back(r);
	}
	return result;
}

void ResourceTableContainer::clear_selection() {
	selection_anchor = -1;
	_set_selection({});
}

void ResourceTableContainer::_on_pan_input(const Ref<InputEvent> &p_event) {
	const Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_null() || !mb->is_pressed()) {
		return;
	}

	switch (mb->get_button_index()) {
		case MOUSE_BUTTON_WHEEL_UP:
			v_scroll->set_value(v_scroll->get_value() - MAX(1.0, v_scroll->get_page() * 0.2));
			break;
		case MOUSE_BUTTON_WHEEL_DOWN:
			v_scroll->set_value(v_scroll->get_value() + MAX(1.0, v_scroll->get_page() * 0.2));
			break;
		case MOUSE_BUTTON_WHEEL_LEFT:
			h_scroll->set_value(h_scroll->get_value() - MAX(1.0, h_scroll->get_page() * 0.2));
			break;
		case MOUSE_BUTTON_WHEEL_RIGHT:
			h_scroll->set_value(h_scroll->get_value() + MAX(1.0, h_scroll->get_page() * 0.2));
			break;
		default:
			break;
	}
}

// Node's _input(), not "gui_input": a cell under the mouse (e.g. a live
// EditorProperty) would otherwise claim the press first.
void ResourceTableContainer::_input(const Ref<InputEvent> &p_event) {
	const Ref<InputEventKey> key = p_event;
	if (key.is_valid() && key->is_pressed() && !key->is_echo() && key->get_keycode() == KEY_ESCAPE) {
		Control *const focus = get_viewport()->gui_get_focus_owner();
		const bool is_editing_text = focus && is_ancestor_of(focus) && Object::cast_to<LineEdit>(focus);
		if (!is_editing_text && get_global_rect().has_point(get_global_mouse_position())) {
			clear_selection();
		}
		return;
	}

	if (key.is_valid() && key->is_pressed() && !key->is_echo() && key->get_keycode() == KEY_DELETE) {
		Viewport *const viewport = get_viewport();
		Control *const focus = viewport->gui_get_focus_owner();
		const bool is_editing_text = focus && (Object::cast_to<LineEdit>(focus) || Object::cast_to<TextEdit>(focus));
		if (!is_editing_text && !selected_rows.empty() && get_global_rect().has_point(get_global_mouse_position())) {
			emit_signal("delete_requested");
			viewport->set_input_as_handled();
		}
		return;
	}

	const Ref<InputEventMouseButton> click = p_event;
	const MouseButton click_button = click.is_valid() ? click->get_button_index() : MOUSE_BUTTON_NONE;
	if (click.is_valid() && click->is_pressed() && (click_button == MOUSE_BUTTON_LEFT || click_button == MOUSE_BUTTON_RIGHT)) {
		const int row = row_at_global_position(rows, body_clip, v_scroll, h_scroll, cached_v_offset, click->get_global_position());
		if (row < 0) {
			return;
		}
		std::set<int> selection = selected_rows;
		if (click->get_button_index() == MOUSE_BUTTON_RIGHT) {
			if (!selection.count(row)) {
				selection = { row };
				selection_anchor = row;
			}
		} else if (click->is_shift_pressed() && selection_anchor >= 0) {
			selection.clear();
			for (int r = MIN(selection_anchor, row); r <= MAX(selection_anchor, row); r++) {
				selection.insert(r);
			}
		} else if (click->is_ctrl_pressed()) {
			if (!selection.erase(row)) {
				selection.insert(row);
			}
			selection_anchor = row;
		} else {
			selection = { row };
			selection_anchor = row;
		}
		_set_selection(selection);
		return;
	}

	const Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_valid() && mb->get_button_index() == MOUSE_BUTTON_MIDDLE) {
		if (mb->is_pressed()) {
			const Vector2 global_position = mb->get_global_position();
			if (!get_global_rect().has_point(global_position)) {
				return;
			}
			is_pan_dragging = true;
			pan_drag_start_mouse = global_position;
			pan_drag_start_h_value = (int)std::round(h_scroll->get_value());
			pan_drag_start_v_value = (int)std::round(v_scroll->get_value());
			get_viewport()->set_input_as_handled();
		} else if (is_pan_dragging) {
			is_pan_dragging = false;
			get_viewport()->set_input_as_handled();
		}
		return;
	}

	const Ref<InputEventMouseMotion> mm = p_event;
	if (mm.is_valid() && is_pan_dragging) {
		const Vector2 delta = mm->get_global_position() - pan_drag_start_mouse;
		h_scroll->set_value(pan_drag_start_h_value - delta.x);
		v_scroll->set_value(pan_drag_start_v_value - delta.y);
		get_viewport()->set_input_as_handled();
	}
}

// Header strip's only input path (header_clip ignores the mouse). Resize beats sort on the same press.
void ResourceTableContainer::_gui_input(const Ref<InputEvent> &p_event) {
	const Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_valid() && mb->get_button_index() == MOUSE_BUTTON_LEFT) {
		const Vector2 local_position = mb->get_position();
		const int x = (int)local_position.x + cached_h_offset;
		if (mb->is_pressed()) {
			if (local_position.y < 0 || local_position.y >= header_height) {
				return;
			}
			const int separator = separator_at_x(columns, border_x, x);
			if (separator >= 0) {
				dragging_separator = separator;
				drag_start_mouse_x = (int)mb->get_global_position().x;
				drag_start_width = columns[separator].width;
				accept_event();
				return;
			}
			press_column = column_at_x(columns, x);
			accept_event();
		} else {
			if (dragging_separator >= 0) {
				const int column = dragging_separator;
				dragging_separator = -1;
				emit_signal("column_resize_ended", column, columns[column].width);
			} else if (press_column >= 0) {
				if (column_at_x(columns, x) == press_column) {
					emit_signal("sort_requested", press_column);
				}
				press_column = -1;
			}
			accept_event();
		}
		return;
	}

	const Ref<InputEventMouseMotion> mm = p_event;
	if (mm.is_valid() && dragging_separator >= 0) {
		const int delta = (int)mm->get_global_position().x - drag_start_mouse_x;
		int new_width = MAX(MAX(MIN_COLUMN_WIDTH, columns[dragging_separator].min_width), drag_start_width + delta);
		const int max_width = columns[dragging_separator].max_width;
		if (max_width >= 0) {
			new_width = MIN(new_width, max_width);
		}
		columns[dragging_separator].width = new_width;
		columns[dragging_separator].is_width_auto = false;
		recompute_column_x(columns, border_x);
		emit_signal("column_resizing", dragging_separator, new_width);
		queue_sort();
		return;
	}

	// Wheel scroll only reaches here (a header column body doesn't claim
	// input the way a body-row cell, e.g. a live EditorProperty, would).
	_on_pan_input(p_event);
}

