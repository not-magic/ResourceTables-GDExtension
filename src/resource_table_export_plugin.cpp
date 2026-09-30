#include "resource_table_export_plugin.h"

using namespace godot;

void ResourceTableExportPlugin::set_generate_callback(const Callable &p_callback) {
	generate_callback = p_callback;
}

String ResourceTableExportPlugin::_get_name() const {
	return "ResourceTables";
}

void ResourceTableExportPlugin::_export_begin(const PackedStringArray &p_features, bool p_is_debug, const String &p_path, uint32_t p_flags) {
	if (generate_callback.is_valid()) {
		generate_callback.call();
	}
}
