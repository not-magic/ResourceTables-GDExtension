# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A Godot 4 GDExtension (native C++ addon, built against `godot-cpp`), named `ResourceTables`.
It adds a "Resources" bottom panel to the editor (`ResourceTablesPlugin`) for browsing
and editing `Resource` instances as spreadsheet-like tables.

The panel has a single dropdown, labeled by an `Object` editor icon, listing (alphabetically, case-insensitive)
every resource class name with at least one instance found anywhere under `res://` (a live scan, not a
generated cache), plus every project/plugin global class deriving from `Resource` even with no instances, excluding `ResourceTable` types themselves (see below) -- showing every resource
of that type project-wide. Each item's metadata is just the class name; a selection is tracked
across refreshes by item text. `ResourceTableGenerator` scripts are not listed in it at all -- they
only take part through the Tools menu's "Generate ResourceTables" and "New ResourceTable Generator...". The top row is laid out
compacted to the left (labels/dropdowns/buttons sized to content, an expanding spacer control
absorbing the rest) with the "Tools" `MenuButton` (New ResourceTable Generator.../Generate ResourceTables/Export/Import CSV) pinned to the
right after that spacer; "Add" (creates a new resource of the dropdown's type via a plain save-file
prompt) is a `Button` next to the dropdown.

A `ResourceTableGenerator` subclass (see below) has a single job: `_generate_outputs()`, an
`@abstract` method GDScript itself enforces and the script editor autocompletes the same way it does
`_ready()`/`_process()` for a `Node`, which writes whatever the generator wants to disk, in whatever
format it wants (the editor doesn't care). See `new_resource_table_generator_template.txt` below for
the default implementation "New ResourceTable Generator..." stubs in -- it writes a
`GenericResourceTable` (`project/addons/ResourceTables/GenericResourceTable.gd`, `extends
ResourceTable`, adding a single `@export var items: Array`).
`ResourceTable` itself (`src/resource_table.h`) is a thin native marker class, just `Resource`
with no members -- its only job is letting
`ResourceTableUtils::find_resource_type_names()` recognize and exclude its own
instances (`resource->is_class("ResourceTable")`) from the dropdown, since a generator's own output
table isn't itself a browsable resource type. Nothing in `src/` ever constructs or inspects a
`GenericResourceTable`'s own `items` by name -- that property, and the whole
generator/output-table split, lives entirely in `project/addons/ResourceTables/*.gd` and could be
replaced with a different scheme (a different output class, ...) without touching the C++ side at all.

The data area is a single `ResourceTableContainer` (`src/resource_table_container.{h,cpp}`), a general-purpose,
reusable `Container` (registered at `MODULE_INITIALIZATION_LEVEL_SCENE` with a plain
`GDREGISTER_CLASS`, not editor-only -- it's usable in an exported game's own scenes too, unlike
`ResourceTablesPlugin`). It owns its own header row (fixed vertically, scrolling horizontally with
the body), a body that scrolls both ways with its own scrollbars, per-row background/hover-
highlight drawing, grid lines between cells, and user column drag-resize -- none of which
`ResourceTablesPlugin` has to implement itself. Loosely modeled on Godot's own `GridContainer`
(`scene/gui/grid_container.cpp`, used as a reference for the row-major column-width/row-height
accounting) in spirit, but every column width is explicit, caller/user-set state rather than
derived from children's minimum sizes, and the header/body are two separately clipped regions
instead of one grid. `ResourceTablesPlugin` only ever calls its own `clear()`/`set_columns()`/
`set_column_width()`/`set_column_header_text()`/`set_column_sort_direction()`/`add_row()`/
`set_cell()` API (see `_rebuild_table`); building a row still means asking
`EditorInspector::instantiate_property_editor` for a live `EditorProperty` per property (see
below) -- the header row itself, by contrast, has no per-column child `Control` at all (see below).

Internally, `ResourceTableContainer` has two child `Control`s, `header_clip` and `body_clip`, both with
`set_clip_contents(true)`, each sized/positioned every `_resort()` (its `NOTIFICATION_SORT_CHILDREN`
handler -- Container's own compiled `NOTIFICATION_RESIZED` handling, which calls `queue_sort()`,
still runs for a GDExtension subclass alongside this class' own override, since `_notification` is
broadcast to every level of the hierarchy including the extension instance, unlike single-dispatch
virtuals unrelated to notifications). Splitting the header from the body this way is what keeps the
header fixed in place (not scrolling vertically with the data) while the body scrolls under it, and
what stops a scrolled-up body row from visually intruding into the header's own space (`body_clip`'s
rect starts right below the header, so its `clip_contents` physically excludes that area) --
`header_clip` is horizontally offset by the same scroll value as the body, so the header still
tracks a wide table's horizontal scroll despite being vertically fixed. `header_clip` has no
children at all -- its own text/icons/resize highlight are custom-drawn directly (see below); row
cells are children of `body_clip`. Both regions, plus the two scrollbars, get their positions/sizes
recomputed from scratch every `_resort()` call (columns are, in order: Name -- table column 0 is
still a separate, title-less revert-to-disk icon column of its own, a plain `Button` built fresh
per row by `_rebuild_table`, not owned by `ResourceTableContainer` or `ResourceTableName` -- then
one column per property; `sort_column` on `ResourceTablesPlugin`'s side, 0 == Name, 1+i ==
properties[i], is offset by one from `ResourceTableContainer`'s own 0-based column indices as a
result, a conversion `_on_sort_header_pressed` is the one place that happens).

