@tool
class_name AAATable
extends ResourceTable

@export var ITEMS : Array[AAA]

static func _is_valid(_item: AAA) -> bool:
	return true
