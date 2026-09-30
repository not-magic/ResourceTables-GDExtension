@tool
# An example table resource class
class_name GenericResourceTable
extends Resource

# the table must be a @tool script to be able to define this variable. If it exists
# and is false, then it will be skipped in the table view

# it works this way in order to remove any dependency on ResourceTables plugin
# for your table types
static var _show_in_resource_table_view := false

@export var items: Array
