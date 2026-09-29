extends Node2D

@export var aaa_table : GenericResourceTable

func _ready() -> void:
	for item in aaa_table.items:
		print(item.flt_val)
