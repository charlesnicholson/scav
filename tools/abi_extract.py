#!/usr/bin/env python3
"""Extracts scav's C ABI to JSON: scrapes the headers and runs a compiled probe for
struct layout. Exits 2 on a declaration form it does not recognise.

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

# Public C headers, in a fixed order that sets the JSON's order.
HEADERS = (
    "include/scav/scav_types.h",
    "src/core/include/scav/scav_core_c.h",
    "src/layout/include/scav/scav_layout_c.h",
    "src/draw/include/scav/scav_draw_c.h",
    "src/svg/include/scav/scav_svg_c.h",
)

# The primitive types the ABI's fields and parameters build from.
PRIMITIVES = {
    "void", "char", "unsigned char",
    "int8_t", "int16_t", "int32_t", "int64_t",
    "uint8_t", "uint16_t", "uint32_t", "uint64_t",
}

COMMENT = re.compile(r"/\*.*?\*/", re.S)
LINE_COMMENT = re.compile(r"//[^\n]*")
NOLINT = re.compile(r"^\s*/\*\s*NOLINT", re.M)


class Unrecognized(Exception):
    """A declaration form the scraper does not model."""


def strip_comments(text: str) -> str:
    return LINE_COMMENT.sub("", COMMENT.sub("", text))


DEFINE = re.compile(r"^define\s+(?P<name>[A-Za-z_]\w*)(?P<args>\()?(?P<rest>.*)$")


def body_of(text: str, constants: list[dict] | None = None) -> str:
    """The header's live lines with `__cplusplus` undefined and every `#ifndef` taken;
    integer `#define`s go to `constants`. Raises `Unrecognized` on other directives.
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
                    stack.append(True)  # an include guard
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
            elif directive.startswith("define"):
                m = DEFINE.match(directive)
                if m is None:
                    raise Unrecognized(f"#{directive}")
                if m.group("args") is not None:
                    raise Unrecognized(f"function-like macro {m.group('name')}")
                value = m.group("rest").strip()
                if not value:
                    continue  # an include guard names nothing
                if constants is None:
                    continue
                literal = value.rstrip("uUlL")
                try:
                    constants.append({"name": m.group("name"),
                                      "value": int(literal, 0)})
                except ValueError as bad:
                    raise Unrecognized(
                        f"#define {m.group('name')} {value}") from bad
            elif directive.startswith(("include", "pragma")):
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
    """A type as name, pointer depth and constness; the caller handles array extents."""
    text = " ".join(text.split())
    pointer = text.count("*")
    # One `const` flag covers `const T` and `T const`.
    is_const = bool(re.search(r"\bconst\b", text))
    bare = re.sub(r"\bconst\b", " ", text).replace("*", " ")
    bare = " ".join(bare.split())
    if bare.startswith("struct "):
        bare = bare[len("struct "):]
    if not bare:
        raise Unrecognized(f"no type name in {text!r}")
    return {"name": bare, "pointer": pointer, "const": is_const}


# Splits a declaration into its type and a whole declarator list, as in
# `uint32_t off, len` and `scav_byte const **out`.
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
# `\s*` before the name matches a pointer return bound to it (`char const *f`).
FUNCTION = re.compile(
    r"^(?P<ret>[\w\s*]+?)\s*(?P<name>scav_\w+)\((?P<params>.*)\)$", re.S)


def scrape(path: Path) -> dict:
    constants: list[dict] = []
    text = body_of(strip_comments(path.read_text(encoding="utf-8")), constants)
    surface: dict = {"structs": [], "handles": [], "aliases": [], "enums": [],
                     "constants": constants, "functions": []}

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
                # Integer literals may carry `uUlL` suffixes; other values are rejected.
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


def is_msvc(compiler: str) -> bool:
    """True for plain `cl` only; clang-cl uses the GNU flag branch."""
    return Path(compiler).stem.lower() == "cl"


def probe_command(compiler: str, src: Path, exe: Path, work: Path,
                  include_dirs: list[Path]) -> list[str]:
    if is_msvc(compiler):
        # /Fe and /Fo take attached paths; /Fo puts the object in `work`.
        cmd = [compiler, "/nologo", "/std:c++20", "/EHsc",
               f"/Fe{exe}", f"/Fo{work / 'abi_probe.obj'}", str(src)]
    else:
        cmd = [compiler, "-std=c++20", "-o", str(exe), str(src)]
    for directory in include_dirs:
        cmd += ["-I", str(directory)]
    return cmd


def run_probe(compiler: str, include_dirs: list[Path], headers: list[str],
              structs: list[str], fields: dict[str, list[str]]) -> dict:
    """Compile and run the probe, and read the layout back off its stdout."""
    with tempfile.TemporaryDirectory() as work:
        # A C++ probe built with the project's compiler; C11 is checked separately.
        src = Path(work) / "abi_probe.cpp"
        exe = Path(work) / ("abi_probe.exe" if is_msvc(compiler) else "abi_probe")
        src.write_text(probe_source(headers, structs, fields), encoding="utf-8")
        cmd = probe_command(compiler, src, exe, Path(work), include_dirs)
        built = subprocess.run(cmd, capture_output=True, text=True, check=False)
        if built.returncode != 0:
            # Includes stdout, where cl writes its diagnostics.
            raise SystemExit("the ABI probe did not compile:\n"
                             f"{' '.join(cmd)}\n{built.stdout}{built.stderr}")
        ran = subprocess.run([str(exe)], capture_output=True, text=True, check=False)
        if ran.returncode != 0:
            raise SystemExit(f"the ABI probe did not run:\n{ran.stdout}{ran.stderr}")

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
    """Compiles `headers` as C11, exiting on error; returns a skip reason or None."""
    if shutil.which(cc) is None:
        return f"`{cc}` is not installed"
    with tempfile.TemporaryDirectory() as work:
        src = Path(work) / "c_check.c"
        src.write_text("".join(f'#include "{h}"\n' for h in headers)
                       + "int main(void) { return 0; }\n", encoding="utf-8")
        cmd = [cc, "-std=c11", "-Wall", "-Werror", "-c",
               "-o", str(Path(work) / "c_check.o"), str(src)]
        for directory in include_dirs:
            cmd += ["-I", str(directory)]
        built = subprocess.run(cmd, capture_output=True, text=True, check=False)
    if built.returncode != 0:
        raise SystemExit("the C headers do not compile as C:\n"
                         f"{built.stdout}{built.stderr}")
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

    # Headers spelled `scav/<name>`, as a consumer includes them.
    spelled = [f"scav/{Path(h).name}" for h in HEADERS]
    if (skipped := check_compiles_as_c("cc", include_dirs, spelled)) is not None:
        print(f"abi_extract: headers not checked as C: {skipped}", file=sys.stderr)
    layout = run_probe(compiler, include_dirs, spelled, all_structs, all_fields)

    # Records each struct's measured size, alignment, field offsets and padding.
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
