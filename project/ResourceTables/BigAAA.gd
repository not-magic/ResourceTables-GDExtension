@tool
class_name BigAAAGenerator
extends ResourceTableGenerator

func _init() -> void:
	super("AAA")

func _get_items() -> Array:
	# this example is creating a collection that is only enabled and has 
	# a value > 5
	
	var result := []
	for item:AAA in ResourceTableUtils.find_resources_of_type("AAA"):
		if item.int_val > 5 and item.enabled:
			result.append(item)
			
	return result

func _generate_outputs() -> void:
	# comment this out for default basic save logic
	var output := ResourceTableUtils.find_or_create_resource("GenericResourceTable", "res://ResourceTables/BigAAA.tres")
	output.items = _get_items()
	ResourceSaver.save(output)
	print("saved %s" % [output.resource_path])
