"""Tests for the Python tooling (code page generator and data bundler)."""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))

import bundle  # noqa: E402
import gen_codepage  # noqa: E402


class MarkdownTest(unittest.TestCase):
    def test_inline(self):
        html = gen_codepage.markdown("Ein **fetter** und *kursiver* `code < x`")
        self.assertIn("<strong>fetter</strong>", html)
        self.assertIn("<em>kursiver</em>", html)
        self.assertIn("<code>code &lt; x</code>", html)

    def test_escaped_star_is_literal(self):
        self.assertIn("xoshiro256**", gen_codepage.markdown("xoshiro256\\*\\*"))

    def test_blocks(self):
        html = gen_codepage.markdown("## Titel\n\n- a\n- b\n\n| x | y |\n|---|---|\n| 1 | 2 |\n\n```\nraw <b>\n```")
        self.assertIn("<h3>Titel</h3>", html)
        self.assertIn("<ul><li>a</li><li>b</li></ul>", html)
        self.assertIn("<th>x</th>", html)
        self.assertIn("<td>2</td>", html)
        self.assertIn("raw &lt;b&gt;", html)


class ParseSourceTest(unittest.TestCase):
    def test_sections_and_notes(self):
        src = "/*@ ## Datei\n\nIntro */\n#include <x.h>\n\n/*@ ### Funktion\nText */\nint f(void) {\n    return 1; //@ eins\n}\n"
        secs = gen_codepage.parse_source(src)
        self.assertEqual([s["title"] for s in secs], ["Datei", "Funktion"])
        self.assertEqual(secs[0]["lines"][0]["code"], "#include <x.h>")
        ret = [ln for ln in secs[1]["lines"] if ln["note"]][0]
        self.assertEqual(ret["code"], "    return 1;")
        self.assertEqual(ret["note"], "eins")
        self.assertEqual(ret["n"], 9)

    def test_repository_sources_are_annotated(self):
        data = gen_codepage.build()
        errors = [e for e in gen_codepage.check(data) if "stale" not in e]
        self.assertEqual(errors, [])


class ReplayTest(unittest.TestCase):
    def test_compact(self):
        raw = {
            "header": {"games_trained": 10, "seed": 7, "depth": 0},
            "moves": [
                {"t": 1, "board": "1100000000000000", "dir": "L", "reward": 4, "score": 4, "after": "2000000000000000",
                 "spawn": [5, 1], "q": [10.4, None, 3.0, 2.0]},
                {"t": 2, "board": "2000010000000000", "dir": "D", "reward": 0, "score": 4, "after": "0000000000000201",
                 "spawn": [15, 2], "q": [1.0, 2.0, 3.0, 4.0]},
            ],
            "end": {"score": 4, "moves": 2, "max_tile": 4},
        }
        c = bundle.compact_replay(raw)
        self.assertEqual(c["start"], "1100000000000000")
        self.assertEqual(c["dirs"], "LD")
        self.assertEqual(c["spawns"], bundle.B32[10] + bundle.B32[31])
        self.assertEqual(c["q"][0], [10, None, 3, 2])
        self.assertEqual(c["moves"], 2)


if __name__ == "__main__":
    unittest.main()
