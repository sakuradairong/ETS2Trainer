#!/usr/bin/env python3
"""Build a local-only fuel catalog from extracted ETS2 1.61 stock archives.

Pass one --root per extraction directory. Game definition text is generated into
the ignored cpp/build directory and must not be distributed with source code.
"""
import argparse
import json
import re
from pathlib import Path

TANK = re.compile(r"^\s*tank_size\s*:\s*(\d+(?:\.\d+)?)(?:[ \t]*#[^\r\n]*)?[ \t]*$", re.M)
ELECTRIC = "def/vehicle/truck/scania.s_2024e/chassis/4x2.sii"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", action="append", required=True, type=Path,
                        help="Directory produced by the official SCS extractor")
    parser.add_argument("--output", type=Path,
                        default=Path(__file__).resolve().parents[2] / "cpp/build/fueltank_definitions.generated.h")
    args = parser.parse_args()
    entries = {}
    for root in args.root:
        if not (root / "def/vehicle/truck").is_dir():
            parser.error(f"Missing def/vehicle/truck in {root}")
        for path in sorted((root / "def/vehicle/truck").glob("*/chassis/*.sii")):
            if path.is_symlink():
                parser.error(f"Symbolic link is not supported: {path}")
            virtual = path.relative_to(root).as_posix()
            text = path.read_text(encoding="utf-8-sig")
            values = TANK.findall(text)
            if len(values) != 1 or "accessory_chassis_data" not in text:
                continue
            electric = virtual == ELECTRIC
            if ("#kwh" in text.lower() or "2024e/" in virtual) and not electric:
                continue
            if ")FUELDEF\"" in text:
                parser.error(f"Unsupported raw string delimiter in {path}")
            entry = (text, electric)
            if virtual in entries and entries[virtual] != entry:
                parser.error(f"Conflicting definitions for {virtual}")
            entries[virtual] = entry
    if not entries:
        parser.error("No supported chassis definitions found")
    output = ["// Local game definitions. Generated; do not commit or redistribute.",
              "#pragma once", "namespace ets2 {",
              "inline constexpr FuelTankDefinition kFuelTankDefinitions[] = {"]
    for virtual, (text, electric) in sorted(entries.items()):
        output.append("{" + json.dumps("/" + virtual) + ', R"FUELDEF(' + text +
                      ')FUELDEF", ' + str(electric).lower() + "},")
    output.extend(["};", "}", ""])
    args.output.parent.mkdir(parents=True, exist_ok=True)
    # Validate everything before replacing the local generated catalog.
    stage = args.output.with_name(args.output.name + ".tmp")
    created = False
    try:
        with stage.open("x", encoding="utf-8", newline="\n") as stream:
            created = True
            stream.write("\n".join(output))
        stage.replace(args.output)
    finally:
        if created:
            stage.unlink(missing_ok=True)
    print(f"Generated {len(entries)} chassis definitions ({sum(v[1] for v in entries.values())} electric): {args.output}")


if __name__ == "__main__":
    main()
