/* 2048 rules in JavaScript – a straight port of src/board.c, but on a plain
 * array of 16 exponents instead of a bitboard (speed does not matter here,
 * readability does). Also tracks where each tile moves, for the animation. */
"use strict";

const Engine = (() => {
  const DIRS = "LRUD";

  // Cell indices of the four lines for each direction, ordered towards the wall.
  const LINES = {
    L: [0, 1, 2, 3].map((r) => [0, 1, 2, 3].map((c) => 4 * r + c)),
    R: [0, 1, 2, 3].map((r) => [3, 2, 1, 0].map((c) => 4 * r + c)),
    U: [0, 1, 2, 3].map((c) => [0, 1, 2, 3].map((r) => 4 * r + c)),
    D: [0, 1, 2, 3].map((c) => [3, 2, 1, 0].map((r) => 4 * r + c)),
  };

  function fromHex(hex) {
    return Array.from(hex, (ch) => parseInt(ch, 16));
  }
  function toHex(b) {
    return b.map((e) => e.toString(16)).join("");
  }

  /* Slide + merge, exactly like slide_left() in board.c (32768 + 32768 do not merge). */
  function move(board, dir) {
    const out = new Array(16).fill(0);
    const trace = [];
    let reward = 0;
    for (const line of LINES[dir]) {
      const tiles = line.filter((i) => board[i]).map((i) => ({ i, e: board[i] }));
      let o = 0;
      for (let k = 0; k < tiles.length; k++) {
        const to = line[o++];
        if (k + 1 < tiles.length && tiles[k].e === tiles[k + 1].e && tiles[k].e < 15) {
          out[to] = tiles[k].e + 1;
          reward += 2 ** (tiles[k].e + 1);
          trace.push({ from: tiles[k].i, to, e: tiles[k].e }, { from: tiles[k + 1].i, to, e: tiles[k].e, merge: true });
          k++;
        } else {
          out[to] = tiles[k].e;
          trace.push({ from: tiles[k].i, to, e: tiles[k].e });
        }
      }
    }
    const moved = out.some((e, i) => e !== board[i]);
    return { board: out, reward, moved, trace };
  }

  function empties(b) {
    const r = [];
    b.forEach((e, i) => { if (!e) r.push(i); });
    return r;
  }

  function spawnRandom(b) {
    const free = empties(b);
    if (!free.length) return { board: b, pos: -1, exp: 0 };
    const pos = free[Math.floor(Math.random() * free.length)];
    const exp = Math.random() < 0.9 ? 1 : 2;
    const nb = b.slice();
    nb[pos] = exp;
    return { board: nb, pos, exp };
  }

  function newGame() {
    return spawnRandom(spawnRandom(new Array(16).fill(0)).board).board;
  }

  function gameOver(b) {
    return [...DIRS].every((d) => !move(b, d).moved);
  }

  function maxExp(b) {
    return Math.max(...b);
  }

  /* Symmetry s of a cell: mirror if s >= 4, then rotate (s mod 4) times – same as iso_cell() in ntuple.c. */
  function isoCell(cell, s) {
    let r = Math.floor(cell / 4), c = cell % 4;
    if (s >= 4) c = 3 - c;
    for (let k = 0; k < (s & 3); k++) [r, c] = [c, 3 - r];
    return 4 * r + c;
  }

  /* An n-tuple network loaded from the bundled (int16-quantised) weights. */
  class Network {
    constructor(spec) {
      this.tuples = spec.tuples_cells.map((cells, t) => {
        const bytes = Uint8Array.from(atob(spec.tuples[t].data), (ch) => ch.charCodeAt(0));
        const q = new Int16Array(bytes.buffer);
        const w = new Float32Array(q.length);
        const scale = spec.tuples[t].scale;
        for (let i = 0; i < q.length; i++) w[i] = q[i] * scale;
        const iso = [];
        for (let s = 0; s < 8; s++) iso.push(cells.map((c) => isoCell(c, s)));
        return { cells, iso, w };
      });
      this.m = this.tuples.length * 8;
    }
    index(b, cells) {
      let idx = 0;
      for (let j = 0; j < cells.length; j++) idx += b[cells[j]] * 16 ** j;
      return idx;
    }
    /* Every single contribution: [{t, s, cells, idx, w}] – used by the tuple lens. */
    features(b) {
      const out = [];
      this.tuples.forEach((tp, t) => tp.iso.forEach((cells, s) => {
        const idx = this.index(b, cells);
        out.push({ t, s, cells, idx, w: tp.w[idx] });
      }));
      return out;
    }
    value(b) {
      let v = 0;
      for (const tp of this.tuples) for (const cells of tp.iso) v += tp.w[this.index(b, cells)];
      return v;
    }
    /* Greedy move choice: q = reward + V(afterstate) for every direction. */
    q(b) {
      return [...DIRS].map((d) => {
        const m = move(b, d);
        return m.moved ? { dir: d, q: m.reward + this.value(m.board), reward: m.reward, after: m.board } : { dir: d, q: null };
      });
    }
  }

  /* Rebuild a full game from a compact replay; verifies against checkpoint boards. */
  function expandReplay(rep) {
    const B32 = "0123456789abcdefghijklmnopqrstuv";
    const steps = [];
    let b = fromHex(rep.start);
    let score = 0, ok = true;
    for (let t = 0; t < rep.dirs.length; t++) {
      const d = rep.dirs[t];
      const m = move(b, d);
      const sp = B32.indexOf(rep.spawns[t]);
      const pos = sp >> 1, exp = (sp & 1) + 1;
      const next = m.board.slice();
      next[pos] = exp;
      score += m.reward;
      if (m.reward !== rep.rewards[t]) ok = false;
      const chk = rep.checks[String(t + 1)];
      if (chk && chk !== toHex(b)) ok = false;
      steps.push({ t: t + 1, before: b, dir: d, after: m.board, trace: m.trace, reward: m.reward, score, spawn: { pos, exp }, next, q: rep.q[t] });
      b = next;
    }
    return { steps, final: b, ok };
  }

  return { DIRS, move, newGame, spawnRandom, gameOver, maxExp, fromHex, toHex, isoCell, Network, expandReplay, empties };
})();
