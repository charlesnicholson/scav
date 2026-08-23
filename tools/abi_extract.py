#!/usr/bin/env python3
"""Extract scav's C ABI to JSON: a scraper over the headers plus a probe the
toolchain under test compiles and runs.

Section 16 left the choice between this and libclang open. This is the one
picked, for two reasons that only got sharper: libclang means provisioning LLVM
on six bare runners that today carry a compiler and little else -- on darwin
that means *compiling clang*, which is what the lint gate already costs -- and a
probe reports the layout the shipping compiler really produces rather than a
second parser's model of one. The accepted cost is that it cannot answer for a
target it cannot execute, which `wasm32-wasi` will have to solve at P11 with a
runner or a declared fallback.

The headers are deliberately flat C with no macros so that either tool would
work. This one **fails closed**: a declaration form it does not recognise is an
error, never a silent skip, because silently dropping one would leave exactly
the drift the golden exists to catch.

  tools/abi_extract.py --out abi/scav_abi.json
  tools/abi_extract.py --check abi/scav_abi.json     exit 1 on a difference
"""

import argparse
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# Order matters: it is the order a reader meets the surface in, and the JSON is
# a golden, so it has to be stable rather than whatever a glob returns.
HEADERS = (
    "include/scav/scav_types.h",
    "src/core/include/scav/scav_core_c.h",
    "src/layout/include/scav/scav_layout_c.h",
    "src/draw/include/scav/scav_draw_c.h",
    "src/svg/include/scav/scav_svg_c.h",
)

# What a field or parameter type may be built from. A type this does not cover
# is a failure, not a passthrough: the binding generator has to know the width
# of everything it marshals.
PRIMITIVES = {
    "void", "char", "unsigned char",
    "int8_t", "int16_t", "int32_t", "int64_t",
    "uint8_t", "uint16_t", "uint32_t", "uint64_t",
}

COMMENT = re.compile(r"/\*.*?\*/", re.S)
LINE_COMMENT = re.compile(r"//[^\n]*")
NOLINT = re.compile(r"^\s*/\*\s*NOLINT", re.M)


class Unrecognized(Exception):
    """A declaration form the scraper does not model. Never swallowed."""


def strip_comments(text: str) -> str:
    return LINE_COMMENT.sub("", COMMENT.sub("", text))


def body_of(text: str) -> str:
    """The header as a C compiler sees it, which is what the ABI *is*.

    `__cplusplus` is undefined, so the `extern "C" {` braces and any C++-only
    section drop out together and what is left is the C surface. Every other
    conditional is taken; a form this cannot decide is an error, because
    guessing one would silently include or exclude part of the ABI.
    """
    kept: list[str] = []
    # One entry per open conditional: whether its current branch is live.
    stack: list[bool] = []
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith("#"):
            directive = stripped[1:].strip()
            if directive.startswith("ifdef ") or directive.startswith("ifndef "):
                keyword, _, symbol = directive.partition(" ")
                symbol = symbol.strip()
                if symbol == "__cplusplus":
                    stack.append(keyword == "ifndef")
                elif keyword == "ifndef":
                    stack.append(True)  # an include guard, always first through
                else:
                    raise Unrecognized(f"#ifdef {symbol}")
            elif directive.startswith("if"):
                raise Unrecognized(f"#{directive}")
            elif directive == "else":
                if not stack:
                    raise Unrecognized("#else with no #if")
                stack[-1] = not stack[-1]
            elif directive.startswith("endif"):
                if not stack:
                    raise Unrecognized("#endif with no #if")
                stack.pop()
            elif directive.startswith(("include", "define", "pragma")):
                pass
            else:
                raise Unrecognized(f"#{directive}")
            continue
        if all(stack):
            kept.append(line)
    return "\n".join(kept)


