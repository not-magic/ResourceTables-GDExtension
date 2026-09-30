@tool
class_name BBBGenerator
extends ResourceTableGenerator

func _init() -> void:
	super("BBB")

func _generate_outputs() -> void:
	var output := ResourceTableUtils.find_or_create_resource("GenericResourceTable", "res://ResourceTables/BBBs.tres")
	output.items = ResourceTableUtils.find_resources_of_type("BBB")
	ResourceSaver.save(output)
	
