/* Code walkthrough: renders window.T2048_CODE (generated from the annotated C sources). */
"use strict";

(() => {
  UI.initTheme();
  const { el } = UI;
  const data = window.T2048_CODE;
  if (!data) return;

  /* A deliberately small C highlighter: comments, strings, preprocessor, keywords, types, numbers. */
  const KW = new Set("if else for while do switch case default break continue return goto sizeof static inline const volatile extern typedef struct enum union restrict _Atomic".split(" "));
  const TYPES = new Set("void char short int long float double unsigned signed size_t uint8_t uint16_t uint32_t uint64_t int8_t int64_t board_t rng_t net_t step_t path_t game_result_t move_info_t move_cb stats_t io_replay_t train_cfg_t worker_t mark_t preset_t cache_entry_t heap_item_t eval_worker_t demo_ctx_t FILE pthread_t pthread_mutex_t useconds_t time_t".split(" "));
  const TOKEN = /(\/\/.*$|\/\*.*?\*\/|\/\*.*$|"(?:[^"\\]|\\.)*"|'(?:[^'\\]|\\.)*'|^\s*#\s*\w+|\b0x[0-9A-Fa-f]+(?:ULL|U|ULL)?\b|\b\d+(?:\.\d+)?(?:e-?\d+)?[fFuUlL]*\b|\b[A-Za-z_]\w*\b)/g;
  const esc = (s) => s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
  let inBlock = false;
  function highlight(line) {
    if (inBlock) {
      const end = line.indexOf("*/");
      if (end < 0) return `<span class="tok-com">${esc(line)}</span>`;
      inBlock = false;
      return `<span class="tok-com">${esc(line.slice(0, end + 2))}</span>` + highlight(line.slice(end + 2));
    }
    let out = "", last = 0;
    line.replace(TOKEN, (m, _g, off) => {
      out += esc(line.slice(last, off));
      last = off + m.length;
      let cls = null;
      if (m.startsWith("//") || m.startsWith("/*")) { cls = "tok-com"; if (m.startsWith("/*") && !m.endsWith("*/")) inBlock = true; }
      else if (m[0] === '"' || m[0] === "'") cls = "tok-str";
      else if (/^\s*#/.test(m)) cls = "tok-pre";
      else if (/^\d|^0x/.test(m)) cls = "tok-num";
      else if (KW.has(m)) cls = "tok-kw";
      else if (TYPES.has(m)) cls = "tok-type";
      out += cls ? `<span class="${cls}">${esc(m)}</span>` : esc(m);
      return m;
    });
    return out + esc(line.slice(last));
  }

  const nav = document.getElementById("code-nav");
  const host = document.getElementById("modules");
  let group = null;
  const totalNotes = data.modules.reduce((s, m) => s + m.notes, 0);

  const ctrl = el("div", { class: "controls" },
    el("button", { class: "btn notes-toggle", type: "button", "aria-pressed": "false", text: `Alle ${totalNotes} Zeilen-Anmerkungen aufklappen` }));
  host.append(ctrl);
  const toggleAll = ctrl.firstChild;

  for (const mod of data.modules) {
    if (mod.group !== group) {
      group = mod.group;
      nav.append(el("div", { class: "grp", text: group }));
    }
    const id = "m-" + mod.file.replace(".", "-");
    nav.append(el("a", { href: "#" + id, text: mod.file }));
    const section = el("section", { class: "module", id });
    section.append(el("div", { class: "module-head" },
      el("h2", { text: mod.title || mod.file, style: "margin:0" }),
      el("span", { class: "file", text: `src/${mod.file} · ${mod.lines} Zeilen · ${mod.notes} Anmerkungen` })));
    inBlock = false;
    for (const sec of mod.sections) {
      const doc = el("div", { class: "sec-doc", html: sec.html || "<p class='muted small'>(ohne eigene Erklärung)</p>" });
      const code = el("div", { class: "sec-code" });
      for (const ln of sec.lines) {
        const row = el("div", { class: "ln" + (ln.note ? " has-note" : "") }, el("span", { class: "no", text: ln.n }), el("span", { html: highlight(ln.code) || " " }));
        code.append(row);
        if (ln.note) {
          const note = el("div", { class: "ln-note", text: ln.note, hidden: "" });
          row.title = "Anmerkung anzeigen";
          row.addEventListener("click", () => { note.hidden = !note.hidden; row.classList.toggle("open", !note.hidden); });
          code.append(note);
        }
      }
      section.append(el("div", { class: "sec" }, doc, sec.lines.length ? code : el("div", { class: "sec-code" }, el("div", { class: "ln" }, el("span", { class: "no" }), el("span", { class: "muted", text: "(nur Erklärung)" })))));
    }
    host.append(section);
  }

  toggleAll.addEventListener("click", () => {
    const open = toggleAll.getAttribute("aria-pressed") !== "true";
    toggleAll.setAttribute("aria-pressed", String(open));
    toggleAll.textContent = open ? "Anmerkungen wieder einklappen" : `Alle ${totalNotes} Zeilen-Anmerkungen aufklappen`;
    document.querySelectorAll(".ln-note").forEach((n) => (n.hidden = !open));
    document.querySelectorAll(".ln.has-note").forEach((r) => r.classList.toggle("open", open));
  });

  // highlight the module currently in view
  const links = [...nav.querySelectorAll("a")];
  const obs = new IntersectionObserver((entries) => {
    for (const e of entries) if (e.isIntersecting) links.forEach((a) => a.classList.toggle("active", a.getAttribute("href") === "#" + e.target.id));
  }, { rootMargin: "-20% 0px -70% 0px" });
  document.querySelectorAll(".module").forEach((m) => obs.observe(m));
})();
