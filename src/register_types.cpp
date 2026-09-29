#include "register_types.h"

#include "resource_table.h"
#include "resource_table_name.h"
#include "resource_table_utils.h"
#include "resource_tables_plugin.h"
#include "resource_table_container.h"

#include <gdextension_interface.h>
#include <godot_cpp/classes/editor_plugin_registration.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>

using namespace godot;

// Note: godot-cpp only defines TOOLS_ENABLED when target=editor (building an
// engine module), not target=template_debug (what the stock Godot editor
// actually loads a GDExtension as). So editor-only registration below isn't
// guarded by that macro -- it's compiled in for every target, and instead
// relies on MODULE_INITIALIZATION_LEVEL_EDITOR only ever being reached when
// this extension is actually running inside an editor process.
void initialize_resource_tables_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		GDREGISTER_CLASS(ResourceTable);
		GDREGISTER_CLASS(ResourceTableUtils);
		// A general-purpose Control (not editor-only), unlike everything
		// else registered below -- usable in any project's own scenes.
		GDREGISTER_CLASS(ResourceTableContainer);
	}

	if (p_level == MODULE_INITIALIZATION_LEVEL_EDITOR) {
		// Internal: a plain reusable widget for ResourceTablesPlugin's own
		// Name cell -- not meant to show up in the "Create New Node" dialog
		// or be instantiated from a script.
		GDREGISTER_INTERNAL_CLASS(ResourceTableName);
		GDREGISTER_CLASS(ResourceTablesPlugin);
		EditorPlugins::add_by_type<ResourceTablesPlugin>();
	}
}

void uninitialize_resource_tables_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_EDITOR) {
		EditorPlugins::remove_by_type<ResourceTablesPlugin>();
	}
}

extern "C" {
GDExtensionBool GDE_EXPORT resource_tables_library_init(GDExtensionInterfaceGetProcAddress p_get_proc_address, const GDExtensionClassLibraryPtr p_library, GDExtensionInitialization *r_initialization) {
	godot::GDExtensionBinding::InitObject init_obj(p_get_proc_address, p_library, r_initialization);

	init_obj.register_initializer(initialize_resource_tables_module);
	init_obj.register_terminator(uninitialize_resource_tables_module);
	init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);

	return init_obj.init();
}
}
