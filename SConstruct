#!/usr/bin/env python
import os
import sys

# You can find documentation for SCons and SConstruct files at:
# https://scons.org/documentation.html

ADDON_NAME = 'ResourceTables'


# This lets SCons know that we're using godot-cpp, from the godot-cpp folder.
env = SConscript("godot-cpp/SConstruct")

# Configures the 'src' directory as a source for header files.
env.Append(CPPPATH=["src/"])

# --- csv-parser submodule (CSV import/export backend, used by
# src/resource_table_utils.cpp) ---
# csv-parser has no top-level CMake library target we can just link
# against (see csv-parser/include/internal/CMakeLists.txt) -- its .cpp
# files are the actual list of sources its own `csv` CMake target compiles,
# mirrored here. It also throws on things like a malformed row or a file it
# can't open, so both its own sources and resource_table_utils.cpp (the one
# file that calls into it) need exceptions enabled, overriding godot-cpp's
# project-wide -fno-exceptions/_HAS_EXCEPTIONS=0
# (see godot-cpp/tools/common_compiler_flags.py) -- every other source file
# keeps building exception-free.
CSV_PARSER_DIR = "csv-parser"
csv_env = env.Clone()
csv_env.Append(CPPPATH=[CSV_PARSER_DIR + "/include"])
csv_env.Append(CPPDEFINES=[
    # Single-threaded: simplest/most portable across our target platforms,
    # and plenty fast for the small property tables this addon deals with.
    ("CSV_ENABLE_THREADS", 0),
    # Safe default for csv-parser's own scalar-parsing fast path; assumes
    # the standard library's floating-point std::from_chars is NOT
    # available rather than probing for it (this addon has no CMake-style
    # configure step), which just means it always takes csv-parser's
    # portable fallback instead of a newer-stdlib-only shortcut.
    ("CLASSIFY_SCALAR_USE_STD_FLOAT_FROM_CHARS", 0),
])
if csv_env.get("is_msvc", False):
    csv_env["CPPDEFINES"] = [d for d in csv_env["CPPDEFINES"] if not (isinstance(d, tuple) and d[0] == "_HAS_EXCEPTIONS")]
    csv_env.Append(CXXFLAGS=["/EHsc"])
else:
    csv_env["CXXFLAGS"] = [f for f in csv_env["CXXFLAGS"] if f != "-fno-exceptions"]
    csv_env.Append(CXXFLAGS=["-fexceptions"])

csv_library_sources = [
    CSV_PARSER_DIR + "/include/internal/col_names.cpp",
    CSV_PARSER_DIR + "/include/internal/csv_format.cpp",
    CSV_PARSER_DIR + "/include/internal/parser/driver.cpp",
    CSV_PARSER_DIR + "/include/internal/parser/guessing.cpp",
    CSV_PARSER_DIR + "/include/internal/parser/mmap.cpp",
    CSV_PARSER_DIR + "/include/internal/csv_reader.cpp",
    CSV_PARSER_DIR + "/include/internal/csv_reader_iterator.cpp",
    CSV_PARSER_DIR + "/include/internal/csv_row.cpp",
    CSV_PARSER_DIR + "/include/internal/csv_utility.cpp",
]
csv_objects = [csv_env.SharedObject(source=src) for src in csv_library_sources]

# Collects all .cpp files in the 'src' folder as compile targets, except
# resource_table_utils.cpp, which is compiled under csv_env instead (see
# above) since it's the one file that includes csv-parser and needs
# exceptions enabled. SharedObject (not Object) so these are PIC-compatible
# with the SharedLibrary target below.
sources = [s for s in Glob("src/*.cpp") if s.name != "resource_table_utils.cpp"]
sources.append(csv_env.SharedObject(source="src/resource_table_utils.cpp"))
sources += csv_objects

if env["target"] in ["editor", "template_debug"]:
    try:
        doc_data = env.GodotCPPDocData("src/gen/doc_data.gen.cpp", source=Glob("doc_classes/*.xml"))
        sources.append(doc_data)
    except AttributeError:
        print("Not including class reference as we're targeting a pre-4.3 baseline.")

# The filename for the dynamic library for this GDExtension.
# $SHLIBPREFIX is a platform specific prefix for the dynamic library ('lib' on Unix, '' on Windows).
# $SHLIBSUFFIX is the platform specific suffix for the dynamic library (for example '.dll' on Windows).
# env["suffix"] includes the build's feature tags (e.g. '.windows.template_debug.x86_64')
# (see https://docs.godotengine.org/en/stable/tutorials/export/feature_tags.html).
# The final path should match a path in the '.gdextension' file.
lib_filename = "{}{}{}{}".format(env.subst('$SHLIBPREFIX'), ADDON_NAME, env["suffix"], env.subst('$SHLIBSUFFIX'))

