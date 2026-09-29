@tool
## Run from the Script Editor (File > Run, or Ctrl+Shift+X) to call
## _generate_outputs() on one instance of every ResourceTableGenerator
## subclass in the project -- each instance decides itself what to write and
## where (see resource_table_generator.gd's own doc comment).
##
## Discovery is done by ResourceTableUtils (C++), the single shared
## implementation of "what ResourceTableGenerator subclasses exist" also used
## by the ResourceTables editor panel itself.
extends EditorScript

func _run() -> void:

	for script in ResourceTableUtils.find_generator_scripts():
		if not script.can_instantiate():
			push_warning("%s is not @tool -- can't be instantiated in the editor, skipped." % script.resource_path)
			continue
		var generator = script.new()
		generator._generate_outputs()

	EditorInterface.get_resource_filesystem().scan()

	# _generate_outputs() edits an existing resource in place (via
	# ResourceTableUtils.find_or_create) -- if the Inspector currently has
	# that same resource open, it doesn't notice the change on its own, so
	# nudge it to redraw.
	var edited_object := EditorInterface.get_inspector().get_edited_object()
	if edited_object:
		edited_object.notify_property_list_changed()
