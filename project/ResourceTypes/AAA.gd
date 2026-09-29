extends Resource
class_name AAA

enum Test {
	A,
	B,
	C
}
	

@export var int_val := 0
@export var str_val := "string"
@export_range(0, 20) var flt_val := 0.0
@export var enabled := true

@export_range(0, 20) var int_val2 := 0
@export var int_val3 := 0
@export var int_val4 := 0
@export var int_val5 := 0
@export var int_val6 := 0
@export var color_val := Color.RED
@export var enum_val := Test.A
