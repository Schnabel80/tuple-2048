#!/usr/bin/env python3
"""Generate web/data/code.js from the annotated C sources.

Annotation convention (see CLAUDE.md):
  /*@ Markdown text ... */   starts a documented section; the code below it belongs to it
  code;  //@ note            line note, shown next to the line, stripped from the code view

Usage:
  python3 tools/gen_codepage.py           # write web/data/code.js
  python3 tools/gen_codepage.py --check   # fail if annotations are missing or code.js is stale
"""

from __future__ import annotations

import html
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "src"
OUT = ROOT / "web" / "data" / "code.js"

# Reading order for the code walkthrough: bottom-up, from the board to the CLI.
MODULES = [
    ("rng.h", "Zufall"),
    ("board.h", "Spielfeld"),
    ("board.c", "Spielfeld"),
    ("ntuple.h", "Bewertung"),
    ("ntuple.c", "Bewertung"),
    ("td.h", "Lernen"),
    ("td.c", "Lernen"),
    ("search.c", "Vorausschau"),
    ("io.h", "Ausgabe"),
    ("io.c", "Ausgabe"),
    ("train.h", "Training"),
    ("train.c", "Training"),
    ("main.c", "Kommandozeile"),
]


# --------------------------------------------------------------------------- markdown


def _inline(text: str) -> str:
    """Inline markdown: `code`, **bold**, *italic*. Input is raw text, output is HTML."""
    parts = re.split(r"(`[^`]+`)", text)
    out = []
    for part in parts:
        if part.startswith("`") and part.endswith("`") and len(part) > 1:
            out.append("<code>" + html.escape(part[1:-1]) + "</code>")
            continue
        s = html.escape(part, quote=False)
        s = s.replace("\\*", "\x00")
        s = re.sub(r"\*\*(.+?)\*\*", r"<strong>\1</strong>", s)
        s = re.sub(r"(?<![\w*])\*(?!\s)(.+?)(?<!\s)\*(?![\w*])", r"<em>\1</em>", s)
        out.append(s.replace("\x00", "*"))
    return "".join(out)


def markdown(md: str) -> str:
    """Tiny markdown subset: headings, paragraphs, lists, tables, fenced code."""
    lines = md.split("\n")
    out: list[str] = []
    i = 0
    while i < len(lines):
        line = lines[i]
        stripped = line.strip()
        if not stripped:
            i += 1
            continue
        if stripped.startswith("```"):
            j = i + 1
            buf = []
            while j < len(lines) and not lines[j].strip().startswith("```"):
                buf.append(lines[j])
                j += 1
            out.append("<pre class='diagram'>" + html.escape("\n".join(buf)) + "</pre>")
            i = j + 1
            continue
        m = re.match(r"^(#{1,4})\s+(.*)$", stripped)
        if m:
            level = len(m.group(1)) + 1  # "##" in source -> <h3>, page owns h1/h2
            out.append(f"<h{level}>{_inline(m.group(2))}</h{level}>")
            i += 1
            continue
        if stripped.startswith("|"):
            rows = []
            while i < len(lines) and lines[i].strip().startswith("|"):
                cells = [c.strip() for c in lines[i].strip().strip("|").split("|")]
                if not all(re.fullmatch(r":?-{2,}:?", c) for c in cells):
                    rows.append(cells)
                i += 1
            t = ["<table>"]
            for k, row in enumerate(rows):
                tag = "th" if k == 0 else "td"
                t.append("<tr>" + "".join(f"<{tag}>{_inline(c)}</{tag}>" for c in row) + "</tr>")
            t.append("</table>")
            out.append("".join(t))
            continue
        if re.match(r"^(-|\d+\.)\s+", stripped):
            ordered = bool(re.match(r"^\d+\.", stripped))
            items: list[str] = []
            while i < len(lines) and lines[i].strip():
                s = lines[i].strip()
                mm = re.match(r"^(-|\d+\.)\s+(.*)$", s)
                if mm:
                    items.append(mm.group(2))
                elif items:
                    items[-1] += " " + s  # continuation line
                i += 1
            tag = "ol" if ordered else "ul"
            out.append(f"<{tag}>" + "".join(f"<li>{_inline(it)}</li>" for it in items) + f"</{tag}>")
            continue
        para = []
        while i < len(lines) and lines[i].strip() and not re.match(r"^(#|\||```|-\s|\d+\.\s)", lines[i].strip()):
            para.append(lines[i].strip())
            i += 1
        out.append("<p>" + _inline(" ".join(para)) + "</p>")
    return "\n".join(out)


