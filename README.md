# StarterTemplate-GDExtension

A starter template to make a native GDExtension. Based on (https://docs.godotengine.org/en/stable/tutorials/scripting/cpp/gdextension_cpp_example.html)

* Has GDExtension xml documentation support (rebuild with `scons docs`)
* Updates wiki docs from a github action, from the godot doc_classes xml files
* GitHub build action will compile and runs tests
* GitHub release action will compile, .zip the addon and append it to any release with a `vX.X.X` tag format