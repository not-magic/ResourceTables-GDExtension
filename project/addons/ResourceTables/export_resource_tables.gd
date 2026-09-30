@tool
class_name ExportResourceTables
extends EditorScript

static func export_tables(modified_resource:String = '') -> void:
	
	var did_something := false
	for script in ResourceTableUtils.find_generator_scripts():
		if not script.can_instantiate():
			push_warning("%s is not @tool -- can't be instantiated in the editor, skipped." % script.resource_path)
			continue
			
		var generator = script.new() as ResourceTableGenerator
		
		if not modified_resource.is_empty():			
			if generator._resource_class_name != modified_resource:
				# make sure our class is compatible
				var inheritors := ClassDB.get_inheriters_from_class(generator._resource_class_name)
				if  inheritors.find(modified_resource) < 0:
					continue
			
		generator._generate_outputs()
		did_something = true

	EditorInterface.get_resource_filesystem().scan()

	# _generate_outputs() edits an existing resource in place (via
	# ResourceTableUtils.find_or_create) -- if the Inspector currently has
	# that same resource open, it doesn't notice the change on its own, so
	# nudge it to redraw.
	if did_something:
		var edited_object := EditorInterface.get_inspector().get_edited_object()
		if edited_object:
			edited_object.notify_property_list_changed()