# --------------------------------------------------------------------------- parser


def parse_source(text: str) -> list[dict]:
    """Split a source file into sections of {doc, title, lines:[{n, code, note}]}."""
    sections: list[dict] = []
    current = {"doc": "", "lines": []}
    lines = text.split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    i = 0
    while i < len(lines):
        line = lines[i]
        if line.lstrip().startswith("/*@"):
            doc_lines = []
            first = line.lstrip()[3:]
            j = i
            body = first
            while "*/" not in body:
                doc_lines.append(body)
                j += 1
                body = lines[j]
            doc_lines.append(body[: body.index("*/")])
            rest = body[body.index("*/") + 2 :].strip()
            if current["doc"] or current["lines"]:
                sections.append(current)
            current = {"doc": "\n".join(dl.strip() if k == 0 else dl for k, dl in enumerate(doc_lines)).strip(), "lines": []}
            if rest:
                current["lines"].append({"n": j + 1, "code": rest, "note": ""})
            i = j + 1
            continue
        note = ""
        code = line
        idx = line.find("//@")
        if idx >= 0:
            note = line[idx + 3 :].strip()
            code = line[:idx].rstrip()
        current["lines"].append({"n": i + 1, "code": code, "note": note})
        i += 1
    sections.append(current)
    for s in sections:
        while s["lines"] and not s["lines"][0]["code"].strip():
            s["lines"].pop(0)
        while s["lines"] and not s["lines"][-1]["code"].strip():
            s["lines"].pop()
        m = re.match(r"^#{1,4}\s+(.*)$", s["doc"].split("\n", 1)[0]) if s["doc"] else None
        s["title"] = m.group(1).strip() if m else ""
        s["html"] = markdown(s["doc"])
    return sections


def build() -> dict:
    modules = []
    for name, group in MODULES:
        text = (SRC / name).read_text(encoding="utf-8")
        sections = parse_source(text)
        n_lines = text.count("\n")
        n_notes = sum(1 for s in sections for ln in s["lines"] if ln["note"])
        modules.append(
            {
                "file": name,
                "group": group,
                "title": sections[0]["title"] if sections else name,
                "lines": n_lines,
                "notes": n_notes,
                "sections": [{"title": s["title"], "html": s["html"], "lines": s["lines"]} for s in sections],
            }
        )
    return {"modules": modules}


def render(data: dict) -> str:
    return "// Generated by tools/gen_codepage.py - do not edit.\nwindow.T2048_CODE = " + json.dumps(data, ensure_ascii=False, separators=(",", ":")) + ";\n"


def check(data: dict) -> list[str]:
    errors = []
    listed = {name for name, _ in MODULES}
    for path in sorted(SRC.glob("*.[ch]")):
        if path.name not in listed:
            errors.append(f"{path.name}: not listed in MODULES of tools/gen_codepage.py")
    for mod in data["modules"]:
        first = mod["sections"][0]
        if not first["html"]:
            errors.append(f"{mod['file']}: must start with a /*@ ... */ overview block")
        for sec in mod["sections"][1:]:
            if not sec["title"]:
                errors.append(f"{mod['file']}: section without '### heading' near line {sec['lines'][0]['n'] if sec['lines'] else '?'}")
    if not OUT.exists() or OUT.read_text(encoding="utf-8") != render(data):
        errors.append("web/data/code.js is stale - run: python3 tools/gen_codepage.py")
    return errors


def main(argv: list[str]) -> int:
    data = build()
    if "--check" in argv:
        errors = check(data)
        for e in errors:
            print("codepage:", e, file=sys.stderr)
        return 1 if errors else 0
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(render(data), encoding="utf-8")
    total = sum(m["lines"] for m in data["modules"])
    notes = sum(m["notes"] for m in data["modules"])
    print(f"wrote {OUT.relative_to(ROOT)}: {len(data['modules'])} modules, {total} lines, {notes} line notes")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
