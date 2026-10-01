#!/usr/bin/env python3
"""Evaluate a trained network with and without expectimax search.

Writes <run>/showcase.json, which tools/bundle.py ships to the dashboard
("Lernen + Vorausschau" table in chapter 7).

Usage: python3 tools/showcase.py runs/showcase [--games 1000,300,40] [--threads 4]
       games per depth: depth 0, depth 1, depth 2 (deeper search is ~100x slower per level)
"""

from __future__ import annotations

import csv
import io
import json
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LABELS = ["ohne (nur Bauchgefühl)", "1 Zufallsebene", "2 Zufallsebenen"]


def main(argv: list[str]) -> int:
    if not argv:
        print(__doc__)
        return 1
    run = Path(argv[0])
    games = [1000, 300, 40]
    threads = 4
    if "--games" in argv:
        games = [int(x) for x in argv[argv.index("--games") + 1].split(",")]
    if "--threads" in argv:
        threads = int(argv[argv.index("--threads") + 1])
    rows = []
    for depth, n in enumerate(games):
        t0 = time.time()
        out = subprocess.run(
            [str(ROOT / "t2048"), "eval", "--weights", str(run / "weights.bin"), "--games", str(n), "--depth", str(depth),
             "--threads", str(threads), "--seed", "4242"],
            check=True, capture_output=True, text=True,
        ).stdout
        r = next(csv.DictReader(io.StringIO(out)))
        row = {
            "depth": depth,
            "label": LABELS[depth] if depth < len(LABELS) else f"Tiefe {depth}",
            "games": int(r["window"]),
            "avg_score": float(r["avg_score"]),
            "max_score": int(r["max_score"]),
            "rate_2048": float(r["rate_2048"]),
            "rate_8192": float(r["rate_8192"]),
            "rate_16384": float(r["rate_16384"]),
            "rate_32768": float(r["rate_32768"]),
            "seconds": round(time.time() - t0, 1),
        }
        rows.append(row)
        print(json.dumps(row), flush=True)
    (run / "showcase.json").write_text(json.dumps({"rows": rows}, indent=1) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
