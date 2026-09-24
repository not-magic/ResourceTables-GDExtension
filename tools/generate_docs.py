#!/usr/bin/env python3
"""Generate Markdown documentation from Godot-style class XML files.

Reads every *.xml file in doc_classes/ and writes a corresponding
*.md file to docs/, one per class.

Usage:
    python3 tools/generate_docs.py [--src doc_classes] [--out docs]
"""

import argparse
import re
import textwrap
import xml.etree.ElementTree as ET
from pathlib import Path

# Matches a bare Godot doc class reference like `[DampedSpringParameters]` --
# a single identifier with no space or dot inside the brackets. Qualified
# refs such as `[member velocity]`, `[method update]`, or
# `[member Node3D.position]` contain a space and so don't match this one;
# those are handled by MEMBER_METHOD_REF_RE below.
CLASS_LINK_RE = re.compile(r"\[([A-Za-z_][A-Za-z0-9_]*)\]")

# Matches `[member name]`/`[method name]` (referring to this class's own
# member/method) and `[member Class.name]`/`[method Class.name]` (referring
# to another class's).
MEMBER_METHOD_REF_RE = re.compile(
    r"\[(member|method)\s+([A-Za-z_][A-Za-z0-9_]*)(?:\.([A-Za-z_][A-Za-z0-9_]*))?\]"
)

CODEBLOCK_RE = re.compile(r"\[codeblock\](.*?)\[/codeblock\]", re.DOTALL)

# Matches a parameter reference like `[param delta]`.
PARAM_RE = re.compile(r"\[param\s+([A-Za-z_][A-Za-z0-9_]*)\]")


def text(el):
    """Return the stripped text content of an element, or '' if empty/None."""
    if el is None or el.text is None:
        return ""
    return el.text.strip()


def anchor_id(kind, name):
    """The anchor id given to a property/method's `<a name=...>`, and used to
    link to it from `[member ...]`/`[method ...]` refs elsewhere."""
    return f"{kind}-{name.lower()}"


def linkify_class_refs(text, known_classes):
    """Turn `[ClassName]` into a relative Markdown link when ClassName is one
    of this addon's own documented classes (i.e. has a doc_classes/*.xml).
    Otherwise (e.g. a builtin type like `[Vector3]`), drop the brackets and
    bold the name instead, since there's nowhere local to link it to."""

    def repl(m):
        name = m.group(1)
        if name in known_classes:
            return f"[{name}](./{name})"
        return f"**{name}**"

    return CLASS_LINK_RE.sub(repl, text)


def linkify_member_method_refs(text, known_classes, current_class):
    """Turn `[member name]`/`[method name]` (this class) and
    `[member Class.name]`/`[method Class.name]` (another class) into a link
    to that member/method's anchor, when the target class is documented
    locally. Otherwise, drop the brackets/keyword and bold the reference,
    same as an unrecognized `[ClassName]`."""

    def repl(m):
        kind, first, second = m.group(1), m.group(2), m.group(3)
        cls, name = (first, second) if second else (current_class, first)
        if cls in known_classes:
            anchor = anchor_id(kind, name)
            target = f"#{anchor}" if cls == current_class else f"./{cls}#{anchor}"
            return f"[{name}]({target})"
        return f"**{first}.{second}**" if second else f"**{first}**"

    return MEMBER_METHOD_REF_RE.sub(repl, text)


def fence_codeblocks(raw):
    """Turn `[codeblock]...[/codeblock]` into a fenced ```gdscript block,
    dedenting its contents by the common leading whitespace (the XML
    source indents every line to match the surrounding tag depth) while
    preserving the code's own relative indentation (e.g. a function body)."""

    def repl(m):
        code = textwrap.dedent(m.group(1)).strip("\n")
        return f"\n```gdscript\n{code}\n```\n"

    return CODEBLOCK_RE.sub(repl, raw)


def italicize_param_refs(text):
    """Turn `[param name]` into an italicized `_name_`."""
    return PARAM_RE.sub(lambda m: f"_{m.group(1)}_", text)


def strip_prose_indentation(text):
    """Remove the XML source's per-line indentation from ordinary prose --
    continuation lines are indented to match the surrounding XML tag depth,
    which Markdown would otherwise render as an unintended code block --
    without touching the (already-dedented) contents of a fenced code
    block."""
    lines = text.split("\n")
    out = []
    in_code_block = False
    for line in lines:
        if line.strip().startswith("```"):
            in_code_block = not in_code_block
            out.append(line.strip())
        elif in_code_block:
            out.append(line)
        else:
            out.append(line.lstrip())
    return "\n".join(out)


