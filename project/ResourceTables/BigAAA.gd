@tool
class_name BigAAA
extends ResourceTableGenerator

func _init() -> void:
	super("AAA")

func _get_items() -> Array:
	var result := []
	for item:AAA in ResourceTableUtils.find_resources_of_type("AAA"):
		if item.int_val != -1:
			result.append(item)
			
	return result

func _add_instance() -> Resource:
	# this function may return null to disable the 'Add' button.
	var new_item_path_name := "res://ResourceTables/BigAAA_items/%s.tres" % [ResourceTableUtils.find_safe_name("AAA", "BigAAA")] 
	return ResourceTableUtils.create_instance("AAA", new_item_path_name)

func _generate_output() -> void:
	# comment this out for default basic save logic
	var output := ResourceTableUtils.find_or_create("GenericResourceTable", "res://ResourceTables/BigAAA.tres")
	output.items = _get_items()
	ResourceSaver.save(output)