Every column's state (text, sort direction, x/width, auto-fit vs. pinned, min/max, resizable) is a
single `Column` struct, one per index in a `std::vector<Column> columns` -- "the single
authoritative record of a column's state" (see that struct's own comment) rather than several
parallel arrays. A column's `width` defaults to auto-fitting the widest cell actually in it --
header text included, so a long column title doesn't get clipped just because no row happens to
need that much width -- recomputed at the top of every `_resort()` for any column whose own
`is_width_auto` is still true. A column starts out auto (`set_columns` only assigns a *new* index,
beyond whatever was already stored, a starting fallback width from its own `default_widths`
argument, used only until that column actually has cells to measure) and stays that way until the
user drag-resizes it or a caller calls `set_column_width()` directly, either of which sets
`width_is_auto = false` and pins `width` from then on -- existing columns are otherwise left
completely alone across a `set_columns()` call. Since `ResourceTablesPlugin::_rebuild_table` calls
`clear()` then `set_columns()` on nearly every interaction (its longstanding rebuild-everything
policy), this pinning is what makes a user's column drag-resize actually stick rather than being
silently reset back to auto-fit the next time anything else about the table changes -- `clear()`
itself deliberately never touches `columns`, only the rows built from it.

Measuring a cell's "natural" width for this auto-fit pass needs more than a plain
`get_combined_minimum_size().x`, because the Name column's own `Button` (inside `ResourceTableName`)
sets `set_clip_text(true)` (so its text can still degrade gracefully if the column is later dragged
narrower than it) -- and a clip-text `Button`'s minimum size deliberately excludes its text's own
width, since that's exactly what lets it be sized smaller in the first place. Left unaccounted for,
auto-fit would read a clip-text cell as needing next to no width at all and squash that whole
column down to `MIN_COLUMN_WIDTH`. `_measure_cell_natural_width()` (file-local, in
`resource_table_container.cpp`) handles this two ways: first it checks -- dynamically, via
`has_method()`, not a compile-time cast, since `ResourceTableContainer` doesn't know about
`ResourceTableName` by name (it's registered at scene level, meant to stay a general-purpose,
addon-agnostic `Container`) -- whether the cell exposes a `calc_natural_width()` method and trusts
whatever it returns if so; otherwise it falls back to `Object::cast_to<Button>`, and if the cast
succeeds and `get_clip_text()` is on, briefly toggling it off, reading
`get_combined_minimum_size().x`, then restoring it. `ResourceTableName::calc_natural_width()` (see
below) implements the same toggle-and-measure trick internally for its own private `Button`, since
that one isn't reachable from outside the class at all.

