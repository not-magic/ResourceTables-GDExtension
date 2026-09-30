#pragma once

#include <godot_cpp/classes/editor_export_plugin.hpp>
#include <godot_cpp/variant/callable.hpp>

namespace godot {

class ResourceTableExportPlugin : public EditorExportPlugin {
	GDCLASS(ResourceTableExportPlugin, EditorExportPlugin) // NOLINT

private:
	Callable generate_callback;

protected:
	static void _bind_methods() {}

public:
	void set_generate_callback(const Callable &p_callback);

	String _get_name() const override;
	void _export_begin(const PackedStringArray &p_features, bool p_is_debug, const String &p_path, uint32_t p_flags) override;
};

} // namespace godot
