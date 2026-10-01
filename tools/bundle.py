#!/usr/bin/env python3
"""Bundle training output (runs/*) into web/data/runs.js for the static dashboard.

The dashboard must work from file:// (double click), where fetch() is blocked, so
all data is shipped as a plain script that assigns window.T2048_DATA.

Configuration lives in web/bundle.json:
  {
    "main":  "runs/showcase",                 # learning curve, patterns, replays
    "small": "runs/small",                    # small net: weights for the tuple lens
    "ab":    [{"dir": "...", "label": "...", "desc": "..."}],
    "replays": [1, 10, 100, 1000, 10000, 100000, 1000000]   # snapshot points to ship
  }

Usage:  python3 tools/bundle.py [--config web/bundle.json]
"""

from __future__ import annotations

import array
import base64
import csv
import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "web" / "data" / "runs.js"
DIRS = "LRUD"
B32 = "0123456789abcdefghijklmnopqrstuv"  # spawn = pos*2 + (exp-1) -> one char


# --------------------------------------------------------------------------- readers


def read_csv(path: Path) -> list[dict]:
    if not path.exists():
        return []
    with path.open() as f:
        rows = []
        for r in csv.DictReader(f):
            rows.append({k: (float(v) if "." in v else int(v)) for k, v in r.items()})
        return rows


def read_replay(path: Path) -> dict:
    """Parse a replay JSONL file into its raw records."""
    header, moves, end = {}, [], {}
    for line in path.read_text().splitlines():
        if not line.strip():
            continue
        rec = json.loads(line)
        if rec.get("type") == "header":
            header = rec
        elif rec.get("type") == "end":
            end = rec
        else:
            moves.append(rec)
    return {"header": header, "moves": moves, "end": end}


def compact_replay(raw: dict) -> dict:
    """Compact form: start board + direction string + spawn string + q values.

    The browser re-simulates the game with its own engine port and verifies it
    against the checkpoint boards, so a broken port is detected instead of
    silently showing a different game.
    """
    moves = raw["moves"]
    if not moves:
        return {}
    q = []
    for m in moves:
        q.append([None if v is None else round(v) for v in m["q"]])
    checks = {str(m["t"]): m["board"] for m in moves if m["t"] % 250 == 0}
    return {
        "games": raw["header"].get("games_trained", 0),
        "seed": raw["header"].get("seed"),
        "depth": raw["header"].get("depth", 0),
        "start": moves[0]["board"],
        "dirs": "".join(m["dir"] for m in moves),
        "spawns": "".join(B32[m["spawn"][0] * 2 + m["spawn"][1] - 1] for m in moves),
        "rewards": [m["reward"] for m in moves],
        "q": q,
        "checks": checks,
        "score": raw["end"].get("score", moves[-1]["score"]),
        "max_tile": raw["end"].get("max_tile", 0),
        "moves": len(moves),
    }


def read_weights(path: Path) -> dict:
    """Read a T2048W weights file (stage 0 only) into python arrays."""
    data = path.read_bytes()
    if data[:8] != b"T2048W\0\0":
        raise ValueError(f"{path}: not a weights file")
    version, endian, n_tuples, n_stages, _flags = struct.unpack_from("<5I", data, 8)
    if version != 1 or endian != 0x01020304:
        raise ValueError(f"{path}: unsupported version/endianness")
    (games,) = struct.unpack_from("<Q", data, 28)
    name = data[36:68].split(b"\0", 1)[0].decode()
    off = 68 + 8 * n_stages
    tuples = []
    for _ in range(n_tuples):
        (ln,) = struct.unpack_from("<I", data, off)
        tuples.append(list(data[off + 4 : off + 4 + ln]))
        off += 4 + ln
    weights = []
    for cells in tuples:
        size = 16 ** len(cells)
        w = array.array("f")
        w.frombytes(data[off : off + 4 * size])
        off += 8 * size  # skip the visit counters
        weights.append(w)
    return {"name": name, "games": games, "tuples": tuples, "weights": weights}


def quantize(weights: list[array.array]) -> dict:
    """float32 -> int16 + one scale per tuple, base64 encoded (halves the size)."""
    out = []
    for w in weights:
        peak = max(abs(x) for x in w) or 1.0
        scale = peak / 32767.0
        q = array.array("h", (int(round(x / scale)) for x in w))
        if sys.byteorder != "little":
            q.byteswap()
        out.append({"scale": scale, "data": base64.b64encode(q.tobytes()).decode()})
    return {"tuples": out}


def read_run(d: Path, replay_points: list[int] | None) -> dict:
    meta = json.loads((d / "meta.json").read_text()) if (d / "meta.json").exists() else {}
    run = {"meta": meta, "milestones": read_csv(d / "milestones.csv"), "eval": read_csv(d / "eval.csv")}
    if replay_points is None:
        return run
    tops = {}
    for p in sorted((d / "top").glob("top_*.json"), key=lambda p: int(p.stem.split("_")[1])):
        tops[p.stem.split("_")[1]] = json.loads(p.read_text())
    run["top"] = tops
    replays = {}
    available = sorted(int(p.stem.split("_")[1]) for p in (d / "replays").glob("replay_*.jsonl"))
    wanted = set(replay_points) | ({available[-1]} if available else set())
    for g in available:
        if g in wanted:
            replays[str(g)] = compact_replay(read_replay(d / "replays" / f"replay_{g}.jsonl"))
    run["replays"] = replays
    extra = d / "showcase.json"  # optional: expectimax evaluation summary written by the showcase script
    if extra.exists():
        run["showcase"] = json.loads(extra.read_text())
    return run


def main(argv: list[str]) -> int:
    cfg_path = ROOT / "web" / "bundle.json"
    if "--config" in argv:
        cfg_path = Path(argv[argv.index("--config") + 1])
    cfg = json.loads(cfg_path.read_text())
    data: dict = {"generated_from": cfg}

    main_dir = ROOT / cfg["main"]
    if (main_dir / "meta.json").exists():
        data["main"] = read_run(main_dir, cfg.get("replays", []))
    else:
        print(f"warning: {main_dir} missing - dashboard will show no main run", file=sys.stderr)

    small_dir = ROOT / cfg.get("small", "")
    if cfg.get("small") and (small_dir / "weights.bin").exists():
        small = read_run(small_dir, [])
        wts = read_weights(small_dir / "weights.bin")
        small["weights"] = {"tuples_cells": wts["tuples"], "games": wts["games"], **quantize(wts["weights"])}
        reps = sorted((small_dir / "replays").glob("replay_*.jsonl"), key=lambda p: int(p.stem.split("_")[1]))
        if reps:
            small["replay"] = compact_replay(read_replay(reps[-1]))
        data["small"] = small

    data["ab"] = []
    for entry in cfg.get("ab", []):
        d = ROOT / entry["dir"]
        if not (d / "milestones.csv").exists():
            print(f"warning: {d} missing - skipped", file=sys.stderr)
            continue
        run = read_run(d, None)
        data["ab"].append({**entry, "meta": run["meta"], "milestones": run["milestones"], "eval": run["eval"]})

    OUT.parent.mkdir(parents=True, exist_ok=True)
    text = "// Generated by tools/bundle.py - do not edit.\nwindow.T2048_DATA = " + json.dumps(data, separators=(",", ":")) + ";\n"
    OUT.write_text(text)
    print(f"wrote {OUT.relative_to(ROOT)} ({len(text) / 1e6:.2f} MB)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
