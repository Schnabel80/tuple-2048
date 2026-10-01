/* Shared helpers: theme toggle, number formatting, board rendering, glossary. */
"use strict";

const UI = (() => {
  /* ---- theme ---- */
  function initTheme() {
    const btn = document.querySelector(".theme-btn");
    let saved = null;
    try { saved = localStorage.getItem("t2048-theme"); } catch (e) { /* storage blocked */ }
    if (saved) document.documentElement.dataset.theme = saved;
    const label = () => {
      const dark = document.documentElement.dataset.theme
        ? document.documentElement.dataset.theme === "dark"
        : matchMedia("(prefers-color-scheme: dark)").matches;
      if (btn) btn.textContent = dark ? "☀︎ Hell" : "☾ Dunkel";
      return dark;
    };
    label();
    if (btn) btn.addEventListener("click", () => {
      const next = label() ? "light" : "dark";
      document.documentElement.dataset.theme = next;
      try { localStorage.setItem("t2048-theme", next); } catch (e) { /* ignore */ }
      label();
      document.dispatchEvent(new CustomEvent("themechange"));
    });
  }

  /* ---- formatting ---- */
  const nf = new Intl.NumberFormat("de-DE");
  const fmt = (x) => (x == null || Number.isNaN(x) ? "–" : nf.format(Math.round(x)));
  const fmt1 = (x) => new Intl.NumberFormat("de-DE", { maximumFractionDigits: 1 }).format(x);
  const pct = (x) => new Intl.NumberFormat("de-DE", { maximumFractionDigits: 1 }).format(100 * x) + " %";
  function compact(x) {
    if (x >= 1e6) return fmt1(x / 1e6) + " Mio.";
    if (x >= 1e4) return fmt1(x / 1e3) + " Tsd.";
    return fmt(x);
  }
  const tileVal = (e) => (e ? 2 ** e : 0);

  function el(tag, attrs = {}, ...kids) {
    const n = document.createElement(tag);
    for (const [k, v] of Object.entries(attrs)) {
      if (k === "class") n.className = v;
      else if (k === "text") n.textContent = v;
      else if (k === "html") n.innerHTML = v;
      else if (k.startsWith("on")) n.addEventListener(k.slice(2), v);
      else n.setAttribute(k, v);
    }
    for (const k of kids) if (k != null) n.append(k);
    return n;
  }

  /* ---- board view ----
   * Tiles are absolutely positioned via left/top (percent), so a slide is a CSS
   * transition. Animation: draw tiles at their old cells, then move them to the
   * new cells, then swap in the final board with pop/merge effects. */
  class BoardView {
    constructor(host, opts = {}) {
      this.mini = !!opts.mini;
      this.root = el("div", { class: "board" + (this.mini ? " mini" : ""), role: "img", "aria-label": "2048-Spielfeld" });
      this.cellsEl = el("div", { class: "cells" });
      this.tilesEl = el("div", { class: "tiles" });
      this.cells = [];
      for (let i = 0; i < 16; i++) {
        const c = el("div", { class: "cell" });
        this.place(c, i);
        this.cells.push(c);
        this.cellsEl.append(c);
      }
      this.marksEl = el("div", { class: "marks" });
      this.marks = [];
      for (let i = 0; i < 16; i++) {
        const m = el("div", { class: "mark" });
        this.place(m, i);
        this.marks.push(m);
        this.marksEl.append(m);
      }
      this.root.append(this.cellsEl, this.tilesEl, this.marksEl);
      host.append(this.root);
      this.timer = null;
    }
    place(node, i) {
      const step = this.mini ? 26 : 25.75; // tile size + gap, in percent
      node.style.left = (i % 4) * step + "%";
      node.style.top = Math.floor(i / 4) * step + "%";
    }
    tile(i, e, cls = "") {
      const v = tileVal(e);
      const digits = String(v).length;
      const t = el("div", { class: `tile e${Math.min(e, 15)} n${digits} ${cls}` }, el("span", { text: this.mini && v >= 1000 ? String(v) : String(v) }));
      this.place(t, i);
      return t;
    }
    render(board, opts = {}) {
      clearTimeout(this.timer);
      this.tilesEl.replaceChildren();
      board.forEach((e, i) => {
        if (!e && !(opts.wild && opts.wild.includes(i))) return;
        let cls = "";
        if (opts.spawn === i) cls = "pop";
        else if (opts.merged && opts.merged.has(i)) cls = "merge";
        this.tilesEl.append(e ? this.tile(i, e, cls) : Object.assign(el("div", { class: "tile any" }), {}));
      });
      if (opts.wild) [...this.tilesEl.querySelectorAll(".tile.any")].forEach((t, k) => this.place(t, opts.wild[k]));
      this.board = board;
    }
    /* Animate one move: trace from Engine.move, then final board incl. spawn. */
    animate(trace, finalBoard, spawnPos, ms) {
      clearTimeout(this.timer);
      if (!ms || ms < 30) {
        const merged = new Set(trace.filter((t) => t.merge).map((t) => t.to));
        this.render(finalBoard, { spawn: spawnPos, merged });
        return;
      }
      this.root.style.setProperty("--anim", ms + "ms");
      this.tilesEl.replaceChildren();
      const nodes = trace.map((tr) => {
        const n = this.tile(tr.from, tr.e);
        this.tilesEl.append(n);
        return [n, tr.to];
      });
      void this.tilesEl.offsetWidth; // flush layout so the transition starts from the old cell
      for (const [n, to] of nodes) this.place(n, to);
      const merged = new Set(trace.filter((t) => t.merge).map((t) => t.to));
      this.timer = setTimeout(() => this.render(finalBoard, { spawn: spawnPos, merged }), ms);
    }
    /* Outline the given cells on an overlay above the tiles. */
    highlight(cells, color) {
      this.marks.forEach((m, i) => {
        m.classList.toggle("hl", cells.includes(i));
        m.style.setProperty("--hl", color);
      });
    }
    clearHighlight() {
      this.marks.forEach((m) => m.classList.remove("hl"));
    }
  }

  /* ---- glossary popovers: <span class="term" data-term="key"> ---- */
  const GLOSSARY = {
    tupel: "Eine feste Auswahl von Spielfeld-Zellen, z.B. die oberste Reihe. Für jede mögliche Belegung dieser Zellen merkt sich der Agent eine Zahl (ein Gewicht).",
    gewicht: "Eine gelernte Zahl in der Tabelle eines Tupels. Sie sagt: „Wenn diese Zellen genau so belegt sind, ist das so-und-so viel wert.“ Der Agent lernt, indem er Gewichte leicht verschiebt.",
    wert: "Die Schätzung des Agenten, wie viele Punkte er ab einem Spielfeld noch holen wird. Summe aller beteiligten Gewichte.",
    afterstate: "Das Spielfeld direkt nach dem Verschieben – bevor die neue Kachel erscheint. Der Agent bewertet immer diese Zwischenstellung, weil sie vollständig vorhersehbar ist.",
    td: "Temporal-Difference-Learning: Die Vorhersage von jetzt wird mit der Vorhersage einen Schritt später (plus den Punkten dazwischen) verglichen. Die Differenz ist der TD-Fehler – um einen Bruchteil davon wird korrigiert.",
    tdfehler: "Unterschied zwischen dem, was der Agent vorhergesagt hat, und dem, was einen Zug später tatsächlich (besser) bekannt ist. Positiv = er war zu pessimistisch.",
    lernrate: "Wie stark eine einzelne Erfahrung die Gewichte verändert (α). Zu groß: der Agent springt hin und her. Zu klein: er lernt ewig.",
    symmetrie: "Ein Muster ist gedreht oder gespiegelt genauso gut. Darum wird jedes Tupel in 8 Lagen über das Feld gelegt – alle teilen sich dieselbe Tabelle.",
    greedy: "„Gierig“: Der Agent nimmt immer den Zug, den er gerade für den besten hält – ohne Zufallsexperimente.",
    otd: "Optimistische Initialisierung: Am Anfang hält der Agent jede Stellung für sehr wertvoll. Unbekanntes wirkt dadurch verlockend – er probiert systematisch Neues aus.",
    tc: "Temporal Coherence: Jedes Gewicht bekommt seine eigene Lernrate. Wird es immer in dieselbe Richtung korrigiert, lernt es schnell; pendelt es hin und her, wird es eingefroren.",
    expectimax: "Vorausschau: Der Agent spielt im Kopf weiter – seine eigenen Züge (bester wird genommen) und alle möglichen neuen Kacheln (Durchschnitt). Erst am Ende fragt er sein gelerntes Bauchgefühl.",
    multistage: "Getrennte Tabellen für verschiedene Spielphasen, z.B. ab der ersten 16384. Spät im Spiel zählen andere Muster als am Anfang.",
  };

  function initGlossary() {
    let pop = null;
    const close = () => { if (pop) { pop.remove(); pop = null; } };
    document.addEventListener("click", (ev) => {
      const t = ev.target.closest(".term");
      close();
      if (!t) return;
      const text = GLOSSARY[t.dataset.term];
      if (!text) return;
      pop = el("div", { class: "term-pop", role: "tooltip", text });
      document.body.append(pop);
      const r = t.getBoundingClientRect();
      const w = Math.min(300, window.innerWidth - 32);
      pop.style.maxWidth = w + "px";
      pop.style.left = Math.max(16, Math.min(window.scrollX + r.left, window.scrollX + window.innerWidth - w - 16)) + "px";
      pop.style.top = window.scrollY + r.bottom + 6 + "px";
    });
    document.querySelectorAll(".term").forEach((t) => { t.tabIndex = 0; t.addEventListener("keydown", (e) => { if (e.key === "Enter") t.click(); }); });
    document.addEventListener("keydown", (e) => { if (e.key === "Escape") close(); });
  }

  return { initTheme, initGlossary, fmt, fmt1, pct, compact, tileVal, el, BoardView, GLOSSARY };
})();
