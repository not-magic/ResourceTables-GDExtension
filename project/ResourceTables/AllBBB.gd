@tool
class_name AllBBB
extends ResourceTableGenerator

func _init() -> void:
	super("BBB")

func _get_items() -> Array:
	return ResourceTableUtils.find_resources_of_type("BBB")

func _add_instance() -> Resource:
	# this function may return null to disable the 'Add' button.
	var new_item_path_name := "res://ResourceTables/AllBBB_items/%s.tres" % [ResourceTableUtils.find_safe_name("BBB", "AllBBB")] 
	return ResourceTableUtils.create_instance("BBB", new_item_path_name)

func _generate_outputs() -> void:
	# comment this out for default basic save logic
	var output := ResourceTableUtils.find_or_create("GenericResourceTable", "res://ResourceTables/AllBBB.tres")
	output.items = _get_items()
	ResourceSaver.save(output)
	