def split_declarations(text: str) -> list[str]:
    """Top-level declarations, split on semicolons outside braces."""
    out: list[str] = []
    depth = 0
    current = ""
    for ch in text:
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
        if (ch == ";") and (depth == 0):
            if current.strip():
                out.append(" ".join(current.split()))
            current = ""
            continue
        current += ch
    if current.strip():
        raise Unrecognized(f"trailing text with no semicolon: {current.strip()[:60]!r}")
    return out


def parse_type(text: str) -> dict:
    """A type as the generator needs it: a name, a pointer depth, and constness.

    Arrays are handled by the caller, since the extent belongs to the field
    rather than the type.
    """
    text = " ".join(text.split())
    pointer = text.count("*")
    # `const` before the type and `T const` both occur; the ABI never has a
    # const pointer itself, so one flag is enough.
    is_const = bool(re.search(r"\bconst\b", text))
    bare = re.sub(r"\bconst\b", " ", text).replace("*", " ")
    bare = " ".join(bare.split())
    if bare.startswith("struct "):
        bare = bare[len("struct "):]
    if not bare:
        raise Unrecognized(f"no type name in {text!r}")
    return {"name": bare, "pointer": pointer, "const": is_const}


# Split a declaration into its type and its declarator list. Anchoring on the
# declarator grammar rather than on whitespace is what makes both
# `uint32_t off, len` and `scav_byte const **out` come out right: the tail has
# to be a whole declarator list, so `const` cannot be mistaken for a name and
# `off` cannot be mistaken for part of the type.
DECLARATOR = r"\**\s*[A-Za-z_][A-Za-z0-9_]*(?:\[\d+\])?"
FIELD = re.compile(
    rf"^(?P<type>.*?)\s*(?P<names>{DECLARATOR}(?:\s*,\s*{DECLARATOR})*)$")


def parse_fields(body: str) -> list[dict]:
    """Struct fields, one entry per declarator: `uint32_t off, len;` is two."""
    fields: list[dict] = []
    for decl in body.split(";"):
        decl = " ".join(decl.split())
        if not decl:
            continue
        m = FIELD.match(decl)
        if not m:
            raise Unrecognized(f"field {decl!r}")
        base = m.group("type")
        for name in m.group("names").split(","):
            name = name.strip()
            array = None
            if (bracket := name.find("[")) != -1:
                if not name.endswith("]"):
                    raise Unrecognized(f"array field {name!r}")
                array = int(name[bracket + 1:-1])
                name = name[:bracket].strip()
            stars = name.count("*")
            name = name.replace("*", "").strip()
            if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name):
                raise Unrecognized(f"field name {name!r}")
            field = {"name": name, "type": parse_type(base + ("*" * stars))}
            if array is not None:
                field["array"] = array
            fields.append(field)
    return fields


def parse_params(text: str) -> list[dict]:
    if text.strip() in ("", "void"):
        return []
    params: list[dict] = []
    for raw in text.split(","):
        raw = " ".join(raw.split())
        m = FIELD.match(raw)
        if not m:
            # An unnamed parameter carries no name to marshal by; the ABI
            # headers always name them, so this is a real defect.
            raise Unrecognized(f"parameter {raw!r}")
        name = m.group("names").strip()
        stars = name.count("*")
        name = name.replace("*", "").strip()
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name):
            raise Unrecognized(f"parameter name {name!r}")
        params.append({"name": name,
                       "type": parse_type(m.group("type") + ("*" * stars))})
    return params


TYPEDEF_STRUCT = re.compile(r"^typedef struct \{(?P<body>.*)\}\s*(?P<name>\w+)$", re.S)
TYPEDEF_HANDLE = re.compile(r"^typedef struct (?P<tag>\w+) (?P<name>\w+)$")
TYPEDEF_ALIAS = re.compile(r"^typedef (?P<target>[\w\s]+?) (?P<name>\w+)$")
ANON_ENUM = re.compile(r"^enum \{(?P<body>.*)\}$", re.S)
# `\s*` rather than `\s` between the return type and the name: a pointer return
# binds to the name with no space, as in `char const *scav_diag_message`.
FUNCTION = re.compile(
    r"^(?P<ret>[\w\s*]+?)\s*(?P<name>scav_\w+)\((?P<params>.*)\)$", re.S)