Column widths also respect whatever min/max a column's own cells declare, regardless of whether
that column is still auto-fitting or has already been pinned: after the auto-fit assignment above,
every column is clamped to never be narrower than the widest cell's own real
`get_combined_minimum_size().x` (deliberately more permissive than the auto-fit target -- a
clip-text cell's real minimum is smaller than what auto-fit prefers, by design, so dragging a
column narrower than its "nice" auto-fit width is still possible down to that floor) and never
wider than the tightest "Custom Maximum Size" (`Control::get_custom_maximum_size()`) any cell in
the column declares, if any (a cell with no cap set doesn't constrain the column at all). Both
`Control::get_custom_maximum_size()` and `set_custom_maximum_size()` are real, `ClassDB`-bound
`Control` methods (confirmed in `scene/gui/control.cpp`) that just aren't exposed as typed methods
by this project's checked-out `godot-cpp` snapshot, so `_get_cell_custom_max_width()`
(`resource_table_container.cpp`) calls the getter dynamically (`p_cell->call("get_custom_maximum_size")`,
returning a `Vector2` whose negative `x` means "uncapped", Control's own convention) rather than
through a generated binding. A column whose min and max end up equal (no wiggle room left at all)
sets its own `resizable = false`, which `_separator_at_x()` (see below) checks to skip that
column's own resize separator entirely, rather than leave one that would just visibly snap back to
the same width every time it's dragged.

Column drag-resize has no dedicated overlay `Control`s of its own at all -- there's nothing to
mis-order, z-fight, or leak a stuck hover/drag state from, unlike a design built out of one Control
per resize handle. `_separator_at_x(x)` and `_column_at_x(x)` are pure math over `columns`, called
from both `_gui_input` (press/release/drag) and `_update_hover()` (per-frame highlight polling) --
a press within `RESIZE_HANDLE_WIDTH` of a column's right edge starts a resize
(`dragging_separator`), tracked by `drag_start_mouse_x`/`drag_start_width` and updated from the
accumulated global-X delta on every `InputEventMouseMotion` while the button stays held (relying on
the same "input stays routed to whichever control received the initial press, regardless of where
the cursor physically moves" behavior sliders/scrollbars use natively); a press anywhere else in a
column's own header area starts a sort-click instead (`press_column`), confirmed by
`_column_at_x()` again on release matching the same column before emitting `"sort_requested"` --
this is what a `Button`'s own `BaseButton` click detection would otherwise give for free, since
there's no per-column `Button` here to provide it. Resize takes priority over sort when both would
apply to the same press: landing within a separator's own hit region always starts a resize.

Both `hovered_separator` (paints `resize_handle_hover_color` across that separator's own strip in
`_on_header_draw`) and `dragging_separator` are read/cleared entirely from `_update_hover()`'s own
per-frame `NOTIFICATION_INTERNAL_PROCESS` poll, the same poll `hovered_row` uses (see below) --
recomputed fresh from `header_clip->get_local_mouse_position()` every tick rather than from
discrete enter/exit/release signals (there being no per-separator Control to raise them from
regardless), so a resize or scroll release outside the whole editor window can't leave a highlight
or a drag stuck active: `_update_hover()` separately checks
`DisplayServer::get_singleton()->mouse_get_button_state()` (a live query, not `Input`'s own cached
mask, since a release outside the whole editor window never updates that) as a safety net, and
force-ends a resize/pan drag the moment it notices the button genuinely isn't held anymore instead
of waiting for a release event that may never arrive.

`header_height` is a floor (`DEFAULT_HEADER_HEIGHT`, 26px), not a fixed value -- recomputed at the
top of every `_resort()` via `_calc_preferred_header_height()` (font ascent+descent plus
`HEADER_VERTICAL_PADDING` on each side) to also grow to fit the header's own text at whatever font
size the editor theme is currently using, so a larger editor font doesn't get its header text
clipped against a hardcoded height.

Every column header's text and sort-direction icon are drawn directly by `_draw_header_column()`
(called from `_on_header_draw()`, connected to `header_clip`'s own inherited `CanvasItem` `"draw"`
signal) -- there is no per-column header `Control` of any kind, `Button`-based or otherwise; text
is drawn with `draw_string()` (centered, word-bound/ellipsis-constrained justification) and the
sort icon, if any, with `draw_texture_rect()` (`"GuiTreeArrowDown"`/`"GuiArrowUp"` from the
`"EditorIcons"` theme, right-aligned). `_calc_column_natural_width()` mirrors this exact layout
(text width via `Font::get_string_size()`, plus `HEADER_TEXT_PADDING` on each side and an icon
reserve when sorted) so the auto-fit pass above agrees with what actually gets drawn. The header
background itself (`header_background_style`, a `StyleBoxFlat` lightened off the editor theme's own
`base_color` by `HEADER_LIGHTEN_AMOUNT`, corners rounded to the editor's own configured corner
radius) is drawn once across the full header strip in `ResourceTableContainer::_draw()`, so it
always covers the panel's full width regardless of how many columns there are or how far they
reach -- no separate trailing "filler" column is needed for that.

Row cells are sized to their own natural (`get_combined_minimum_size()`) height and vertically
centered within their row, rather than stretched to fill it, since a row's own height is the
tallest of all its cells (see `_resort()`'s row-height pass) -- every other, shorter cell in that
row would otherwise end up stretched taller than it actually needs, and whether its content then
*looks* centered would depend entirely on that specific cell's own internal alignment behavior,
which this class has no control over for an arbitrary, caller-supplied cell Control. Centering the
cell's own rect within the row instead makes this correct unconditionally, regardless of what kind
of Control ends up in a cell.

The hovered-row highlight and every grid line are custom-drawn (`_on_header_draw`/`_on_body_draw`)
rather than drawn by a separate background `Control` per row. A per-row background `Control` was
the first approach tried here and was deliberately abandoned: since row cells (Buttons,
`EditorProperty`s) sit on top of it and are the topmost hit target wherever the mouse actually is,
a background-only `Control`'s own `mouse_entered`/`mouse_exited` would only ever fire in the thin
gaps between cells, not "anywhere across the row" the way a hover highlight should feel. Instead, `hovered_row` is tracked by polling
`body_clip->get_local_mouse_position()` every `NOTIFICATION_INTERNAL_PROCESS` tick
(`_update_hover()`, `set_process_internal(true)` in the constructor) and comparing it against each
row's cached rect -- independent of any child cell's own input handling or mouse filter, at the
cost of a per-frame linear scan over rows, cheap at this addon's scale. There's no zebra striping
-- every row shares the same flat `body_background_style` background, with only the hovered row
(`row_hover_color`, `Color(1, 1, 1, 0.12)`) and grid lines (`grid_line_color`, pulled from the
editor theme's own `separator_color`) drawn on top. Grid lines are drawn at each column boundary
(spanning the region's full height) and each row boundary (spanning its full width) after the row
backgrounds, so they sit on top of them.

`v_scroll`/`h_scroll` are real `VScrollBar`/`HScrollBar` nodes, used as-is: their own drawing,
dragging, and grabber hover/pressed states are theirs, not reimplemented. `ResourceTableContainer`
only lays them out (`_resort()`, flush against the container's own edges, `SCROLLBAR_WIDTH` -- 14px
-- thick with a fixed 4px theme padding override on each side rather than the ambient theme's own,
which could otherwise leave little to no room for the grabber inside that width) and mirrors their
`Range` value into its own `cached_h_offset`/`cached_v_offset` fields.

**Rule: don't reimplement an engine Control to work around a rendering/behavior problem.** An
earlier version of this class custom-drew and custom-dragged both scrollbars from scratch after a
real `VScrollBar`/`HScrollBar` first appeared not to render. That reimplementation was removed and
replaced with the real controls per explicit instruction: if a built-in Control seems broken or
insufficient, stop and report the specific blocker rather than building a workaround -- the choice
of workaround (if any) is the user's to make, not something to reach for silently. Mouse wheel over
the header or body (`_on_pan_input`) still scrolls via `"gui_input"`, the same approach
`ScrollContainer::gui_input` uses, with the same well-known limitation: a wheel event over a child
cell that's eligible for input is consumed by that cell first and never reaches here.

Each property column header's text is the same human-readable capitalization the Inspector uses
for a property's label (`String(name).capitalize()`, e.g. `"int_val"` -> `"Int Val"`) rather than
the raw property name -- `String::capitalize()` implements the same underlying
split-on-underscore-and-titlecase transform the Inspector's real
`EditorPropertyNameProcessor::_capitalize_name` uses, just without its acronym remap table (e.g.
`"id"` -> `"ID"`) and stop-word lowercasing, since `EditorPropertyNameProcessor` itself isn't
exposed to GDExtension (confirmed by reading `editor/inspector/editor_property_name_processor.h`
in Godot's own source -- it's not even a registered class, just a plain singleton) -- close enough
for ordinary property names, and exactly matching for any name without underscored acronyms or
articles/prepositions in it.

Each row's Name cell is a `ResourceTableName` (`src/resource_table_name.{h,cpp}`, a small reusable
widget, `GDREGISTER_INTERNAL_CLASS`'d since `ResourceTablesPlugin` is its only caller) rather than a
plain `Button`. Table column 0, the revert-to-disk icon, is a separate plain `Button` built fresh
per row by `_rebuild_table` itself (see below) -- `ResourceTableName` owns only the Name cell
proper: a flat, clip-text `Button` (`name_label`, actually a `Label` with `MOUSE_FILTER_PASS`, not
a `Button` -- see its own drag-data override below) and a `LineEdit` used for inline renaming,
swapped in over the same sub-rect and hidden until `begin_rename()`. Dragging the Name cell
(`_get_drag_data()`, reachable only because `name_label` uses `MOUSE_FILTER_PASS` instead of a
plain `Label`'s default `MOUSE_FILTER_IGNORE`, which `Viewport`'s drag-source walk-up requires)
returns the same `{"type": "resource", "resource": ...}` shape `EditorResourcePicker`'s own drop
targets (e.g. an Inspector property slot) already accept, with a preview matching
`EditorNode::drag_resource()`'s own (an icon over a label). `ResourceTableName` owns and pops its
own right-click context menu entirely -- see below -- rather than only detecting the click and
handing off to `ResourceTablesPlugin`: `begin_rename()` swaps the `Button` out for the `LineEdit`
*in place*, pre-filled with the current text, focused, with its text selected, and Enter
(`"text_submitted"`) or clicking away (`"focus_exited"`) commits (`_commit_rename()`); Escape
(checked in a `"gui_input"` connection on the `LineEdit`, since `LineEdit` has no dedicated "cancel"
signal of its own) cancels instead, reverting to the original text with no further action at all.
`_commit_rename()` performs the actual on-disk rename itself (`DirAccess::rename_absolute()` plus
its `.uid` sidecar, `Resource::take_over_path()`, then `EditorFileSystem::scan()`) rather than
emitting a signal for `ResourceTablesPlugin` to act on -- that `scan()` call is enough to reach
`ResourceTablesPlugin`'s own existing `"filesystem_changed"` listener, which already rebuilds the
table and reconciles the dirty-mtime map for edits made outside this class entirely (e.g. a rename
via the FileSystem dock), so no dedicated signal back to the plugin is needed for this either.

`ResourceTableContainer`'s own auto-fit sizing (`_measure_cell_natural_width`, see above) can't
reach into `name_label` directly since it's private to `ResourceTableName` -- that's what its own
`calc_natural_width()` (duck-typed via `has_method()`) answers instead, doing the same
toggle-`clip_text`-off-and-back-on measurement internally. `ResourceTableName` also overrides
`_get_minimum_size()` (`Control`'s own overridable minimum-size hook, not the public non-virtual
`get_minimum_size()`) to return `name_label`'s own combined minimum size -- only a `Container`
subclass aggregates children's sizes automatically, and `ResourceTableName` is a plain `Control`,
so without this override `ResourceTableContainer::_resort()`'s own vertical-centering math for row
cells would see `(0, 0)` here regardless of what `name_label` actually needs.

Every property cell is a live `EditorProperty` -- this file does not hand-roll a widget per
`Variant::Type`. Building a row asks `EditorInspector::instantiate_property_editor(object, type,
path, hint, hint_text, usage)` -- a static method bound to `ClassDB` (confirmed in
`editor/inspector/editor_inspector.cpp`) -- for the exact same `EditorProperty` widget class the
real Inspector would use for that property's type *and hint*, then places it via
`table->set_cell(row, 2 + j, editor)` (`set_draw_label(false)`, since the column header already
names the property). This is why adding an `enum` or `Color` export to a resource type (or anything else the
Inspector already knows how to edit) just works with no changes here: a `PROPERTY_HINT_ENUM` int
gets `EditorPropertyEnum` (a dropdown), a `Color` gets `EditorPropertyColor` (a swatch + picker),
an `@export_range` int/float gets a properly clamped `EditorPropertySpin`/`EditorPropertyRange`,
etc. -- Godot decides which widget class based on the same `type`/`hint`/`hint_string` already
sitting in `current_properties`' Dictionaries (from `ResourceTableUtils::find_properties_of_type()`).
If `instantiate_property_editor` returns null for some type/hint combination, the cell falls back
to a plain, non-interactive `Label` showing `value.stringify()` rather than leaving a gap. The
widget's `set_object_and_property()`/`update_property()` bind and refresh it against the real
resource+property; its `property_changed(property, value, field, changing)` signal (also how
`EditorInspector` itself listens to each property's widget) is what `_on_property_editor_changed`
reacts to.

`_on_property_editor_changed` has to handle continuous edits (e.g. dragging a slider inside an
embedded `EditorProperty`) carefully, because naively rebuilding the table on every
`property_changed` emission would destroy the very widget being dragged -- `EditorProperty` fires
`property_changed` with `changing=true` repeatedly during a drag and `changing=false` once on
release, exactly mirroring how `EditorInspector` itself handles the same signal. This file tracks
an edit "session" (`property_edit_session_resource`/`_property`/`_old_value`, keyed by resource+
property): on a `changing=true` tick it applies the value directly via `resource->set()` with no
undo/redo and no rebuild, remembering the value the property had when the session started; only on
the final `changing=false` tick does it call `_apply_property_edit()` with that remembered
starting value and *then* schedule a deferred `_rebuild_table()`. `_apply_property_edit()` takes
the old value as an explicit parameter (rather than reading `resource->get()` itself) because by
commit time the resource's live value already equals the new one (from the live-preview ticks),
so re-reading it would make every edit look like a no-op; it still skips applying anything if old
and new compare equal (e.g. a click that didn't actually change the value), and otherwise routes
through `get_undo_redo()->create_action()`/`add_do_property()`/`add_undo_property()`/
`commit_action()` (the same `EditorUndoRedoManager` the Inspector itself uses for property edits),
so Ctrl+Z/Ctrl+Y works on table-cell edits too -- `commit_action()` (its `p_execute` defaults to
`true`) is what actually performs the property set, so there's no separate `resource->set()` call
alongside it for the final value. Since nothing else notices an undo/redo happening, `_enter_tree`
also connects to `get_undo_redo()->connect("version_changed", ...)` to rebuild the table whenever
any undo/redo action fires anywhere in the editor (not just ones touching a currently-displayed
resource -- filtering that out isn't worth the complexity given how cheap a rebuild is), otherwise
a cell would keep showing its just-edited value even after Ctrl+Z reverted the underlying property.
`_mark_resource_dirty()` itself only does bookkeeping (recording the dirty path) and deliberately
does *not* schedule its own rebuild -- each caller decides when it's safe to do that, since the
property-editor-changed path above must not rebuild while a drag is still in progress.

Table column 0, the revert-to-disk icon, is a plain `Button` built fresh per row by `_rebuild_table`
(not owned by `ResourceTableName` at all): its icon (`"ReloadSmall"`, same as the Inspector's
per-property reset-to-default) is always assigned, but the `Button` itself is only made visible for
a resource with unsaved edits -- always building it with a real icon, just hidden when not dirty,
is what keeps the auto-fit revert column from jittering width the moment a row's dirty state flips
(a not-dirty row still needs a real `Button` with a real icon so its own
`get_combined_minimum_size()` includes the same stylebox padding a dirty row's does). Dirty state is
tracked entirely by this plugin (`dirty_resource_mtimes`) since nothing in the engine exposes an
"is this resource dirty"/"was this just saved" signal for an arbitrary `Resource` (`EditorNode`'s
`resource_saved` signal and its `EditorPlugin::notify_resource_saved()` callback both exist, but
neither is bound for GDExtension use, and `EditorNode` itself isn't exposed at all).
`_mark_resource_dirty()` -- called from both a committed property-cell edit and
`_on_inspector_property_edited` (editing the same resource in the Inspector) -- records the file's
on-disk modified-time the first time it's edited; `_on_filesystem_changed` later calls
`_check_dirty_resources_saved()`, which clears any entry whose on-disk modified-time has since moved
on, treating that as "this got saved" (by Ctrl+S, the Inspector's own save button, or anything else)
-- a heuristic, not a certainty, given there's nothing more precise to hook. Clicking the revert
button (`_on_revert_button_pressed`) clears the entry and reverts the *displayed* resource's
properties, one by one, from an independently loaded fresh copy
(`ResourceLoader.load(path, "", CACHE_MODE_IGNORE)`) rather than just reloading in place
(`CACHE_MODE_REPLACE`) -- the `.tres` saver skips writing any property that already equals its own
default at save time (`ResourceFormatSaverText::save`, confirmed in Godot's own
`scene/resources/resource_format_text.cpp`), so `CACHE_MODE_REPLACE` alone silently leaves a
since-edited in-memory value in place for any property the file happened to omit (e.g. edit a
property away from its default, then back to its default, then revert: the file never mentioned
that property either time, so a plain reload never touches it). Loading a second, independent copy
and explicitly copying every displayed property across covers that gap, since `get()` on the fresh
copy correctly returns the real default for anything the file left out.

Right-clicking a row is handled by the Name cell specifically (not any arbitrary cell in the row,
since property cells are live `EditorProperty` widgets that may want right-click for their own
purposes), and is handled entirely inside `ResourceTableName` itself -- it builds, owns, and pops
its own right-click context menu (`context_menu`, a `PopupMenu`) rather than only detecting the
click and handing off to `ResourceTablesPlugin`. The menu is ordered and iconed to match the
FileSystem dock's own per-file context menu as closely as the exposed editor API allows: Open in
Inspector (this menu's own addition -- that dock's own equivalent is its default double-click
gesture, not a menu item), Show in FileSystem, Copy Path/Copy Absolute Path/Copy UID/Rename/
Duplicate/Move-Duplicate To/Delete (icons via `get_theme_icon(name, "EditorIcons")` on
`EditorInterface::get_base_control()`, using the exact icon names `editor/docks/filesystem_dock.cpp`
uses for each -- "Copy Absolute Path" has no icon there either, so it stays icon-less here too).
There's no "Edit Dependencies.../View Owners..." here, unlike the FileSystem dock's own menu: those
open the engine's own `DependencyEditor`/`DependencyEditorOwners` dialogs, and while both classes
are registered in `ClassDB`, neither binds a single method (confirmed with
`ClassDB.class_get_method_list()` at runtime) -- there's no way to construct or drive one
reflectively from a GDExtension, and no substitute reimplementation of that specific pair of
dialogs was worth carrying here. "Show in FileSystem" is a real, exposed `FileSystemDock` method,
though (`EditorInterface::get_file_system_dock()->navigate_to_path()`, matching
`EditorResourcePicker`'s own handler for the same action). Duplicate/Move-To/Delete each pop their
own dialog (`duplicate_or_move_dialog`, an `EditorFileDialog` shared between the two, tracked by
`duplicate_or_move_is_move`; `delete_confirm_dialog`, a `ConfirmationDialog`) and perform the actual
disk operation themselves -- again relying on `EditorFileSystem::scan()` to reach
`ResourceTablesPlugin`'s own `"filesystem_changed"` listener afterward rather than a dedicated
signal, the same as renaming above.

Clicking a column header calls `_on_sort_header_pressed(logical_column)` (via
`ResourceTableContainer`'s own `"sort_requested"` signal -- see above for how a header click is
actually detected, there being no per-column `Button` to fire one natively), toggling
`is_sort_ascending` if it's already the active sort column or resetting to ascending on a new one,
then rebuilding. Dragging the boundary between two headers instead resizes that column (see
`ResourceTableContainer`'s own column drag-resize, above) -- there's still no click-and-drag column
*reordering*, only resizing.

Discovery/save primitives shared between the panel and `export_resource_tables.gd` live in
`ResourceTableUtils` (`src/resource_table_utils.{h,cpp}`), a static-method-only class exposed to
GDScript for exactly that reuse (`find_resources_of_type`, `find_generator_scripts`, `find_or_create`,
`create_instance`, `find_safe_name`, etc.). Discovering *which scripts* are generators
(`find_generator_scripts()`) walks `ProjectSettings::get_global_class_list()` exactly the way
resolving any other project global class name does elsewhere in this file (`_base_chain_inherits`,
possibly through other project global classes in between, down to a real engine class -- here,
`"ResourceTableGenerator"` itself, since that base has no native registration of its own to check
via `ClassDB::is_parent_class` except as the final link in the chain).

A `ResourceTableGenerator` subclass (`project/addons/ResourceTables/resource_table_generator.gd`,
`extends RefCounted`, `@abstract`) is deliberately not a `Resource` at all: it's a live object,
freshly instantiated per discovered script whenever generators are run, never itself saved to a
`.tres` file. Its contract is `_resource_class_name` (set by its own `_init(resource_class_name)`)
and the abstract `_generate_outputs()`. It has to be `@tool`: `Script::can_instantiate()` is false
for a non-`@tool` script while running inside the editor, and `export_resource_tables.gd`
instantiates a generator via a plain `Script.new()` (needed so `_init()` actually runs -- the same
"attach a script to an already-constructed bare object" trick `_instantiate_resource_of_type()`
uses elsewhere in this file to sidestep that exact restriction doesn't work here, since it skips
`_init()` entirely). Nothing in `src/` ever names `GenericResourceTable` or its `items` property.

`export_resource_tables.gd` finds every generator script in the project
(`ResourceTableUtils.find_generator_scripts()`), instantiates one of each, and calls
`_generate_outputs()` directly, for bulk regeneration -- it never scans for or resaves any generator
*instance*, since there's no such persisted thing to find. Its `export_tables(modified_resource := "")` (a
`Script` subclass, not an `EditorScript`, called via `script->call()` from `_run_export_tables`) runs every generator,
or only those whose `_resource_class_name` is compatible with `modified_resource` when one is given. It's called with
no argument for the "Generate ResourceTables" Tools-menu action, before a build (`EditorPlugin::_build()`), and game export (`ResourceTableExportPlugin`, an
`EditorExportPlugin` whose `_export_begin` calls back into the plugin). Automatic regeneration is per-generator: on
`filesystem_changed`, `_sync_generators()` snapshots (path -> modified-time) every resource of each generator's
`_resource_class_name` and calls `export_tables(class)` for each whose snapshot changed, or that has none yet (a
newly created generator). A generator's own output tables aren't of its handled type, so its writes can't retrigger
it. `_sync_generators(false)` at startup only records snapshots.

"New ResourceTable Generator..." (a Tools-menu item) is a single `ConfirmationDialog` (`new_generator_dialog`) with two
rows: a "Resource Class" `LineEdit` (free text -- it doesn't have to already resolve to an existing
type) with a `ClassList`-icon button that calls `EditorInterface::popup_create_dialog` (base type
`Resource`) and fills the field from its callback, and an "Output Path" `LineEdit` (defaulting to
`res://NewTable.gd`) with a `FileBrowse`-icon button that opens `new_generator_file_dialog`, a
save-mode `EditorFileDialog` used purely to fill that field. OK calls `_create_generator_script`, which
writes
`new_resource_table_generator_template.txt` to that path with its own `${RESOURCE_CLASS_NAME}`/
`${RESOURCE_TABLE_DIR}`/`${RESOURCE_TABLE_NAME}` placeholders substituted (`String::replace()`,
global by default) -- `RESOURCE_CLASS_NAME` is the Resource Class field's text, `RESOURCE_TABLE_DIR` is the saved
`.gd` file's own parent directory, full `res://...` path with a trailing slash (so a script directly
under `res://` doesn't produce `res:///`; the template writes `${RESOURCE_TABLE_DIR}${RESOURCE_TABLE_NAME}...`), and
`RESOURCE_TABLE_NAME` is that file's own basename with no path or extension. The template itself is
a plain, hand-edited `.txt` file, not GDScript this addon's own C++ has any special parsing for --
editing it (or pointing `NEW_GENERATOR_TEMPLATE_PATH` at a different file) is the only thing needed
to change what a newly-created generator subclass looks like.

The panel's Export/Import CSV actions (`ResourceTableUtils::export_csv`/`import_csv`)
read and write CSV through the vendored `csv-parser` submodule
(`csv-parser/`, see its own `AGENTS.md`), not Godot's own `FileAccess::get_csv_line()`/
`store_csv_line()`. `p_path` for these is always a real OS filesystem path -- the CSV file picker
uses `EditorFileDialog::ACCESS_FILESYSTEM` -- so `std::ifstream`/`std::ofstream` and
`csv::CSVReader`'s filename constructor can use it directly, unlike a `res://` path. csv-parser
throws on things like a malformed row or a file it can't open, so `resource_table_utils.cpp` is
the one file in `src/` compiled with C++ exceptions re-enabled (see `SConstruct`), overriding
godot-cpp's project-wide `-fno-exceptions`; every other source file keeps building exception-free.
csv-parser has no CMake target we link against -- `SConstruct` compiles its handful of `.cpp`
sources directly (mirroring `csv-parser/include/internal/CMakeLists.txt`'s source list) with
threading disabled (`CSV_ENABLE_THREADS=0` -- single-threaded is simplest/portable and plenty fast
for the small property tables this addon deals with).

## Setup

`godot-cpp` (branch `4.5`) and `csv-parser` are git submodules and both are required to build:

```
git submodule update --init --recursive
```

## Common commands

All commands run from the repo root via SCons.

```
scons                                   # build the extension for the host platform, default target
scons platform=<linux|windows|macos> target=<template_debug|template_release>
scons tests                             # build and run the native unit tests in tests/
scons format                            # clang-format -i over src/ and tests/ (.clang-format)
scons tidy                              # clang-tidy over src/*.cpp with the real build's flags (.clang-tidy)
scons docs                              # regenerate doc_classes/*.xml (via Godot --doctool) AND docs/*.md
scons update_wiki                       # regenerate only docs/*.md from the current doc_classes/*.xml
```

Notes:
- `scons tests` builds `tests/bin/tests` from `tests/*.cpp` using a plain native `Environment()`
  (no godot-cpp, no Godot process) — it's for engine-independent logic only, and always runs
  after building. Add new engine-independent tests as more `.cpp` files under `tests/`.
- The `docs` alias's `--doctool` step requires a flatpak install of the Godot editor
  (`org.godotengine.Godot`) and a `template_debug` build with doc data compiled in; it runs
  `flatpak run org.godotengine.Godot --doctool ../ --gdextension-docs` from `project/` (needs
  `project/project.godot` to load the extension into). `update_wiki` alone has no such
  dependency and just regenerates Markdown from whatever XML is already on disk.
- The built shared library lands in `project/addons/ResourceTables/bin/`; opening
  `project/project.godot` in the Godot editor exercises the addon.

## Architecture

- **`src/`** — the extension's C++ source. `register_types.cpp` is the entry point Godot calls
  (`resource_tables_library_init`, named in
  `project/addons/ResourceTables/ResourceTables.gdextension`); it registers each `GDCLASS` at
  `MODULE_INITIALIZATION_LEVEL_SCENE` in `initialize_resource_tables_module`. New classes get
  added there.
- **`doc_classes/*.xml`** is the source of truth for the addon's user-facing API docs, in Godot's
  class-doc XML schema (one file per class, matching `_bind_methods()`). It's generated by
  `scons docs` (via Godot's `--doctool`) but can also be hand-edited between doctool runs.
- **`tools/generate_docs.py`** converts `doc_classes/*.xml` into GitHub-wiki-ready Markdown
  under `docs/` (gitignored — a build artifact, not committed). It handles Godot's BBCode-ish
  markup (`[b]`, `[code]`, `[codeblock]`, `[ClassName]`, `[member ...]`, `[method ...]`,
  `[param ...]`) and cross-links classes/members that have their own `doc_classes/*.xml`.
  `.github/workflows/wiki.yml` runs it on every push to `main` that touches `doc_classes/**` or
  the script itself, then mirrors the output into the repo's GitHub wiki directly (not via a
  generic wiki-sync action, since `docs/` isn't committed to `main` for such an action to see).
- **`project/`** is a minimal Godot project used to load and manually test the built addon
  (`project/addons/ResourceTables/`), and as the load target for the `--doctool` docs build.
- **CI** (`.github/workflows/`): `build.yml` builds `template_debug` and runs `scons tests` on
  linux/windows/macos for every push/PR to `main`. `release.yml` (triggered by a `vX.X.X` tag or
  manual dispatch) builds `template_debug` + `template_release` on all three platforms, then
  zips `project/addons/ResourceTables/` (binaries for every platform, `LICENSE` included, no
  source) and attaches it to the GitHub Release — that zip's layout (`addons/ResourceTables/...`)
  is meant to be extracted straight into a consuming project's `res://`, and is also the intended
  "Custom" download URL for a Godot Asset Library submission.

## Naming consistency

The addon's name (`ResourceTables`) appears in several places that don't derive from each other
automatically, so a future rename (or adding a second addon) needs to keep them in sync:
`SConstruct`'s `ADDON_NAME`, the `project/addons/ResourceTables/` folder name, the
`*.gdextension` file (filename + its `entry_symbol`), `register_types.cpp`'s init/terminate
function names and `extern "C"` entry symbol (must match the `.gdextension` file's
`entry_symbol`), and the `resource-tables-bin-*` artifact name plus `ResourceTables.zip` in
`release.yml`.
