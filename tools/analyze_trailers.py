#!/usr/bin/env python3
"""Map the trailer graph of a converted SiiNunit text save (research tool).

Usage: python analyze_trailers.py <save.sii>
"""
import re
import sys
from collections import defaultdict

UNIT_RE = re.compile(r"^(\w+) : (.+) \{$")
FIELD_RE = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)(?:\[(\d+)\])?: (.*)$")


def parse(path):
    units = []
    cur = None
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            m = UNIT_RE.match(line)
            if m:
                cur = {"type": m.group(1), "id": m.group(2), "fields": {},
                       "arrays": defaultdict(list)}
                units.append(cur)
                continue
            if line == "}":
                cur = None
                continue
            if cur is None:
                continue
            m = FIELD_RE.match(line)
            if m:
                name, idx, val = m.group(1), m.group(2), m.group(3)
                if idx is None:
                    cur["fields"][name] = val
                else:
                    cur["arrays"][name].append(val)
    return units


def main():
    units = parse(sys.argv[1])
    by_id = {u["id"]: u for u in units}
    print("units: %d" % len(units))

    players = [u for u in units if u["type"] == "player"]
    for p in players:
        print("\n== player %s ==" % p["id"])
        for k in ("my_vehicles_mode", "assigned_vehicles_mode"):
            print("  %s: %s" % (k, p["fields"].get(k)))
        for name in ("my_vehicles", "assigned_vehicles", "trailers", "trailer_defs",
                     "current_job", "selected_job", "assigned_truck", "assigned_trailer",
                     "my_truck", "my_trailer", "my_trailer_attached"):
            vals = p["arrays"].get(name) or ([p["fields"][name]] if name in p["fields"] else [])
            if vals:
                print("  %s: %s" % (name, vals))
        for pv_id in p["arrays"].get("my_vehicles", []):
            pv = by_id.get(pv_id.removeprefix("_nameless.")) or by_id.get(pv_id)
            pv = by_id.get(pv_id)
            if pv:
                print("  slot %s: vehicle=%s trailer=%s attached=%s placements=%d" % (
                    pv_id, pv["fields"].get("vehicle"), pv["fields"].get("trailer"),
                    pv["fields"].get("stored_trailer_attached"),
                    int(pv["fields"].get("stored_trailer_placements", "0"))))

    # trailers and who references them
    refs = defaultdict(list)
    for u in units:
        for name, vals in u["arrays"].items():
            if name in ("trailers", "company_trailer", "delivered_trailer",
                        "reserved_trailer", "assigned_trailer", "my_trailer"):
                for v in vals:
                    refs[v].append("%s.%s[]" % (u["type"], name))
        for name in ("company_trailer", "delivered_trailer", "assigned_trailer",
                     "my_trailer", "trailer"):
            v = u["fields"].get(name)
            if v and v.startswith("_nameless"):
                refs[v].append("%s.%s" % (u["type"], name))

    print("\n== trailer units ==")
    for t in [u for u in units if u["type"] == "trailer"]:
        tdef = by_id.get(t["fields"].get("trailer_definition", ""))
        tdef_desc = "?"
        if tdef:
            tdef_desc = "%s source=%s body=%s chain=%s" % (
                tdef["fields"].get("trailer"), tdef["fields"].get("source_name"),
                tdef["fields"].get("body_type"), tdef["fields"].get("chain_type"))
        # license plate from drv plate accessory
        plate = ""
        for acc_id in t["arrays"].get("accessories", []):
            acc = by_id.get(acc_id)
            if acc and acc["type"] == "vehicle_drv_plate_accessory":
                plate = acc["fields"].get("text", "")
        print("  %s def=%s plate=%s slave=%s private=%s cargo=%s refs=%s" % (
            t["id"], tdef_desc, plate, t["fields"].get("slave_trailer"),
            t["fields"].get("is_private"), t["fields"].get("cargo_mass"),
            refs.get(t["id"], [])))

    print("\n== player_job units ==")
    for j in [u for u in units if u["type"] == "player_job"]:
        print("  %s company_trailer=%s cargo=%s" % (
            j["id"], j["fields"].get("company_trailer"), j["fields"].get("cargo")))

    print("\n== garage trailer slots ==")
    for g in [u for u in units if u["type"] == "garage"]:
        tr = g["arrays"].get("trailers", [])
        if tr:
            print("  %s: %s" % (g["fields"].get("spawn_place_hint", g["id"]), tr))

    print("\n== driver units with trailers ==")
    for d in [u for u in units if u["type"] == "driver"]:
        at = d["fields"].get("assigned_trailer")
        if at and at != "null":
            print("  %s assigned_trailer=%s" % (d["id"], at))


if __name__ == "__main__":
    main()