def format_description(raw, known_classes=frozenset(), current_class=None):
    """Convert BBCode-ish Godot doc markup into plain Markdown text."""
    if not raw:
        return ""
    out = fence_codeblocks(raw)
    replacements = [
        ("[b]", "**"), ("[/b]", "**"),
        ("[i]", "_"), ("[/i]", "_"),
        ("[code]", "`"), ("[/code]", "`"),
    ]
    for old, new in replacements:
        out = out.replace(old, new)
    # Class refs first: its regex only matches a single bare identifier in
    # brackets, so it can't accidentally re-match the `[name](#anchor)` links
    # linkify_member_method_refs produces, but running it after would.
    out = linkify_class_refs(out, known_classes)
    out = linkify_member_method_refs(out, known_classes, current_class)
    out = italicize_param_refs(out)
    out = strip_prose_indentation(out)
    return out.strip()


def format_type(type_name):
    return f"`{type_name}`" if type_name else ""


def render_params(params):
    parts = []
    for p in params:
        name = p.get("name", "")
        ptype = p.get("type", "")
        default = p.get("default")
        piece = f"{format_type(ptype)} {name}"
        if default is not None:
            piece += f" = {default}"
        parts.append(piece)
    return ", ".join(parts)


def render_params_plain(params):
    """Like render_params, but with no per-piece Markdown formatting -- for
    building a method's plain-text signature before it's wrapped in a single
    code span."""
    parts = []
    for p in params:
        name = p.get("name", "")
        ptype = p.get("type", "")
        default = p.get("default")
        piece = f"{ptype} {name}" if ptype else name
        if default is not None:
            piece += f" = {default}"
        parts.append(piece)
    return ", ".join(parts)


def render_class(root, class_name, known_classes=frozenset()):
    lines = []
    inherits = root.get("inherits")

    lines.append(f"# {class_name}")
    lines.append("")
    if inherits:
        lines.append(f"**Inherits:** `{inherits}`")
        lines.append("")

    brief = format_description(text(root.find("brief_description")), known_classes, class_name)
    if brief:
        lines.append(brief)
        lines.append("")

    description = format_description(text(root.find("description")), known_classes, class_name)
    if description:
        lines.append("## Description")
        lines.append("")
        lines.append(description)
        lines.append("")

    tutorials = root.find("tutorials")
    if tutorials is not None:
        # <tutorials>'s own text (before any <link> children) isn't part of
        # the Godot doc schema, but this addon's XML sometimes puts a
        # [codeblock] example there -- run it through format_description too
        # so [codeblock] (and any other BBCode-ish markup) is handled no
        # matter which element it ends up in.
        intro = format_description(text(tutorials), known_classes, class_name)
        links = tutorials.findall("link")
        if intro or links:
            lines.append("## Tutorials")
            lines.append("")
            if intro:
                lines.append(intro)
                lines.append("")
            for link in links:
                title = link.get("title")
                url = text(link)
                label = title if title else url
                if url:
                    lines.append(f"- [{label}]({url})")
            if links:
                lines.append("")

    members = root.find("members")
    if members is not None:
        member_list = members.findall("member")
        if member_list:
            lines.append("## Properties")
            lines.append("")
            lines.append("| Type | Name | Default | Description |")
            lines.append("|------|------|---------|-------------|")
            for m in member_list:
                mtype = format_type(m.get("type", ""))
                name = m.get("name", "")
                default = m.get("default", "")
                desc = format_description(text(m), known_classes, class_name).replace("\n", " ")
                name_cell = f'<a name="{anchor_id("member", name)}"></a>{name}'
                lines.append(f"| {mtype} | {name_cell} | `{default}` | {desc} |")
            lines.append("")

    methods = root.find("methods")
    if methods is not None:
        method_list = methods.findall("method")
        if method_list:
            lines.append("## Methods")
            lines.append("")
            for meth in method_list:
                name = meth.get("name", "")
                ret_el = meth.find("return")
                ret_type = ret_el.get("type") if ret_el is not None else "void"
                params = meth.findall("param")
                qualifiers = meth.get("qualifiers", "")
                sig = f"{ret_type} {name}({render_params_plain(params)})"
                if qualifiers:
                    sig += f" {qualifiers}"
                lines.append(f'<a name="{anchor_id("method", name)}"></a>')
                lines.append(f"### `{sig}`")
                lines.append("")
                desc = format_description(text(meth.find("description")), known_classes, class_name)
                if desc:
                    lines.append(desc)
                    lines.append("")

    signals = root.find("signals")
    if signals is not None:
        signal_list = signals.findall("signal")
        if signal_list:
            lines.append("## Signals")
            lines.append("")
            for sig in signal_list:
                name = sig.get("name", "")
                params = sig.findall("param")
                lines.append(f"### {name}({render_params(params)})")
                lines.append("")
                desc = format_description(text(sig.find("description")), known_classes, class_name)
                if desc:
                    lines.append(desc)
                    lines.append("")

    constants = root.find("constants")
    if constants is not None:
        const_list = constants.findall("constant")
        if const_list:
            lines.append("## Constants")
            lines.append("")
            lines.append("| Name | Value | Description |")
            lines.append("|------|-------|-------------|")
            for c in const_list:
                name = c.get("name", "")
                value = c.get("value", "")
                desc = format_description(text(c), known_classes, class_name).replace("\n", " ")
                lines.append(f"| {name} | `{value}` | {desc} |")
            lines.append("")

    theme_items = root.find("theme_items")
    if theme_items is not None:
        item_list = theme_items.findall("theme_item")
        if item_list:
            lines.append("## Theme Properties")
            lines.append("")
            lines.append("| Type | Name | Default | Description |")
            lines.append("|------|------|---------|-------------|")
            for item in item_list:
                itype = format_type(item.get("type", ""))
                name = item.get("name", "")
                default = item.get("default", "")
                desc = format_description(text(item), known_classes, class_name).replace("\n", " ")
                lines.append(f"| {itype} | {name} | `{default}` | {desc} |")
            lines.append("")

    return "\n".join(lines).rstrip() + "\n"


