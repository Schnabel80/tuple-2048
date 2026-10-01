/* The learning page: wires bundled data (window.T2048_DATA) to the chapters. */
"use strict";

(() => {
  const D = window.T2048_DATA || {};
  const MAIN = D.main || null;
  const SMALL = D.small || null;
  const $ = (id) => document.getElementById(id);
  const { el, fmt, pct, compact, BoardView } = UI;
  const DIR_NAME = { L: "Links", R: "Rechts", U: "Hoch", D: "Runter" };
  const TUPLE_COLORS = ["var(--s1)", "var(--s2)", "var(--s3)", "var(--s5)", "var(--s4)", "#7f6fd6", "#888", "#3a9"];

  UI.initTheme();
  UI.initGlossary();

  const smallNet = SMALL && SMALL.weights ? new Engine.Network(SMALL.weights) : null;
  const lastRow = (rows) => (rows && rows.length ? rows[rows.length - 1] : null);

  /* ================================================================ hero */
  function hero() {
    const host = $("hero-stats");
    if (!MAIN) {
      host.append(el("div", { class: "note-box", text: "Noch keine Trainingsdaten gebündelt – siehe README (make train && make site)." }));
      document.querySelectorAll("[data-bind=games]").forEach((n) => (n.textContent = "viele"));
      return;
    }
    const rows = MAIN.milestones;
    const last = lastRow(rows);
    const tail = rows.slice(-10);
    const avg = (k) => tail.reduce((s, r) => s + r[k] * r.window, 0) / tail.reduce((s, r) => s + r.window, 0);
    const best = Math.max(...rows.map((r) => r.max_score));
    const hours = last.elapsed_s / 3600;
    document.querySelectorAll("[data-bind=games]").forEach((n) => (n.textContent = fmt(last.games)));
    const tiles = [
      [compact(last.games), "Trainingspartien"],
      [fmt(avg("avg_score")), "Ø Punkte zuletzt"],
      [pct(avg("rate_2048")), "Partien mit 2048"],
      [pct(avg("rate_8192")), "Partien mit 8192"],
      [fmt(best), "Bester Score im Training"],
      [hours >= 1 ? UI.fmt1(hours) + " h" : fmt(last.elapsed_s / 60) + " min", `Trainingszeit (${MAIN.meta.threads} Threads)`],
    ];
    for (const [v, l] of tiles) host.append(el("div", { class: "stat" }, el("div", { class: "v", text: v }), el("div", { class: "l", text: l })));
    $("footer-meta").textContent = `Netz: ${MAIN.meta.net} · Seed ${MAIN.meta.seed} · Git ${MAIN.meta.git_rev}`;
  }

  /* ================================================================ q bars */
  function renderQ(host, q, chosen) {
    host.replaceChildren();
    // Bars start below the worst option so that differences are visible even when
    // all four values are large and close together (as they are for a trained agent).
    const vals = q.filter((v) => v != null);
    const max = Math.max(...vals), worst = Math.min(...vals);
    const min = vals.length > 1 ? worst - (max - worst) * 0.6 : Math.min(0, max);
    [..."LRUD"].forEach((d, i) => {
      const v = q[i];
      const row = el("div", { class: "qbar" + (v == null ? " invalid" : "") + (d === chosen ? " best" : "") });
      row.append(el("span", { text: DIR_NAME[d] }));
      const fill = el("div", { class: "fill" });
      fill.style.width = v == null ? "0%" : Math.max(2, ((v - min) / (max - min || 1)) * 100) + "%";
      row.append(el("div", { class: "track" }, fill));
      row.append(el("span", { class: "num", text: v == null ? "geht nicht" : fmt(v) }));
      host.append(row);
    });
  }

  function whyText(q, chosen, reward) {
    const ranked = [..."LRUD"].map((d, i) => ({ d, v: q[i] })).filter((x) => x.v != null).sort((a, b) => b.v - a.v);
    if (ranked.length === 1) return `Nur <b>${DIR_NAME[chosen]}</b> war überhaupt möglich.`;
    const [a, b] = ranked;
    const gap = a.v - b.v;
    const rel = gap / Math.max(1, Math.abs(a.v));
    let s = `<b>${DIR_NAME[chosen]}</b> bringt sofort ${fmt(reward)} Punkte; zusammen mit dem geschätzten Wert des Feldes danach ergibt das ${fmt(a.v)}. `;
    s += `Zweitbeste Wahl wäre <b>${DIR_NAME[b.d]}</b> mit ${fmt(gap)} weniger`;
    s += rel < 0.002 ? " – praktisch ein Unentschieden." : rel > 0.05 ? " – eine klare Entscheidung." : ".";
    return s;
  }

  /* ================================================================ chapter 1: play */
  function play() {
    const view = new BoardView($("play-board"));
    let board, score, auto = null;
    const advice = $("play-advice");
    const askBtn = $("play-ask"), autoBtn = $("play-auto");
    if (!smallNet) { askBtn.disabled = true; autoBtn.disabled = true; }

    function reset() {
      board = Engine.newGame();
      score = 0;
      view.render(board);
      $("play-score").textContent = "Punkte: 0";
      advice.replaceChildren();
    }
    function step(d) {
      const m = Engine.move(board, d);
      if (!m.moved) return false;
      const sp = Engine.spawnRandom(m.board);
      score += m.reward;
      view.animate(m.trace, sp.board, sp.pos, auto ? 70 : 110);
      board = sp.board;
      $("play-score").textContent = "Punkte: " + fmt(score) + (Engine.gameOver(board) ? " – Spiel vorbei!" : "");
      if (advice.childElementCount && !auto) ask();
      return true;
    }
    function ask() {
      if (!smallNet) return;
      const q = smallNet.q(board);
      const valid = q.filter((x) => x.q != null);
      advice.replaceChildren(el("h3", { text: "Der Agent würde …", style: "margin:12px 0 4px" }));
      if (!valid.length) { advice.append(el("p", { text: "… nichts mehr tun können." })); return; }
      const best = valid.reduce((a, b) => (b.q > a.q ? b : a));
      const qb = el("div", { class: "qbars" });
      renderQ(qb, q.map((x) => x.q), best.dir);
      advice.append(qb, el("p", { class: "small", html: whyText(q.map((x) => x.q), best.dir, best.reward) }));
    }
    function toggleAuto() {
      if (auto) { clearInterval(auto); auto = null; autoBtn.setAttribute("aria-pressed", "false"); return; }
      autoBtn.setAttribute("aria-pressed", "true");
      auto = setInterval(() => {
        if (Engine.gameOver(board)) { toggleAuto(); return; }
        const q = smallNet.q(board).filter((x) => x.q != null);
        step(q.reduce((a, b) => (b.q > a.q ? b : a)).dir);
      }, 90);
    }
    $("play-new").addEventListener("click", reset);
    askBtn.addEventListener("click", ask);
    autoBtn.addEventListener("click", toggleAuto);
    const KEYS = { ArrowLeft: "L", ArrowRight: "R", ArrowUp: "U", ArrowDown: "D" };
    document.addEventListener("keydown", (e) => {
      if (!KEYS[e.key] || e.target.closest("input, select, textarea")) return;
      const r = view.root.getBoundingClientRect();
      if (r.bottom < 0 || r.top > window.innerHeight) return; // only when the board is on screen
      e.preventDefault();
      step(KEYS[e.key]);
    });
    let t0 = null;
    view.root.addEventListener("pointerdown", (e) => (t0 = [e.clientX, e.clientY]));
    view.root.addEventListener("pointerup", (e) => {
      if (!t0) return;
      const dx = e.clientX - t0[0], dy = e.clientY - t0[1];
      t0 = null;
      if (Math.max(Math.abs(dx), Math.abs(dy)) < 24) return;
      step(Math.abs(dx) > Math.abs(dy) ? (dx > 0 ? "R" : "L") : dy > 0 ? "D" : "U");
    });
    reset();
  }

  /* ================================================================ chapter 2: tuple lens */
  function lens() {
    const host = $("lens-board");
    if (!smallNet) { host.textContent = "Kleines Netz nicht gebündelt."; return; }
    const view = new BoardView(host);
    const rep = SMALL.replay ? Engine.expandReplay(SMALL.replay) : null;
    let board, tupleSel = 0;
    const segs = $("lens-tuples");
    smallNet.tuples.forEach((tp, t) => {
      const b = el("button", { type: "button", "aria-pressed": t === 0 ? "true" : "false", text: `Tupel ${t + 1}` });
      b.style.borderBottom = `3px solid ${TUPLE_COLORS[t]}`;
      b.addEventListener("click", () => {
        tupleSel = t;
        [...segs.children].forEach((c, k) => c.setAttribute("aria-pressed", String(k === t)));
        draw();
      });
      segs.append(b);
    });
    function pick() {
      if (rep && rep.steps.length) {
        const s = rep.steps[Math.floor(rep.steps.length * (0.2 + 0.7 * Math.random()))];
        board = s.before;
      } else board = Engine.newGame();
    }
    function draw() {
      view.render(board);
      const feats = smallNet.features(board);
      const list = $("lens-list");
      list.replaceChildren();
      const mine = feats.filter((f) => f.t === tupleSel);
      const LAGE = ["Original", "90° gedreht", "180° gedreht", "270° gedreht", "gespiegelt", "gespiegelt + 90°", "gespiegelt + 180°", "gespiegelt + 270°"];
      mine.forEach((f) => {
        const row = el("div", { class: "lens-row" },
          el("span", { class: "dot", style: `background:${TUPLE_COLORS[f.t]}` }),
          el("span", { text: LAGE[f.s] }),
          el("span", { class: "val", text: (f.w >= 0 ? "+" : "") + fmt(f.w) }));
        row.addEventListener("pointerenter", () => { view.highlight(f.cells, TUPLE_COLORS[f.t]); row.classList.add("active"); });
        row.addEventListener("pointerleave", () => { view.highlight(mine[0].cells, TUPLE_COLORS[tupleSel]); row.classList.remove("active"); });
        list.append(row);
      });
      const sumT = mine.reduce((s, f) => s + f.w, 0);
      const total = feats.reduce((s, f) => s + f.w, 0);
      list.append(el("div", { class: "lens-sum" }, el("span", { text: `Tupel ${tupleSel + 1} gesamt` }), el("span", { text: fmt(sumT) })));
      view.highlight(mine[0].cells, TUPLE_COLORS[tupleSel]);
      $("lens-total").innerHTML = `Alle 4 Tupel × 8 Lagen = 32 Meinungen. Ihre Summe ist der <b>Wert dieses Feldes: ${fmt(total)}</b> – so viele Punkte traut sich das kleine Netz ab hier noch zu.`;
    }
    $("lens-next").addEventListener("click", () => { pick(); draw(); });
    pick();
    draw();
  }

  /* ================================================================ chapter 3: live TD learning in JS */
  function live() {
    const N_ISO = 8;
    const TUPLES = [[0, 1, 2, 3], [4, 5, 6, 7], [0, 1, 4, 5], [1, 2, 5, 6]];
    const ISO = TUPLES.map((cells) => [...Array(N_ISO).keys()].map((s) => cells.map((c) => Engine.isoCell(c, s))));
    const M = TUPLES.length * N_ISO;
    const ALPHA = 0.1;
    let W = TUPLES.map(() => new Float32Array(65536));

    function idx(b, cells) { return b[cells[0]] | (b[cells[1]] << 4) | (b[cells[2]] << 8) | (b[cells[3]] << 12); }
    function value(b) {
      let v = 0;
      for (let t = 0; t < TUPLES.length; t++) { const w = W[t]; for (const c of ISO[t]) v += w[idx(b, c)]; }
      return v;
    }
    function update(b, err) {
      const step = (ALPHA * err) / M;
      let v = 0;
      for (let t = 0; t < TUPLES.length; t++) { const w = W[t]; for (const c of ISO[t]) { const i = idx(b, c); w[i] += step; v += w[i]; } }
      return v;
    }
    function playAndLearn() {
      let b = Engine.newGame();
      const path = [];
      let score = 0;
      for (;;) {
        let best = null;
        for (const d of Engine.DIRS) {
          const m = Engine.move(b, d);
          if (!m.moved) continue;
          const q = m.reward + value(m.board);
          if (!best || q > best.q) best = { q, after: m.board, reward: m.reward };
        }
        if (!best) break;
        path.push(best);
        score += best.reward;
        b = Engine.spawnRandom(best.after).board;
      }
      // backward TD(0) pass, exactly like td_train_game() in td.c
      let target = 0, shown = null;
      const pick = Math.floor(path.length * 0.6);
      for (let i = path.length - 1; i >= 0; i--) {
        const before = value(path[i].after);
        const err = target - before;
        if (i === pick) shown = { t: i + 1, a: path[i].after, b: path[i + 1] ? path[i + 1].after : null, v: before, r: path[i + 1] ? path[i + 1].reward : 0, vNext: path[i + 1] ? value(path[i + 1].after) : 0, err };
        const vNew = update(path[i].after, err);
        if (i === pick) shown.vNew = vNew;
        target = path[i].reward + vNew;
      }
      return { score, maxExp: Engine.maxExp(b), shown };
    }

    let games = 0, running = false, recent = [], points = [], lastChart = 0, lastShown = null;
    const viewA = new BoardView($("td-a"), { mini: true }), viewB = new BoardView($("td-b"), { mini: true });
    viewA.render(new Array(16).fill(0));
    viewB.render(new Array(16).fill(0));
    const budget = () => [0, 8, 22, 45][+$("live-speed").value];

    function frame() {
      if (!running) return;
      const t0 = performance.now();
      while (performance.now() - t0 < budget()) {
        const r = playAndLearn();
        games++;
        recent.push(r);
        if (recent.length > 100) recent.shift();
        if (games % 50 === 0) points.push([games, recent.reduce((s, x) => s + x.score, 0) / recent.length]);
        if (r.shown) lastShown = r.shown;
      }
      $("live-games").textContent = fmt(games);
      $("live-avg").textContent = fmt(recent.reduce((s, x) => s + x.score, 0) / recent.length);
      $("live-2048").textContent = pct(recent.filter((x) => x.maxExp >= 11).length / recent.length);
      if (performance.now() - lastChart > 700) { drawChart(); showStep(); lastChart = performance.now(); }
      requestAnimationFrame(frame);
    }
    function drawChart() {
      if (points.length < 2) { $("live-chart").replaceChildren(el("p", { class: "small muted", text: "Die Kurve erscheint nach den ersten 100 Partien …" })); return; }
      Chart.line($("live-chart"), {
        series: [{ name: "Ø Punkte (gleitend über 100 Partien)", short: "Ø Punkte", color: "var(--s1)", points }],
        xLabel: "Partien", yLabel: "Punkte", height: 220, table: false,
      });
    }
    function showStep() {
      const s = lastShown;
      if (!s || !s.b) return;
      viewA.render(s.a);
      viewB.render(s.b);
      $("td-t").textContent = s.t;
      const cls = s.err >= 0 ? "pos" : "neg";
      $("td-formula").innerHTML =
        `Vorhersage für das linke Feld:  V = ${fmt(s.v)}<br>` +
        `Einen Zug später bekannt:       Punkte ${fmt(s.r)} + V(rechtes Feld) ${fmt(s.vNext)} = ${fmt(s.r + s.vNext)}<br>` +
        `TD-Fehler:                      <span class="${cls}">${s.err >= 0 ? "+" : ""}${fmt(s.err)}</span>  ` +
        `(${s.err >= 0 ? "zu pessimistisch" : "zu optimistisch"})<br>` +
        `Korrektur (10 % davon):         V wird ${fmt(s.v)} → ${fmt(s.vNew)}`;
    }
    $("live-start").addEventListener("click", () => {
      running = !running;
      $("live-start").textContent = running ? "Pause" : "Weiterlernen";
      if (running) requestAnimationFrame(frame);
    });
    $("live-reset").addEventListener("click", () => {
      W = TUPLES.map(() => new Float32Array(65536));
      games = 0; recent = []; points = []; lastShown = null;
      $("live-games").textContent = "0"; $("live-avg").textContent = "–"; $("live-2048").textContent = "–";
      $("td-formula").textContent = "Alles vergessen. Starte das Lernen erneut.";
      viewA.render(new Array(16).fill(0)); viewB.render(new Array(16).fill(0));
      drawChart();
    });
    drawChart();
  }

  /* ================================================================ chapter 4: learning curve */
  function curve() {
    if (!MAIN) return;
    const rows = MAIN.milestones.filter((r) => r.window > 0);
    const meta = MAIN.meta;
    const firstAt = (k) => { const r = rows.find((x) => x[k] > 0); return r ? r.games : null; };
    const annotations = [];
    const f2048 = firstAt("rate_2048"), f8192 = firstAt("rate_8192"), f16384 = firstAt("rate_16384"), f32768 = firstAt("rate_32768");
    if (meta.tc && meta.tc_after) annotations.push({ x: meta.tc_after, text: "TC an", dy: 0 });
    let mode = "score";
    function draw() {
      const opts = mode === "score"
        ? {
            series: [
              { name: "Ø Punkte", color: "var(--s1)", points: rows.map((r) => [r.games, r.avg_score]) },
              { name: "Bester Score im Abschnitt", short: "Bester", color: "var(--s2)", points: rows.map((r) => [r.games, r.max_score]) },
            ],
            yLabel: "Punkte",
          }
        : {
            series: [
              { name: "2048 erreicht", short: "2048", color: "var(--s1)", points: rows.map((r) => [r.games, 100 * r.rate_2048]) },
              { name: "8192 erreicht", short: "8192", color: "var(--s2)", points: rows.map((r) => [r.games, 100 * r.rate_8192]) },
              { name: "16384 erreicht", short: "16384", color: "var(--s3)", points: rows.map((r) => [r.games, 100 * r.rate_16384]) },
            ],
            yFormat: (v) => UI.fmt1(v) + " %",
            yMax: 100,
            yLabel: "Anteil Partien",
          };
      Chart.line($("curve-chart"), { ...opts, xLog: true, xLabel: "Trainingspartien (logarithmisch)", annotations });
    }
    document.querySelectorAll("#curve-mode button").forEach((b) => b.addEventListener("click", () => {
      mode = b.dataset.mode;
      document.querySelectorAll("#curve-mode button").forEach((x) => x.setAttribute("aria-pressed", String(x === b)));
      draw();
    }));
    draw();
    document.addEventListener("themechange", draw);
    const facts = [];
    if (f2048) facts.push(`die erste <b>2048</b> nach ${fmt(f2048)} Partien`);
    if (f8192) facts.push(`die erste <b>8192</b> nach ${fmt(f8192)}`);
    if (f16384) facts.push(`die erste <b>16384</b> nach ${fmt(f16384)}`);
    if (f32768) facts.push(`sogar eine <b>32768</b> nach ${fmt(f32768)}`);
    if (facts.length) $("curve-facts").append(el("p", { class: "note-box small", html: "Meilensteine dieses Laufs: " + facts.join(", ") + "." }));
    if (meta.tc && meta.tc_after)
      $("curve-facts").append(el("p", { class: "small", style: "margin-top:12px", html:
        `<b>Der Knick bei „TC an“</b> (nach ${fmt(meta.tc_after)} Partien): Ab hier bekommt jedes Gewicht seine eigene Lernrate. ` +
        "Im ersten Moment weiß das Verfahren aber noch nicht, welche Gewichte schon fertig sind – alle lernen kurz mit voller Kraft, " +
        "und der Agent „verlernt“ für ein paar tausend Partien einiges. Dann bremst TC die fertigen Gewichte ab, und die Kurve steigt " +
        "<b>steiler als vorher</b>. Ein sanfteres Einschalten wäre eine mögliche Verbesserung." }));
    $("curve-note").textContent = `Netz „${meta.net}“, ${meta.tuples.length} Tupel × ${meta.tuples[0].length} Zellen, Lernrate ${meta.alpha}` +
      (meta.init ? `, optimistischer Start ${fmt(meta.init)}` : "") + (meta.tc ? `, TC ab ${fmt(meta.tc_after)} Partien` : "") +
      (meta.stages && meta.stages.length > 1 ? `, Spielphasen ab 2^${meta.stages.slice(1).join(", 2^")}` : "") + ".";
  }

  /* ================================================================ chapter 5: replay */
  function replay() {
    if (!MAIN || !MAIN.replays) return;
    const keys = Object.keys(MAIN.replays).map(Number).sort((a, b) => a - b);
    const sel = $("rep-select");
    for (const k of keys) {
      const r = MAIN.replays[k];
      sel.append(el("option", { value: k, text: `nach ${fmt(k)} ${k === 1 ? "Partie" : "Partien"} – ${fmt(r.score)} Punkte, ${fmt(r.max_tile)}er` }));
    }
    const view = new BoardView($("rep-board"));
    let game = null, i = 0, timer = null;
    const SPEEDS = [700, 350, 160, 70, 25, 0];

    function load(k) {
      stop();
      game = Engine.expandReplay(MAIN.replays[k]);
      if (!game.ok) console.warn("replay verification failed for", k);
      i = 0;
      $("rep-seek").max = game.steps.length;
      show();
      $("rep-note").innerHTML = k < 1000
        ? "Beim <b>Anfänger</b> liegen die Werte oft nah beieinander oder sind fast 0 – er „weiß“ noch nicht, was gut ist. Die Wahl ist dann beinahe Zufall."
        : "Der <b>Profi</b> hat klare Vorlieben. Interessant sind die Momente, in denen ein Zug „geht nicht“ ist oder der Abstand knapp wird – dort entscheidet sich die Partie.";
    }
    function boardAt(n) { return n < game.steps.length ? game.steps[n].before : game.final; }
    function show(animStep) {
      const s = game.steps[i];
      if (animStep) {
        const ms = SPEEDS[+$("rep-speed").value];
        view.animate(animStep.trace, animStep.next, animStep.spawn.pos, Math.min(ms * 0.8, 160));
      } else view.render(boardAt(i));
      $("rep-seek").value = i;
      const score = i ? game.steps[i - 1].score : 0;
      $("rep-status").innerHTML = `Zug <b>${fmt(i)}</b> von ${fmt(game.steps.length)} · Punkte <b>${fmt(score)}</b>` + (i >= game.steps.length ? " · <b>Spiel vorbei</b>" : "");
      if (s) {
        renderQ($("rep-q"), s.q, s.dir);
        $("rep-why").innerHTML = whyText(s.q, s.dir, s.reward);
      } else {
        $("rep-q").replaceChildren();
        $("rep-why").textContent = "Kein Zug mehr möglich – das Spiel ist vorbei.";
      }
    }
    function forward() {
      if (i >= game.steps.length) { stop(); return; }
      const s = game.steps[i];
      i++;
      show(s);
    }
    function back() { if (i > 0) { i--; show(); } }
    function tick() {
      const ms = SPEEDS[+$("rep-speed").value];
      if (ms === 0) { for (let k = 0; k < 8 && i < game.steps.length; k++) i++; show(); }
      else forward();
      if (i >= game.steps.length) { stop(); show(); return; }
      timer = setTimeout(tick, ms || 16);
    }
    function stop() { clearTimeout(timer); timer = null; $("rep-play").textContent = "▶ Abspielen"; }
    $("rep-play").addEventListener("click", () => {
      if (timer) { stop(); return; }
      if (i >= game.steps.length) i = 0;
      $("rep-play").textContent = "❚❚ Pause";
      tick();
    });
    $("rep-step").addEventListener("click", () => { stop(); forward(); });
    $("rep-back").addEventListener("click", () => { stop(); back(); });
    $("rep-seek").addEventListener("input", (e) => { stop(); i = +e.target.value; show(); });
    sel.addEventListener("change", () => load(+sel.value));
    sel.value = keys[keys.length - 1];
    load(keys[keys.length - 1]);
  }

  /* ================================================================ chapter 6: patterns */
  function patterns() {
    if (!MAIN || !MAIN.top) return;
    const snaps = Object.keys(MAIN.top).map(Number).sort((a, b) => a - b).filter((k) => k >= 100);
    const sel = $("pat-snap");
    for (const k of snaps) sel.append(el("option", { value: k, text: `${fmt(k)} Partien` }));
    sel.value = snaps[snaps.length - 1];
    let mode = "top_value";
    function draw() {
      const top = MAIN.top[sel.value];
      const host = $("patterns");
      host.replaceChildren();
      top.tuples.forEach((tp) => {
        const color = TUPLE_COLORS[tp.id];
        const shapeHost = el("div");
        const shape = new BoardView(shapeHost, { mini: true });
        shape.render(new Array(16).fill(0));
        shape.highlight(tp.cells, color);
        const grid = el("div", { class: "pgrid" });
        const entries = tp[mode].slice(0, 8);
        if (!entries.length) grid.append(el("div", { class: "small muted", text: "Noch nicht genug Erfahrung für verlässliche Muster." }));
        for (const e of entries) {
          const b = new Array(16).fill(0);
          tp.cells.forEach((c, j) => (b[c] = e.exps[j]));
          const item = el("div", { class: "pitem" });
          const v = new BoardView(item, { mini: true });
          v.render(b);
          v.cells.forEach((c, i) => c.classList.toggle("dim", !tp.cells.includes(i)));
          item.append(el("div", { text: mode === "top_value" ? fmt(e.value) : compact(e.visits) + "×" }));
          grid.append(item);
        }
        host.append(el("div", { class: "ptuple" },
          el("div", {}, shapeHost, el("div", { class: "small muted", style: "text-align:center;margin-top:4px", text: `Tupel ${tp.id + 1}` })),
          grid));
      });
    }
    document.querySelectorAll("#pat-mode button").forEach((b) => b.addEventListener("click", () => {
      mode = b.dataset.mode;
      document.querySelectorAll("#pat-mode button").forEach((x) => x.setAttribute("aria-pressed", String(x === b)));
      draw();
    }));
    sel.addEventListener("change", draw);
    draw();
  }

  /* ================================================================ chapter 7: A/B */
  function ab() {
    const runs = D.ab || [];
    if (!runs.length) { $("ab-chart").append(el("p", { class: "muted", text: "Keine Vergleichsläufe gebündelt." })); return; }
    const colors = ["var(--s1)", "var(--s2)", "var(--s3)", "var(--s4)", "var(--s5)"];
    function draw() {
      Chart.line($("ab-chart"), {
        series: runs.map((r, k) => ({
          name: r.label, short: r.short || r.label, color: colors[k % colors.length],
          points: r.milestones.filter((m) => m.window >= 100).map((m) => [m.games, m.avg_score]),
        })),
        xLog: true, xLabel: "Trainingspartien (logarithmisch)", yLabel: "Ø Punkte",
      });
    }
    draw();
    document.addEventListener("themechange", draw);
    const tbl = el("table", { class: "data" });
    tbl.append(el("tr", {}, ...["Variante", "Partien", "Ø Punkte", "2048", "8192", "16384", "Zeit"].map((h) => el("th", { text: h }))));
    for (const r of runs) {
      const tail = r.milestones.slice(-3);
      const w = tail.reduce((s, m) => s + m.window, 0) || 1;
      const avg = (k) => tail.reduce((s, m) => s + m[k] * m.window, 0) / w;
      const last = lastRow(r.milestones);
      tbl.append(el("tr", {},
        el("td", { text: r.label, style: "text-align:left" }), el("td", { text: compact(last.games) }), el("td", { text: fmt(avg("avg_score")) }),
        el("td", { text: pct(avg("rate_2048")) }), el("td", { text: pct(avg("rate_8192")) }), el("td", { text: pct(avg("rate_16384")) }),
        el("td", { text: fmt(last.elapsed_s / 60) + " min" })));
    }
    $("ab-table").append(el("div", { class: "tbl-scroll", style: "max-height:none" }, tbl), el("p", { class: "small muted", style: "margin-top:6px", text: "Werte: Durchschnitt der letzten drei Messpunkte jedes Laufs." }));
    for (const r of runs) if (r.desc) $("ab-table").append(el("p", { class: "small", html: `<b>${r.label}:</b> ${r.desc}` }));

    const sc = MAIN && MAIN.showcase;
    if (sc && sc.rows) {
      const t = el("table", { class: "data" });
      t.append(el("tr", {}, ...["Vorausschau", "Partien", "Ø Punkte", "2048", "8192", "16384"].map((h) => el("th", { text: h }))));
      for (const row of sc.rows)
        t.append(el("tr", {}, el("td", { text: row.label, style: "text-align:left" }), el("td", { text: fmt(row.games) }), el("td", { text: fmt(row.avg_score) }),
          el("td", { text: pct(row.rate_2048) }), el("td", { text: pct(row.rate_8192) }), el("td", { text: pct(row.rate_16384) })));
      $("showcase").append(el("h3", { text: "Lernen + Vorausschau", style: "margin-top:18px" }),
        el("p", { class: "small", text: "Das fertige Netz aus Kapitel 4, gespielt ohne und mit Expectimax-Vorausschau:" }),
        el("div", { class: "tbl-scroll", style: "max-height:none" }, t),
        el("p", { class: "small", style: "margin-top:8px", text: "Das Netz ist in allen drei Zeilen exakt dasselbe – nur das Nachdenken vor jedem Zug unterscheidet sich. Lernen liefert das Bauchgefühl, Vorausschau nutzt es geschickter. Der Preis: Jede Ebene kostet etwa 20–30-mal so viel Rechenzeit pro Zug." }));
    }
  }

  /* ================================================================ glossary */
  function glossary() {
    const NAMES = { tupel: "Tupel", gewicht: "Gewicht", wert: "Wert V(s)", afterstate: "Afterstate", td: "TD-Learning", tdfehler: "TD-Fehler",
      lernrate: "Lernrate α", symmetrie: "Symmetrie", greedy: "Greedy", otd: "Optimistischer Start (OTD)", tc: "TC-Learning",
      expectimax: "Expectimax", multistage: "Spielphasen (Multi-Stage)" };
    const dl = $("glossary");
    for (const [k, name] of Object.entries(NAMES)) dl.append(el("dt", { text: name }), el("dd", { text: UI.GLOSSARY[k] }));
  }

  hero();
  play();
  lens();
  live();
  curve();
  replay();
  patterns();
  ab();
  glossary();
})();
