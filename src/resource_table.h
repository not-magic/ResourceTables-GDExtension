#pragma once

#include <godot_cpp/classes/resource.hpp>

namespace godot {

// Marker base for a ResourceTableGenerator's own output (e.g.
// GenericResourceTable.gd, project/addons/ResourceTables/). Deliberately
// just a marker, carrying no members or logic of its own -- its only job is
// to let ResourceTableUtils recognize and exclude its own instances from the
// "Resources" panel's main type dropdown (see
// ResourceTableUtils::find_resource_type_names_with_instances), the same way
// a generator script itself is excluded.
class ResourceTable : public Resource {
	GDCLASS(ResourceTable, Resource)

protected:
	static void _bind_methods() {}
};

} // namespace godot
