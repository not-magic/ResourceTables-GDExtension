#pragma once

#include <godot_cpp/classes/resource.hpp>

namespace godot {

// Base class for a table shown in the ResourceTables bottom panel. A
// subclass is itself the persisted table data: declare it with its own
// `class_name` and an `@export var ITEMS: Array[YourResourceType]` --
// ResourceTableUtils reflects YourResourceType off of ITEMS' typed-array
// hint, and (re)populates ITEMS by scanning res:// for resources of that
// type (see ResourceTableUtils::regenerate_table, invoked in bulk by
// addons/ResourceTables/export_resource_tables.gd). An optional
// `static _is_valid(item: YourResourceType) -> bool` filters which of those
// get included.
//
// Deliberately just a marker: find_table_class_names() identifies table
// subclasses by inheritance from this class, and it carries no state or
// logic of its own -- everything else lives on the subclass (ITEMS,
// _is_valid) or in ResourceTableUtils.
class ResourceTable : public Resource {
	GDCLASS(ResourceTable, Resource)

protected:
	static void _bind_methods() {}
};

} // namespace godot
