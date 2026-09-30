@tool
## Must be @tool: Script.can_instantiate() is false for non-@tool scripts in the editor.
## extends RefCounted, not Resource: generators are live objects, never saved to a .tres.
@abstract
extends RefCounted
class_name ResourceTableGenerator

var _resource_class_name: String

func _init(resource_class_name: String) -> void:
	_resource_class_name = resource_class_name

@abstract func _generate_outputs() -> void
