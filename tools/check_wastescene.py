"""Structural check of a .wastescene file.

Does not parse YAML (no pyyaml here) but validates the invariants the engine's
yaml-cpp reader depends on: the Scene/Entities header, the '- Entity:' list
shape, unique entity ids, and that every collider's HalfExtents matches its
visual Scale/2 (so what you see is what you hit).

Run:  python tools/check_wastescene.py <path>
"""
import re
import sys


def main(path):
    with open(path, encoding="utf-8") as f:
        lines = f.read().splitlines()

    errors = []

    if not lines or not lines[0].startswith("Scene: "):
        errors.append("missing 'Scene: <name>' header on line 1")
    if len(lines) < 2 or lines[1] != "Entities:":
        errors.append("missing 'Entities:' on line 2")

    # Every entity begins with "  - Entity: <id>" at two-space indent.
    entity_starts = [i for i, ln in enumerate(lines) if re.match(r"^  - Entity: \d+$", ln)]
    if not entity_starts:
        errors.append("no entities found (expected '  - Entity: <id>')")

    ids = []
    for i in entity_starts:
        ids.append(int(lines[i].split(":")[1].strip()))
    if len(set(ids)) != len(ids):
        dupes = [i for i in set(ids) if ids.count(i) > 1]
        errors.append("duplicate entity ids: {}".format(dupes))

    # Tag of each entity, for reporting.
    tags = []
    for idx, start in enumerate(entity_starts):
        end = entity_starts[idx + 1] if idx + 1 < len(entity_starts) else len(lines)
        block = lines[start:end]
        tag = "?"
        scale = None
        half = None
        has_body = False
        for ln in block:
            m = re.match(r"^      Tag: (.*)$", ln)
            if m:
                tag = m.group(1)
            m = re.match(r"^      Scale: \[([^]]+)\]$", ln)
            if m:
                scale = [float(x) for x in m.group(1).split(",")]
            m = re.match(r"^      HalfExtents: \[([^]]+)\]$", ln)
            if m:
                half = [float(x) for x in m.group(1).split(",")]
            if ln.strip().startswith("RigidBody3DComponent:"):
                has_body = True
        tags.append(tag)
        if has_body and scale and half:
            for axis, (s, h) in enumerate(zip(scale, half)):
                if abs(s / 2.0 - h) > 1e-3:
                    errors.append(
                        "{}: collider axis {} HalfExtents {} != Scale/2 {}".format(
                            tag, "xyz"[axis], h, s / 2.0))

    print("file:", path)
    print("scene:", lines[0].split(": ", 1)[1])
    print("entities:", len(entity_starts))
    print("tags:", ", ".join(tags))

    if errors:
        print()
        for e in errors:
            print("ERROR:", e)
        return 1

    print()
    print("OK: structure valid, ids unique, colliders match visuals.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1
                  else "DemonCore-Editor/assets/scenes/ExampleGame.wastescene"))
