#include "register_types.h"

#ifdef TOOLS_ENABLED
#include "resource_table_container.h"
#include "resource_table_export_plugin.h"
#include "resource_table_name.h"
#include "resource_table_utils.h"
#include "resource_tables_plugin.h"

#include <godot_cpp/classes/editor_plugin_registration.hpp>
#endif

#include <gdextension_interface.h>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>

using namespace godot;

void initialize_resource_tables_module(ModuleInitializationLevel p_level) {
#ifdef TOOLS_ENABLED
	if (p_level == MODULE_INITIALIZATION_LEVEL_EDITOR) {
		GDREGISTER_INTERNAL_CLASS(ResourceTableName);
		GDREGISTER_INTERNAL_CLASS(ResourceTableExportPlugin);
		GDREGISTER_INTERNAL_CLASS(ResourceTableContainer);
		GDREGISTER_INTERNAL_CLASS(ResourceTablesPlugin);
		GDREGISTER_CLASS(ResourceTableUtils);
		EditorPlugins::add_by_type<ResourceTablesPlugin>();
	}
#endif
}

void uninitialize_resource_tables_module(ModuleInitializationLevel p_level) {
#ifdef TOOLS_ENABLED
	if (p_level == MODULE_INITIALIZATION_LEVEL_EDITOR) {
		EditorPlugins::remove_by_type<ResourceTablesPlugin>();
	}
#endif
}

extern "C" {
GDExtensionBool GDE_EXPORT resource_tables_library_init(GDExtensionInterfaceGetProcAddress p_get_proc_address, const GDExtensionClassLibraryPtr p_library, GDExtensionInitialization *r_initialization) {
	const godot::GDExtensionBinding::InitObject init_obj(p_get_proc_address, p_library, r_initialization);

	init_obj.register_initializer(initialize_resource_tables_module);
	init_obj.register_terminator(uninitialize_resource_tables_module);
	init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);

	return init_obj.init();
}
}
