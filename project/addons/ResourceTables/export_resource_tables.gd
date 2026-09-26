@tool
## Run from the Script Editor (File > Run, or Ctrl+Shift+X) to (re)generate
## res://generated_resource_tables/<TableClassName>.tres for every
## ResourceTable subclass in the project -- each one an instance of that
## class with its `ITEMS` populated from every resource of ITEMS' declared
## element type currently in the project.
##
## Discovery/filtering is done by ResourceTableUtils (C++), the single shared
## implementation of "what resources does this table show" also used by the
## ResourceTables editor panel itself.
extends EditorScript


func _run() -> void:
	for table_class_name in ResourceTableUtils.find_table_class_names():
		var resource_class_name := ResourceTableUtils.regenerate_table(table_class_name)
		if resource_class_name.is_empty():
			push_warning("%s has no `ITEMS: Array[Type]` export, or Type can't be resolved -- skipped." % table_class_name)
			continue
		print("Regenerated ", table_class_name, " (", resource_class_name, ")")

	EditorInterface.get_resource_filesystem().scan()
