#!/usr/bin/env python3
"""Builds scav-explained.html: the shell, every section fragment in filename order, and
every data/*.json embedded as <script type="application/json" id="sxdata-NAME">."""

import argparse
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default="", help="comma-separated section filename prefixes, e.g. 05,06")
    ap.add_argument("--out", default=str(HERE / "scav-explained.html"))
    a = ap.parse_args()
    keep = [k for k in a.only.split(",") if k]
    OUT = Path(a.out)
    shell = (HERE / "shell.html").read_text(encoding="utf-8")
    bodies, scripts = [], []
    for frag in sorted((HERE / "sections").glob("*.html")):
        if keep and not any(frag.name.startswith(k) for k in keep):
            continue
        text = frag.read_text(encoding="utf-8")
        found = re.findall(r"<script>(.*?)</script>", text, flags=re.S)
        scripts.extend(f"<script>/* {frag.name} */{s}</script>" for s in found)
        bodies.append(re.sub(r"<script>.*?</script>", "", text, flags=re.S).strip())
    data = []
    for js in sorted((HERE / "data").glob("*.json")):
        payload = js.read_text(encoding="utf-8").replace("</", "<\\/")
        data.append(f'<script type="application/json" id="sxdata-{js.stem}">{payload}</script>')
    page = (shell.replace("<!--SECTIONS-->", "\n".join(bodies))
                 .replace("<!--DATA-->", "\n".join(data))
                 .replace("<!--SCRIPTS-->", "\n".join(scripts)))
    OUT.write_text(page, encoding="utf-8")
    print(f"wrote {OUT} ({OUT.stat().st_size / 1e6:.2f} MB): "
          f"{len(bodies)} sections, {len(data)} data files")
    return 0


if __name__ == "__main__":
    sys.exit(main())