def scrape(path: Path) -> dict:
    text = body_of(strip_comments(path.read_text(encoding="utf-8")))
    surface: dict = {"structs": [], "handles": [], "aliases": [], "enums": [],
                     "functions": []}

    for decl in split_declarations(text):
        if m := TYPEDEF_STRUCT.match(decl):
            surface["structs"].append({"name": m.group("name"),
                                       "fields": parse_fields(m.group("body"))})
        elif m := TYPEDEF_HANDLE.match(decl):
            if m.group("tag") != m.group("name"):
                raise Unrecognized(f"handle tag differs from its name: {decl!r}")
            surface["handles"].append({"name": m.group("name")})
        elif m := ANON_ENUM.match(decl):
            values = []
            for entry in m.group("body").split(","):
                entry = " ".join(entry.split())
                if not entry:
                    continue
                if "=" not in entry:
                    raise Unrecognized(f"enumerator without a value: {entry!r}")
                name, value = (part.strip() for part in entry.split("=", 1))
                # An integer-suffixed literal is still an integer; anything that
                # is not one at all is a form this does not model.
                literal = value.rstrip("uUlL")
                try:
                    values.append({"name": name, "value": int(literal, 0)})
                except ValueError as bad:
                    raise Unrecognized(f"enumerator value {value!r}") from bad
            surface["enums"].append({"values": values})
        elif m := FUNCTION.match(decl):
            surface["functions"].append({"name": m.group("name"),
                                         "returns": parse_type(m.group("ret")),
                                         "params": parse_params(m.group("params"))})
        elif m := TYPEDEF_ALIAS.match(decl):
            surface["aliases"].append({"name": m.group("name"),
                                       "target": parse_type(m.group("target"))})
        else:
            raise Unrecognized(f"declaration {decl[:80]!r} in {path.name}")
    return surface


PROBE_PREAMBLE = """// Generated by tools/abi_extract.py. Reports the layout this toolchain really
// produces, rather than a second parser's model of one.
#include <cstddef>
#include <cstdint>
#include <cstdio>
"""


def probe_source(headers: list[str], structs: list[str],
                 fields: dict[str, list[str]]) -> str:
    lines = [PROBE_PREAMBLE]
    for header in headers:
        lines.append(f'#include "{header}"')
    lines.append("")
    lines.append("int main(void) {")
    for name in structs:
        lines.append(f'  std::printf("struct {name} %zu %zu\\n", '
                     f'sizeof({name}), alignof({name}));')
        for field in fields[name]:
            lines.append(f'  std::printf("field {name} {field} %zu %zu\\n", '
                         f'offsetof({name}, {field}), '
                         f'sizeof(static_cast<{name}*>(nullptr)->{field}));')
    lines.append("  return 0;")
    lines.append("}")
    return "\n".join(lines) + "\n"


def run_probe(compiler: str, include_dirs: list[Path], headers: list[str],
              structs: list[str], fields: dict[str, list[str]]) -> dict:
    """Compile and run the probe, and read the layout back off its stdout."""
    with tempfile.TemporaryDirectory() as work:
        # C++, with the project's own compiler: that is the toolchain that ships,
        # and these are standard-layout PODs, so its answer is the C answer. That
        # the headers also compile *as C* is a separate claim, checked below.
        src = Path(work) / "abi_probe.cpp"
        exe = Path(work) / "abi_probe"
        src.write_text(probe_source(headers, structs, fields), encoding="utf-8")
        cmd = [compiler, "-std=c++20", "-o", str(exe), str(src)]
        for directory in include_dirs:
            cmd += ["-I", str(directory)]
        built = subprocess.run(cmd, capture_output=True, text=True, check=False)
        if built.returncode != 0:
            raise SystemExit(f"the ABI probe did not compile:\n{built.stderr}")
        ran = subprocess.run([str(exe)], capture_output=True, text=True, check=False)
        if ran.returncode != 0:
            raise SystemExit(f"the ABI probe did not run:\n{ran.stderr}")

    layout: dict = {}
    for line in ran.stdout.splitlines():
        parts = line.split()
        if parts[0] == "struct":
            layout.setdefault(parts[1], {})["size"] = int(parts[2])
            layout[parts[1]]["align"] = int(parts[3])
        elif parts[0] == "field":
            layout.setdefault(parts[1], {}).setdefault("fields", {})[parts[2]] = {
                "offset": int(parts[3]), "size": int(parts[4])
            }
        else:
            raise Unrecognized(f"probe output {line!r}")
    return layout


