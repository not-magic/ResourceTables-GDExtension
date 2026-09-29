@tool
## Base for a generator run by the "Run Generators" Tools-menu action
## (ResourceTablesPlugin) and export_resource_tables.gd. Must be @tool: both
## instantiate a subclass via its own Script.new(), and
## Script.can_instantiate() is false for a non-@tool script while running in
## the editor -- so a subclass generated from
## new_resource_table_generator_template.txt is @tool too.
##
## extends RefCounted, not Resource: a generator is a live object, never
## saved to its own .tres file -- ResourceTableUtils.find_generator_scripts()
## discovers the *script*, not an instance of it.
@abstract
extends RefCounted
class_name ResourceTableGenerator

var _resource_class_name: String

func _init(resource_class_name: String) -> void:
	_resource_class_name = resource_class_name

## Writes this generator's output tables -- the editor doesn't care what
## format is used.
@abstract func _generate_outputs() -> void
