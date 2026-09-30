#pragma once

#include <godot_cpp/classes/resource.hpp>

namespace godot {

class ResourceTable : public Resource {
	GDCLASS(ResourceTable, Resource) // NOLINT

protected:
	static void _bind_methods() {}
};

} // namespace godot