def check_compiles_as_c(cc: str, include_dirs: list[Path],
                       headers: list[str]) -> str | None:
    """Every C header, compiled as C11. A `C header is a second language, not a
    second place to look` only holds if it is actually C."""
    if shutil.which(cc) is None:
        return f"`{cc}` is not installed"
    with tempfile.TemporaryDirectory() as work:
        src = Path(work) / "c_check.c"
        src.write_text("".join(f'#include "{h}"\n' for h in headers)
                       + "int main(void) { return 0; }\n", encoding="utf-8")
        cmd = [cc, "-std=c11", "-Wall", "-Werror", "-c", "-o", "/dev/null", str(src)]
        for directory in include_dirs:
            cmd += ["-I", str(directory)]
        built = subprocess.run(cmd, capture_output=True, text=True, check=False)
    if built.returncode != 0:
        raise SystemExit(f"the C headers do not compile as C:\n{built.stderr}")
    return None


def extract(compiler: str) -> dict:
    """The whole surface: scraped shape, probed layout, merged."""
    include_dirs = [REPO_ROOT / "include"]
    surface: dict = {"version": 1, "headers": []}
    all_structs: list[str] = []
    all_fields: dict[str, list[str]] = {}

    for relative in HEADERS:
        path = REPO_ROOT / relative
        include_dirs.append(path.parent.parent)
        scraped = scrape(path)
        surface["headers"].append({"path": relative, **scraped})
        for struct in scraped["structs"]:
            all_structs.append(struct["name"])
            all_fields[struct["name"]] = [f["name"] for f in struct["fields"]]

    # Spelled the way a consumer spells them, since that is the include path the
    # install tree presents and the one the probe has to prove works.
    spelled = [f"scav/{Path(h).name}" for h in HEADERS]
    check_compiles_as_c("cc", include_dirs, spelled)
    layout = run_probe(compiler, include_dirs, spelled, all_structs, all_fields)

    # Padding is pinned, so the JSON carries it explicitly rather than leaving a
    # reader to derive it: an ABI break should be a diff, not an inference.
    for header in surface["headers"]:
        for struct in header["structs"]:
            probed = layout[struct["name"]]
            struct["size"] = probed["size"]
            struct["align"] = probed["align"]
            packed = 0
            for field in struct["fields"]:
                measured = probed["fields"][field["name"]]
                field["offset"] = measured["offset"]
                field["size"] = measured["size"]
                packed += measured["size"]
            struct["padding"] = probed["size"] - packed
    return surface


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", type=Path)
    ap.add_argument("--check", type=Path)
    ap.add_argument("--compiler", default="cc")
    args = ap.parse_args()

    try:
        surface = extract(args.compiler)
    except Unrecognized as e:
        # Fail closed: a form this does not model is a defect in the tool or a
        # new shape in the ABI, and both need a human.
        print(f"abi_extract: unrecognized declaration: {e}", file=sys.stderr)
        return 2

    text = json.dumps(surface, indent=2, sort_keys=False) + "\n"
    if args.check is not None:
        want = args.check.read_text(encoding="utf-8")
        if want != text:
            print(f"abi_extract: {args.check} is out of date", file=sys.stderr)
            return 1
        return 0
    if args.out is not None:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(text, encoding="utf-8")
        return 0
    sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
