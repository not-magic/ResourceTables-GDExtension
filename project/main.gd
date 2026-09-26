extends Node2D

@export var aaa_table : AAATableDef

func _ready() -> void:
	for item in aaa_table.ITEMS:
		print(item.flt_val)
