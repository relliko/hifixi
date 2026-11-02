"""What hifps's batch and dedupe (proxy.asm) would do to a d3dlog capture's frame (C:\\Games\\PhoenixXI\\d3dlog\\*.txt):
the calls dedupe drops, the quads batch merges and the draws they become, and how many of those calls our d3d8.dll
already returns from at once (SetRenderState and SetTextureStageState with the value in effect).

  python tools/hifps_proxy_counts.py [capture ...]

Two views of each frame:
  all     the frame as captured (2026-10-05: the game drew everything through Direct3D 8)
  today   the calls the game still makes now that HiFi XI draws the zone, the characters and their shadows itself and
          World FX's VisTest answers the visibility tests: calls whose caller chain runs through those functions are
          left out (by FFXiMain offset; client-notes / d3dlog findings.md). An approximation: the game's leftover state
          calls around them (setup outside those functions) stay in.

The proxy's rules, as proxy.asm has them: a Set* call whose value equals the shadow is dropped (dedupe); the shadow is
forgotten at BeginScene, EndScene, Present, Reset and ApplyStateBlock (Ashita draws and runs plugins there); a UP
draw forgets stream 0. batch takes DrawPrimitiveUP(TRIANGLESTRIP, 2) with a stride 12..64 into a list; anything that
changes a value, any other draw, a Clear, SetRenderTarget, SetViewport, constants, the calls above, flush it.
The capture logs textures by size and format, not by pointer: SetTexture with an equal description counts as the
same texture (so dedupe's SetTexture drops are an upper bound).
"""
import re
import sys
from collections import Counter

CAPTURES = ["day_npcs", "night", "water", "spell", "menu"]
FOLDER = "C:/Games/PhoenixXI/d3dlog/"

# FFXiMain offsets of the code the game no longer runs (or whose Direct3D 8 calls no longer happen)
TODAY_OUT = [
    (0x17ADE0, 0x17B510, "zone: lit models (17ADE0, 183DA0's caller)"),
    (0x17C9C0, 0x17E000, "zone: object draw 17C9C0"),
    (0x183DA0, 0x184500, "zone: lit model pass 183DA0"),
    (0x029F00, 0x02F300, "characters (02DC10 and its setup)"),
    (0x033800, 0x033D00, "characters: silhouette target 033800"),
    (0x184FE0, 0x185C00, "character blob shadows 184FE0"),
    (0x185CA0, 0x186B00, "character projected shadows 185CA0"),
    (0x06C280, 0x06CA00, "visibility tests 06C280 (VisTest answers)"),
]


def parse(path):
    calls = []
    section = 0
    with open(path, "r", errors="replace") as f:
        for line in f:
            if line.startswith("==== "):
                section = int(line[5:].split(".")[0])
                continue
            if section != 7:
                continue
            w = line.split()
            if len(w) < 2 or not w[0].isdigit():
                continue
            op = w[1]
            b = line.rfind("[")
            args = line[line.find(op) + len(op):b if b > 0 else None].strip()
            chain = [int(x, 16) for x in re.findall(r"0x[0-9A-Fa-f]+", line[b:])] if b > 0 else []
            calls.append((op, args, chain))
    return calls


def today(chain):
    return not any(lo <= a < hi for a in chain for lo, hi, _ in TODAY_OUT)


SETTERS = {"RS", "TSS", "Tex", "LightEnable", "VS", "PS", "Stream", "Indices", "SetLight", "SetMaterial", "Xform"}
FORGET = {"BeginScene", "EndScene", "Present", "ApplyStateBlock", "Reset"}


def key_value(op, args):
    """The proxy's shadow key and value for a setter, None if the proxy passes it through unshadowed."""
    if op == "RS":
        name, _, value = args.partition(" = ")
        return ("RS", name.strip()), value.strip()
    if op == "TSS":
        left, _, value = args.partition(" = ")
        stage, name = left.split()[:2]
        if int(stage) >= 8:
            return None
        return ("TSS", stage, name), value.strip()
    if op == "Tex":
        w = args.split()
        if int(w[0]) >= 8:
            return None
        return ("Tex", w[0]), " ".join(w[1:])
    if op == "LightEnable":
        w = args.split()
        if int(w[0]) >= 8:
            return None
        return ("LE", w[0]), w[1]
    if op == "SetLight":
        w = args.split()
        if int(w[0]) >= 8:
            return None
        return ("L", w[0]), " ".join(w[1:])
    if op == "VS":
        return ("VS",), args
    if op == "PS":
        return ("PS",), args
    if op == "Stream":
        w = args.split()
        return ("ST", w[0]), " ".join(w[1:])
    if op == "Indices":
        return ("IX",), args
    if op == "SetMaterial":
        return ("MAT",), args
    if op == "Xform":
        # (the matrices aren't on the call line: every SetTransform counted as a new value, a lower bound for dedupe)
        return None
    return None


def analyse(calls):
    shadow = {}
    out = Counter()
    pending = 0
    batches = 0
    merged = 0
    for op, args, chain in calls:
        out["calls"] += 1
        if op in ("DP", "DIP", "DPUP"):
            out["draws"] += 1
        if op in FORGET:
            if pending:
                batches += 1
                pending = 0
            shadow.clear()
            continue
        if op in SETTERS:
            kv = key_value(op, args)
            if kv is not None:
                k, v = kv
                if shadow.get(k) == v:
                    out["dedupe"] += 1
                    out["dedupe " + op] += 1
                    if op in ("RS", "TSS"):
                        out["dedupe ours already"] += 1
                    continue
                shadow[k] = v
            # (a new value: the pending quads drawn first)
            if pending:
                batches += 1
                pending = 0
            continue
        if op == "DPUP":
            shadow.pop(("ST", "0"), None)
            w = args.split()
            m = re.search(r"stride (\d+)", args)
            stride = int(m.group(1)) if m else 0
            if len(w) >= 4 and w[1] == "TRIANGLESTRIP" and w[3] == "2" and 12 <= stride <= 64 and stride % 4 == 0:
                pending += 1
                merged += 1
                continue
        # anything else: flush
        if pending:
            batches += 1
            pending = 0
    if pending:
        batches += 1
    out["quads"] = merged
    out["batches"] = batches
    return out


def main():
    names = sys.argv[1:] or CAPTURES
    print("%-9s %-5s %6s %5s | %6s %6s %6s | %5s %6s %8s" % ("capture", "view", "calls", "draws", "dedupe", "ours", "left", "quads", "draws", "saved"))
    for name in names:
        calls = parse(FOLDER + name + ".txt")
        for view, sel in (("all", calls), ("today", [c for c in calls if today(c[2])])):
            o = analyse(sel)
            print("%-9s %-5s %6d %5d | %6d %6d %6d | %5d %6d %8d" % (
                name, view, o["calls"], o["draws"], o["dedupe"], o["dedupe ours already"], o["dedupe"] - o["dedupe ours already"],
                o["quads"], o["batches"], o["quads"] - o["batches"]))
            if view == "today":
                kinds = ", ".join("%s %d" % (k[7:], v) for k, v in sorted(o.items()) if k.startswith("dedupe ") and k != "dedupe ours already")
                print("          dropped by kind: " + kinds)
    print("dedupe: calls dropped; ours: of those, SetRenderState/SetTextureStageState our d3d8.dll returns from at once")
    print("anyway; left: the rest. quads: DrawPrimitiveUP quads batch takes; draws: the merged draws they become;")
    print("saved: draws fewer a frame.")


if __name__ == "__main__":
    main()