# Creates a SCons target for the path with our sources.
library = env.SharedLibrary(
    "project/addons/{}/bin/{}".format(ADDON_NAME, lib_filename),
    source=sources,
)

# Selects the shared library as the default target.
Default(library)

# --- Unit tests (tests/) ---
# Builds and runs a native, engine-independent test program (no godot-cpp
# linking, no running Godot process required) for any pure logic added to
# src/. Uses its own native Environment rather than the (possibly
# cross-compiling) `env` above, since the test binary needs to run on this
# machine.
test_env = Environment()
test_env.Append(CPPPATH=["src/"])
if test_env["CXX"] == "cl":
    test_env.Append(CXXFLAGS=["/std:c++17"])
else:
    test_env.Append(CXXFLAGS=["-std=c++17"])

test_program = test_env.Program("tests/bin/tests", Glob("tests/*.cpp"))
run_tests = test_env.Alias("tests", test_program, test_program[0].abspath)
AlwaysBuild(run_tests)

# --- Formatting and linting (.clang-format, .clang-tidy) ---
# `scons format` rewrites src/ and tests/ in place with clang-format;
# `scons tidy` runs clang-tidy over every src/*.cpp (plus headers under src/) with
# the same include paths/defines the real build uses. Point CLANG_FORMAT /
# CLANG_TIDY at a specific binary to override the one found on PATH. The
# .clang-format/.clang-tidy files need a recent LLVM (distro clang 14 can't
# parse them); `pip install clang-format clang-tidy` provides one.
import subprocess

godot_env = env
lint_sources = sorted(str(f) for f in Glob("src/*.cpp") + Glob("src/*.h") + Glob("tests/*.cpp"))


def run_format(target, source, env):
    tool = os.environ.get("CLANG_FORMAT", "clang-format")
    return subprocess.call([tool, "-i", "--style=file"] + lint_sources)


def run_tidy(target, source, env):
    tool = os.environ.get("CLANG_TIDY", "clang-tidy")
    status = 0
    for path in lint_sources:
        if not path.endswith(".cpp") or path.startswith("tests"):
            continue
        lint_env = csv_env if path.endswith("resource_table_utils.cpp") else godot_env
        compile_args = ["-I" + str(d) for d in lint_env["CPPPATH"]] + ["-std=c++17"]
        for define in lint_env["CPPDEFINES"]:
            if isinstance(define, (tuple, list)):
                compile_args.append("-D{}={}".format(*define))
            else:
                compile_args.append("-D" + str(define))
        if lint_env is csv_env:
            compile_args.append("-fexceptions")
        status |= subprocess.call([tool, "--quiet", "--header-filter=.*/src/.*", path, "--"] + compile_args)
    return status


format_sources = Command("format", None, run_format)
AlwaysBuild(format_sources)

tidy_sources = Command("tidy", None, run_tidy)
AlwaysBuild(tidy_sources)

# --- Docs update (doc_classes/) ---
# Regenerates doc_classes/*.xml from the classes' _bind_methods() by loading
# the built extension into Godot's --doctool. Requires a template_debug build
# (with doc data compiled in, see the GodotCPPDocData block above) and a
# flatpak install of the Godot editor (org.godotengine.Godot). Run with
# `scons docs`. Runs from project/ since --doctool needs a Godot project
# (project.godot) to load the extension into.
update_docs = Command(
    "update_docs",
    None,
    "flatpak run org.godotengine.Godot --doctool ../ --gdextension-docs",
    chdir="project",
)
AlwaysBuild(update_docs)

# --- Wiki docs (docs/) ---
# Regenerates docs/*.md (GitHub wiki pages) from doc_classes/*.xml via
# tools/generate_docs.py. Not part of the default build -- run explicitly
# with `scons update_wiki` (against whatever doc_classes/*.xml is currently
# on disk), or via `scons docs`, which also regenerates that XML first so
# the wiki pages never drift from it. Split into two Command nodes so a
# plain build/`scons update_wiki` never pulls in the flatpak --doctool step
# above.
wiki_action = "{} tools/generate_docs.py --src doc_classes --out docs".format(sys.executable)

update_wiki = Command("update_wiki", None, wiki_action)
AlwaysBuild(update_wiki)

update_wiki_after_docs = Command("update_wiki_after_docs", None, wiki_action)
AlwaysBuild(update_wiki_after_docs)
Requires(update_wiki_after_docs, update_docs)

docs_alias = Alias("docs", [update_docs, update_wiki_after_docs])
