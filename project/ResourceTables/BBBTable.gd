@tool
class_name BBBTable
extends ResourceTable

@export var ITEMS : Array[BBB]

static func _is_valid(_item: BBB) -> bool:
	return true
