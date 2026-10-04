"""faces.json: estop with no text, every face pin at both ends of t3 and t1."""

import common as X

FACES = {0: "left", 1: "right", 2: "top", 3: "bottom"}
NOTES = ["No space requests (`--no-text`): every state is its minimum box, labels take no room.",
         "Each variant is `--no-text --no-search` + the shipped pins + one `--end T:L:E:F` (T transition, L leg/segment index, E end 0 = source / 1 = target, F face 0 left, 1 right, 2 top, 3 bottom).",
         "A face pin turns which border of its state that segment end leaves or enters by; ordering, sizing and routing are re-derived with it. `same_as_shipped` marks the face the shipped drawing already uses (coordinate hash equal).",
         "featured: t3 (Latched -> Clear) source end; both ends of t3 and t1 are included."]


def build(scav: X.Scav) -> dict:
    base, _ = X.geometry(scav, "estop", scale="notext")
    pins = [p for p in base["pin_list"] if p[0] != "--no-text"]
    flat = [a for p in pins for a in p]
    res = {"chart": "estop", "scale": "notext", "notes": NOTES,
           "shipped": base, "shipped_pins": base["pins"], "variants": []}
    for t in (3, 1):
        for e in (0, 1):
            for f in range(4):
                g, _ = X.geometry(scav, "estop", ["--no-search", *flat, "--end", f"{t}:0:{e}:{f}"],
                                  scale="notext")
                res["variants"].append({"trans": t, "leg": 0, "end": e, "end_name": "src" if e == 0 else "dst",
                                        "face": f, "face_name": FACES[f], "pin": f"--end {t}:0:{e}:{f}",
                                        "cost": g["cost"],
                                        "same_as_shipped": g["hash"]["coordinate"] == base["hash"]["coordinate"],
                                        "route": g["trans"][t]["points"], "geometry": g})
    res["featured"] = {"trans": 3, "end": 0}
    print(f"  estop shipped {base['pins']}, t2 {base['cost']['t2']}; {len(res['variants'])} face variants",
          flush=True)
    return res
