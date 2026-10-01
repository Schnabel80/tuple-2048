/* Minimal SVG line chart: log or linear x, one y axis, crosshair tooltip,
 * legend, direct end labels, optional vertical annotations and a table view. */
"use strict";

const Chart = (() => {
  const NS = "http://www.w3.org/2000/svg";
  const svg = (tag, attrs = {}) => {
    const n = document.createElementNS(NS, tag);
    for (const [k, v] of Object.entries(attrs)) n.setAttribute(k, v);
    return n;
  };

  function niceTicks(min, max, count) {
    const span = max - min || 1;
    const step0 = span / count;
    const mag = 10 ** Math.floor(Math.log10(step0));
    const step = [1, 2, 2.5, 5, 10].map((m) => m * mag).find((s) => span / s <= count) || 10 * mag;
    const ticks = [];
    for (let v = Math.ceil(min / step) * step; v <= max + 1e-9; v += step) ticks.push(+v.toFixed(10));
    return ticks;
  }

  /**
   * opts: { series:[{name, color:'var(--s1)', points:[[x,y],...]}], xLog, xLabel, yLabel,
   *         yFormat(v), xFormat(v), yMin, yMax, annotations:[{x, text}], height, table:true }
   */
  function line(host, opts) {
    host.replaceChildren();
    host.classList.add("chart");
    // Render at the real pixel width so text stays at its CSS size; re-render on resize.
    const W = Math.max(300, Math.round(host.getBoundingClientRect().width) || 720);
    host._chartOpts = opts;
    host._chartW = W;
    if (!host._chartRO && window.ResizeObserver) {
      let raf = 0;
      host._chartRO = new ResizeObserver(() => {
        cancelAnimationFrame(raf);
        raf = requestAnimationFrame(() => {
          const w = Math.round(host.getBoundingClientRect().width);
          if (w && Math.abs(w - host._chartW) > 16) line(host, host._chartOpts);
        });
      });
      host._chartRO.observe(host);
    }
    const narrow = W < 480;
    const H = opts.height || (narrow ? 260 : 320), M = { l: narrow ? 50 : 62, r: narrow ? 64 : 92, t: 14, b: 40 };
    const pw = W - M.l - M.r, ph = H - M.t - M.b;
    const all = opts.series.flatMap((s) => s.points);
    if (!all.length) { host.textContent = "Keine Daten."; return; }
    const xs = all.map((p) => p[0]), ys = all.map((p) => p[1]);
    const xMin = Math.min(...xs), xMax = Math.max(...xs);
    const yMin = opts.yMin ?? 0, yMax = opts.yMax ?? Math.max(...ys) * 1.06;
    const lx = (v) => (opts.xLog ? Math.log10(Math.max(v, 1e-9)) : v);
    const X = (v) => M.l + ((lx(v) - lx(xMin)) / (lx(xMax) - lx(xMin) || 1)) * pw;
    const Y = (v) => M.t + ph - ((v - yMin) / (yMax - yMin || 1)) * ph;
    const yf = opts.yFormat || UI.fmt, xf = opts.xFormat || UI.compact;

    const root = svg("svg", { viewBox: `0 0 ${W} ${H}`, role: "img", "aria-label": opts.ariaLabel || opts.yLabel || "Diagramm" });

    // grid + y axis
    const ay = svg("g", { class: "axis" });
    for (const t of niceTicks(yMin, yMax, 5)) {
      ay.append(svg("line", { class: "gridline", x1: M.l, x2: M.l + pw, y1: Y(t), y2: Y(t) }));
      const tx = svg("text", { x: M.l - 8, y: Y(t) + 4, "text-anchor": "end" });
      tx.textContent = yf(t);
      ay.append(tx);
    }
    // x axis
    let xt;
    if (opts.xLog) {
      xt = [];
      for (let p = Math.floor(Math.log10(Math.max(xMin, 1))); 10 ** p <= xMax * 1.0001; p++) if (10 ** p >= xMin * 0.999) xt.push(10 ** p);
    } else xt = niceTicks(xMin, xMax, narrow ? 4 : 6);
    for (const t of xt) {
      const tx = svg("text", { x: X(t), y: M.t + ph + 18, "text-anchor": "middle" });
      tx.textContent = xf(t);
      ay.append(tx);
    }
    ay.append(svg("line", { x1: M.l, x2: M.l + pw, y1: M.t + ph, y2: M.t + ph }));
    if (opts.xLabel) {
      const t = svg("text", { x: M.l + pw / 2, y: H - 4, "text-anchor": "middle" });
      t.textContent = opts.xLabel;
      ay.append(t);
    }
    root.append(ay);

    // annotations
    for (const a of opts.annotations || []) {
      if (a.x < xMin || a.x > xMax) continue;
      const g = svg("g", { class: "annot" });
      g.append(svg("line", { x1: X(a.x), x2: X(a.x), y1: M.t, y2: M.t + ph }));
      const t = svg("text", { x: X(a.x) + 4, y: M.t + 12 + (a.dy || 0) });
      t.textContent = a.text;
      g.append(t);
      root.append(g);
    }

    // series
    const labels = [];
    for (const s of opts.series) {
      if (!s.points.length) continue;
      const d = s.points.map((p, i) => (i ? "L" : "M") + X(p[0]).toFixed(1) + "," + Y(p[1]).toFixed(1)).join("");
      root.append(svg("path", { class: "line", d, stroke: s.color, "stroke-dasharray": s.dash || "" }));
      const last = s.points[s.points.length - 1];
      labels.push({ y: Y(last[1]), x: X(last[0]), s });
    }
    // direct end labels, de-collided
    labels.sort((a, b) => a.y - b.y);
    for (let i = 1; i < labels.length; i++) if (labels[i].y - labels[i - 1].y < 14) labels[i].y = labels[i - 1].y + 14;
    if (opts.series.length <= 4)
      for (const l of labels) {
        const t = svg("text", { class: "label", x: l.x + 8, y: l.y + 4 });
        t.textContent = l.s.short || l.s.name;
        root.append(t);
      }

    // hover layer
    const cross = svg("line", { class: "cross", y1: M.t, y2: M.t + ph, visibility: "hidden" });
    const dots = opts.series.map((s) => {
      const c = svg("circle", { r: 4.5, fill: s.color, stroke: "var(--surface)", "stroke-width": 2, visibility: "hidden" });
      root.append(c);
      return c;
    });
    root.append(cross);
    const hit = svg("rect", { x: M.l, y: M.t, width: pw, height: ph, fill: "transparent" });
    root.append(hit);
    host.append(root);

    const tip = UI.el("div", { class: "tooltip" });
    host.append(tip);
    function nearest(pts, x) {
      let best = null, bd = Infinity;
      for (const p of pts) {
        const d = Math.abs(lx(p[0]) - lx(x));
        if (d < bd) { bd = d; best = p; }
      }
      return best;
    }
    function move(ev) {
      const r = root.getBoundingClientRect();
      const px = ((ev.clientX - r.left) / r.width) * W;
      const xv = opts.xLog ? 10 ** (lx(xMin) + ((px - M.l) / pw) * (lx(xMax) - lx(xMin))) : xMin + ((px - M.l) / pw) * (xMax - xMin);
      const ref = nearest(opts.series[0].points, xv) || nearest(all, xv);
      const xx = X(ref[0]);
      cross.setAttribute("x1", xx);
      cross.setAttribute("x2", xx);
      cross.setAttribute("visibility", "visible");
      let html = `<div style="font-weight:600;margin-bottom:4px">${opts.xTip ? opts.xTip(ref[0]) : UI.fmt(ref[0]) + " Partien"}</div>`;
      opts.series.forEach((s, i) => {
        const p = nearest(s.points, ref[0]);
        if (!p) return;
        dots[i].setAttribute("cx", X(p[0]));
        dots[i].setAttribute("cy", Y(p[1]));
        dots[i].setAttribute("visibility", "visible");
        html += `<div><span class="sw" style="background:${s.color}"></span>${s.name}: <b>${yf(p[1])}</b></div>`;
      });
      tip.innerHTML = html;
      tip.style.opacity = 1;
      const hostR = host.getBoundingClientRect();
      const tx = (xx / W) * r.width + (r.left - hostR.left);
      const tw = tip.offsetWidth;
      tip.style.left = (tx + 14 + tw > hostR.width ? tx - tw - 14 : tx + 14) + "px";
      tip.style.top = ((ev.clientY - hostR.top) - 20) + "px";
    }
    function leave() {
      tip.style.opacity = 0;
      cross.setAttribute("visibility", "hidden");
      dots.forEach((d) => d.setAttribute("visibility", "hidden"));
    }
    hit.addEventListener("pointermove", move);
    hit.addEventListener("pointerleave", leave);

    // legend (always for >= 2 series)
    if (opts.series.length >= 2) {
      const lg = UI.el("div", { class: "legend" });
      for (const s of opts.series) lg.append(UI.el("span", {}, UI.el("span", { class: "sw", style: `background:${s.color}` }), s.name));
      host.prepend(lg);
    }

    // table view
    if (opts.table !== false) {
      const det = UI.el("details", { class: "table-view" }, UI.el("summary", { text: "Daten als Tabelle" }));
      const xsU = [...new Set(opts.series.flatMap((s) => s.points.map((p) => p[0])))].sort((a, b) => a - b);
      const tbl = UI.el("table", { class: "data" });
      const head = UI.el("tr", {}, UI.el("th", { text: opts.xLabel || "x" }), ...opts.series.map((s) => UI.el("th", { text: s.name })));
      tbl.append(head);
      const maps = opts.series.map((s) => new Map(s.points.map((p) => [p[0], p[1]])));
      for (const x of xsU) tbl.append(UI.el("tr", {}, UI.el("td", { text: UI.fmt(x) }), ...maps.map((m) => UI.el("td", { text: m.has(x) ? yf(m.get(x)) : "" }))));
      det.append(UI.el("div", { class: "tbl-scroll" }, tbl));
      host.append(det);
    }
  }

  return { line };
})();
