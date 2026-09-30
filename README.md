# ResourceTables

If you have used Unreal before, you might be wondering: What Godot's equivalent to DataTables?

I have spent years supporting Unreal `DataTable` workflows on AAA games and I believe there is a better design hidden in Godot data formats already: **Treating Resources as rows in a table**.

## Resources Table Editor

The main reason `DataTable` is used in Unreal is because it's more convenient to edit groups of related things together. This means *it's not actually a data format problem, it's a tools problem*. This extension adds a **Resources** view in the bottom dock panel where you can bulk edit all the resources you have in your game. This is a high-quality view, limited only by what editor functionality is exposed to a GDExtension.

![Screenshot of the table view in action](images/WeaponTuning.png)

### Usage

Open the **Resources** panel at the bottom of the editor and choose a type from the dropdown. Edit
cells in place; save with Ctrl+S or the Inspector as usual.

Your resource types must have a `class_name` assigned for them to appear!

- **Export** / **Import CSV** From the tools menu, to allow bulk editing all of your resources in an external spreadsheet

## ResourceTable for Runtime

The second reason people use `DataTable` is to be able to iterate over all rows at runtime. You might have a list of all `WeaponDrop` resources in the game and you can loop over all of them to determine what loot to spawn. This `WeaponDrop` resource should probably be placed next to the weapon scene or definition to keep it self-contained.  **ResourceTables** adds a lightweight `@tool` generator interface that is hooked up to run when saving new resources or exporting the game. It has no restrictions on your output table formats or location, or even the number of outputs it has. 

This generator can also assist making table editing easier by giving sensible defaults to resource names and locations, when appropriate.

### Generators

A generator is a script extending `ResourceTableGenerator` that implements `_generate_outputs()` and
writes whatever it wants to disk. Use **Tools > New ResourceTable Generator...** to create one from
the template:

```gdscript
@tool
class_name MyItemGenerator
extends ResourceTableGenerator

func _init() -> void:
	super("MyItem")

func _generate_outputs() -> void:
	var output := ResourceTableUtils.find_or_create_resource("GenericResourceTable", "res://MyItems.tres")
	output.items = ResourceTableUtils.find_resources_of_type("MyItem")
	ResourceSaver.save(output)
```

Generators run automatically when resources of their type change, before a build, and on game export. You can write any
resource table type you want, including your own types if you want them to be strictly typed or split into multiple tables.

Generated tables should derive from `ResourceTable` to be excluded from the table view drop-down.

## Why `ResourceTables` are better than `DataTables`

It's not very often that `DataTables` are referenced directly, usually you reference an individual row with the `DataTableRowHandle`. This means in Unreal you have to load the entire table to find one row, which may require loading unnecessary extra resources. Godot referencing `Resources` directly is essentially the same thing, but with no extra overhead.

`DataTables` are difficult to manage across a team, because any edits to one row lock the entire table. But in Godot `Resources` are individual files.

`DataTables` are limited to rows of the exact same type. Because `ResourceTables` still have the resource class hierarchy, you can edit base types together, and store them mixed in a generated table if you want. 

Unreal still needed a way to dynamically combine multiple `DataTables` into one thing, which is why they introduced `DataRegistries`. This lets them define a `DataTable` in a feature plugin, then stitch them together in a virtual table which is the `DataRegistry`. These then need an entirely new API for referencing data registry row handles which is complicated and is often not setup in a project correctly. *This basically means `DataTables` in Unreal are a bit of tech debt, and shouldn't be used directly for things you want to be a global table anyway*. `ResourceTables` solve this problem before it begins.

## Features

- **Table editor** — pick a resource type from the dropdown and see every instance under `res://`
  (including subclasses) as a row, with one column per exported property.
- **CSV** — export a type's table to CSV and import it back.
- **Generators** — scripts that automatically write an aggregated table whenever the source resources
  change, and before builds and exports.

## Installation

1. Download the latest `ResourceTables.zip` from the [Releases](../../releases) page.
2. Extract it into your project so that `addons/resource_tables/` sits in `res://`.
3. Enable **ResourceTables** in *Project > Project Settings > Plugins* (if listed), then reload the project.

Installation should also appear on the Godot asset store eventually.


## API Documentation

API docs are available in the wiki, or the built-in godot documentation browser

## License

[MIT](LICENSE)