def render_sidebar(class_names):
    lines = ["# Classes", ""]
    for name in sorted(class_names, key=str.lower):
        lines.append(f"- [{name}]({name})")
    return "\n".join(lines).rstrip() + "\n"


def find_addon_name(addons_dir):
    """Return the addon's name, taken from its folder under project/addons/."""
    addons_dir = Path(addons_dir)
    if not addons_dir.is_dir():
        return None
    candidates = sorted(p.name for p in addons_dir.iterdir() if p.is_dir())
    if len(candidates) == 1:
        return candidates[0]
    return None


def render_footer(addon_name):
    return f"Documentation for the **{addon_name}** GDExtension addon.\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--src", default="doc_classes", help="Directory containing class XML files")
    parser.add_argument("--out", default="docs", help="Output directory for Markdown files")
    parser.add_argument("--addons-dir", default="project/addons", help="Directory to auto-detect the addon name from, for _Footer.md")
    parser.add_argument("--addon-name", default=None, help="Addon name shown in _Footer.md (overrides auto-detection from --addons-dir)")
    args = parser.parse_args()

    src_dir = Path(args.src)
    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)

    xml_files = sorted(src_dir.glob("*.xml"))
    if not xml_files:
        print(f"No XML files found in {src_dir}")
        return

    parsed = []
    class_names = []
    for xml_path in xml_files:
        tree = ET.parse(xml_path)
        root = tree.getroot()
        class_name = root.get("name", xml_path.stem)
        parsed.append((xml_path, root, class_name))
        class_names.append(class_name)

    # Known up front so a class's description can link to another class
    # documented later in this same run (order of doc_classes/*.xml doesn't
    # matter).
    known_classes = set(class_names)

    for xml_path, root, class_name in parsed:
        markdown = render_class(root, class_name, known_classes)
        out_path = out_dir / f"{xml_path.stem}.md"
        out_path.write_text(markdown, encoding="utf-8")
        print(f"Wrote {out_path}")

    sidebar_path = out_dir / "_Sidebar.md"
    sidebar_path.write_text(render_sidebar(class_names), encoding="utf-8")
    print(f"Wrote {sidebar_path}")

    addon_name = args.addon_name or find_addon_name(args.addons_dir)
    if addon_name:
        footer_path = out_dir / "_Footer.md"
        footer_path.write_text(render_footer(addon_name), encoding="utf-8")
        print(f"Wrote {footer_path}")
    else:
        print(f"Could not determine addon name from {args.addons_dir}; skipping _Footer.md")


if __name__ == "__main__":
    main()
