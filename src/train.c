/*@ ## train.c – der Trainingslauf

Der Ablauf eines Trainings:

1. Netz anlegen (oder gespeichertes laden).
2. Einen **Fahrplan** aus Meilensteinen erstellen: regelmäßig (alle N Partien)
   für eine glatte Lernkurve, dazu **Snapshots** bei 1, 2, 5, 10, 20, 50, 100, …
   Partien – logarithmisch, weil am Anfang viel passiert und später wenig.
3. Zwischen zwei Meilensteinen spielen **mehrere Threads gleichzeitig** und
   lernen in **dasselbe** Netz – ohne Sperren (*Hogwild!*). Das klingt
   gefährlich, ist aber harmlos: Jeder Zug fasst nur wenige der Millionen
   Gewichte an, Kollisionen sind selten und wirken wie ein bisschen Rauschen.
4. An jedem Meilenstein halten alle Threads an. Jetzt ist das Wissen
   **eingefroren**: Statistik schreiben, an Snapshots zusätzlich gelernte Muster
   exportieren, eine Vorführpartie aufzeichnen und die Gewichte sichern.

Während des Spielens wird **nichts** ausgegeben – I/O würde das Training nur
ausbremsen. */
#include "train.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "io.h"
#include "td.h"

#ifndef GIT_REV
#define GIT_REV "unknown"
#endif

void train_cfg_defaults(train_cfg_t *c) {
    memset(c, 0, sizeof *c);
    c->net = "strong";
    c->out = "runs/default";
    c->games = 100000;
    c->threads = 1;
    c->alpha = 0.1f;
    c->tc_alpha = 1.0f;
    c->n_stages = 1;
    c->every = 1000;
    c->seed = 1;
    c->demo_seed = 2048;
    c->top_k = 12;
    c->min_visits = 1000;
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}

static int mkdir_p(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, 0755) && errno != EEXIST) return -1;
            *p = '/';
        }
    return mkdir(tmp, 0755) && errno != EEXIST ? -1 : 0;
}

/*@ ### Ein Arbeiter-Thread

Jeder Thread holt sich mit einem **atomaren Zähler** die nächste Partienummer
(`atomic_fetch_add`) – so spielt kein Spiel doppelt, und kein Thread wartet auf
einen anderen. Seine Statistik sammelt er lokal und gibt sie erst am Ende des
Abschnitts ab. Jeder Thread hat seinen eigenen Zufallsgenerator und seinen
eigenen Partie-Speicher. */
typedef struct {
    net_t *net;
    _Atomic uint64_t *next;
    uint64_t end;
    int eval; //@ 1 = nur spielen (eingefroren), 0 = spielen und lernen.
    rng_t rng;
    path_t path;
    stats_t stats;
    pthread_t tid;
} worker_t;

static void *worker_main(void *arg) {
    worker_t *w = arg;
    for (;;) {
        uint64_t g = atomic_fetch_add(w->next, 1); //@ Nächste freie Partie „ziehen“.
        if (g >= w->end) break;
        game_result_t r = w->eval ? td_play_game(w->net, &w->rng, 0, NULL, NULL) : td_train_game(w->net, &w->rng, &w->path); //@ Bewertungsmodus: nur spielen. Trainingsmodus: spielen und lernen.
        stats_add(&w->stats, r);
    }
    return NULL;
}

/*@ ### Einen Abschnitt parallel abarbeiten

Startet alle Threads für die Partien `[start, end)`, wartet auf alle (`join`)
und fasst ihre Statistiken zusammen. Danach ist garantiert kein Thread mehr am
Lernen – der perfekte Moment für einen Meilenstein. */
static void run_segment(worker_t *ws, int nt, uint64_t start, uint64_t end, int eval, stats_t *out) {
    _Atomic uint64_t next = start; //@ Der gemeinsame Zähler aller Threads für diesen Abschnitt.
    memset(out, 0, sizeof *out);
    for (int i = 0; i < nt; i++) {
        ws[i].next = &next;
        ws[i].end = end;
        ws[i].eval = eval;
        memset(&ws[i].stats, 0, sizeof ws[i].stats);
    }
    if (nt == 1) {
        worker_main(&ws[0]); //@ Ein Thread: direkt aufrufen – dann ist das Training exakt reproduzierbar.
    } else {
        for (int i = 0; i < nt; i++) pthread_create(&ws[i].tid, NULL, worker_main, &ws[i]); //@ Alle Threads starten – sie lernen ab jetzt gleichzeitig in dasselbe Netz.
        for (int i = 0; i < nt; i++) pthread_join(ws[i].tid, NULL); //@ Warten, bis alle fertig sind. Danach ist das Netz „eingefroren“.
    }
    for (int i = 0; i < nt; i++) stats_merge(out, &ws[i].stats);
}

/*@ ### Der Fahrplan

Alle Meilensteine als sortierte Liste ohne Doppelte. Ein Eintrag ist ein
**Snapshot**, wenn er in der Reihe 1-2-5-10-20-50-… liegt (oder das Ende bzw.
der Start von TC ist). */
typedef struct {
    uint64_t games;
    int snap;
} mark_t;

static int cmp_mark(const void *a, const void *b) {
    uint64_t x = ((const mark_t *)a)->games, y = ((const mark_t *)b)->games;
    return (x > y) - (x < y);
}

static mark_t *build_schedule(const train_cfg_t *c, uint64_t start, size_t *count) {
    size_t cap = 64 + (size_t)(c->games / (c->every ? c->every : c->games)), n = 0;
    mark_t *m = malloc(cap * sizeof *m);
    if (!m) abort();
    for (uint64_t g = c->every; c->every && g <= c->games; g += c->every) //@ Regelmäßige Messpunkte für die glatte Lernkurve.
        if (g > start) m[n++] = (mark_t){g, 0};
    for (uint64_t p = 1; p <= c->games; p *= 10) { //@ Logarithmische Snapshots: 1, 2, 5, 10, 20, 50, …
        const uint64_t f[3] = {1, 2, 5};
        for (int i = 0; i < 3; i++)
            if (p * f[i] <= c->games && p * f[i] > start) m[n++] = (mark_t){p * f[i], 1};
    }
    m[n++] = (mark_t){c->games, 1}; //@ Das Ende des Laufs ist immer ein Snapshot.
    if (c->tc && c->tc_after > start && c->tc_after < c->games) m[n++] = (mark_t){c->tc_after, 1};
    qsort(m, n, sizeof *m, cmp_mark);
    size_t k = 0;
    for (size_t i = 0; i < n; i++) { //@ Doppelte zusammenführen – Snapshot gewinnt.
        if (k && m[k - 1].games == m[i].games)
            m[k - 1].snap |= m[i].snap;
        else
            m[k++] = m[i];
    }
    *count = k;
    return m;
}

static void write_meta(const train_cfg_t *c, const net_t *n) {
    char path[1200];
    snprintf(path, sizeof path, "%s/meta.json", c->out);
    FILE *f = fopen(path, "w");
    if (!f) return;
    time_t t = time(NULL);
    char ts[32];
    strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", gmtime(&t));
    fprintf(f, "{\"format_version\":%d,\"net\":\"%s\",\"tuples\":[", FORMAT_VERSION, n->name);
    for (int i = 0; i < n->n_tuples; i++) {
        fprintf(f, "%s[", i ? "," : "");
        for (int j = 0; j < n->len[i]; j++) fprintf(f, "%s%u", j ? "," : "", n->cells[i][j]);
        fprintf(f, "]");
    }
    fprintf(f, "],\"stages\":[");
    for (int s = 0; s < n->n_stages; s++) fprintf(f, "%s%d", s ? "," : "", n->stage_exp[s]);
    fprintf(f,
            "],\"alpha\":%g,\"init\":%g,\"tc\":%d,\"tc_after\":%llu,\"tc_alpha\":%g,\"threads\":%d,\"seed\":%llu,"
            "\"demo_seed\":%llu,\"demo_depth\":%d,\"games\":%llu,\"resumed_from\":%llu,\"started\":\"%s\",\"git_rev\":\"%s\"}\n",
            c->alpha, c->init, c->tc, (unsigned long long)c->tc_after, c->tc_alpha, c->threads, (unsigned long long)c->seed,
            (unsigned long long)c->demo_seed, c->demo_depth, (unsigned long long)c->games, (unsigned long long)n->games_trained, ts,
            GIT_REV);
    fclose(f);
}

/* On resume: continue the elapsed-time axis where the previous run stopped. */
static double previous_elapsed(const char *csv) {
    FILE *f = fopen(csv, "r");
    if (!f) return 0.0;
    char line[512], last[512] = "";
    while (fgets(line, sizeof line, f))
        if (line[0] >= '0' && line[0] <= '9') memcpy(last, line, sizeof last);
    fclose(f);
    char *comma = strchr(last, ',');
    return comma ? strtod(comma + 1, NULL) : 0.0;
}

/*@ ### Ein Snapshot

Mit eingefrorenem Netz:
1. die gelernten Top-Muster exportieren,
2. eine **Vorführpartie** mit immer demselben Seed spielen und aufzeichnen –
   so sieht man auf der Webseite, wie *dieselbe* Ausgangslage nach 10 und nach
   100 000 Trainingspartien gespielt wird,
3. optional zusätzliche Bewertungspartien spielen (saubere Statistik ohne
   Lernen),
4. die Gewichte sichern (für Resume und `demo`). */
static void snapshot(const train_cfg_t *c, net_t *n, worker_t *ws, FILE *eval_csv, double elapsed) {
    char path[1200];
    unsigned long long g = (unsigned long long)n->games_trained;
    snprintf(path, sizeof path, "%s/top/top_%llu.json", c->out, g);
    io_write_top(n, 0, c->top_k, c->min_visits, path);

    snprintf(path, sizeof path, "%s/replays/replay_%llu.jsonl", c->out, g);
    FILE *f = fopen(path, "w");
    if (f) {
        rng_t r;
        rng_seed(&r, c->demo_seed);
        io_replay_t ctx = {f};
        io_replay_header(f, n, c->demo_seed, c->demo_depth); //@ Immer derselbe Demo-Seed: vergleichbare Partien über alle Meilensteine.
        game_result_t res = td_play_game(n, &r, c->demo_depth, io_replay_cb, &ctx);
        io_replay_footer(f, res);
        fclose(f);
    }

    if (c->eval_games && eval_csv) {
        stats_t es;
        rng_t *saved = malloc((size_t)c->threads * sizeof *saved);
        if (!saved) abort();
        for (int i = 0; i < c->threads; i++) {
            saved[i] = ws[i].rng; //@ Trainings-Zufall beiseitelegen, damit die Bewertung das Training nicht verändert.
            rng_seed(&ws[i].rng, c->seed * 7919 + n->games_trained * 31 + (uint64_t)i);
        }
        run_segment(ws, c->threads, 0, c->eval_games, 1, &es);
        for (int i = 0; i < c->threads; i++) ws[i].rng = saved[i];
        free(saved);
        io_csv_row(eval_csv, n->games_trained, elapsed, 0.0, &es, es.max_exp);
    }

    snprintf(path, sizeof path, "%s/weights.bin", c->out);
    net_save(n, path);
    if (c->snapshot_weights) {
        snprintf(path, sizeof path, "%s/weights/weights_%llu.bin", c->out, g);
        net_save(n, path);
    }
}

/*@ ### Das Training

Die Hauptschleife geht den Fahrplan Meilenstein für Meilenstein durch:
Abschnitt parallel spielen → Statistikzeile → ggf. Snapshot. Erreicht der Lauf
`tc_after`, wird TC-Learning zugeschaltet: Ab dann bestimmt jedes Gewicht seine
Lernrate selbst. */
int train_run(const train_cfg_t *c) {
    net_t *n = calloc(1, sizeof *n);
    if (!n) return 1;
    int rc = c->resume ? net_load(n, c->resume) : net_create(n, c->net, c->n_stages, c->stage_exp, c->init); //@ Weitertrainieren oder bei null anfangen.
    if (rc) {
        fprintf(stderr, "error: cannot %s network (%d)\n", c->resume ? "load" : "create", rc);
        return 1;
    }
    n->alpha = c->alpha;
    if (c->tc && n->games_trained >= c->tc_after) {
        if (net_enable_tc(n)) return 1;
        n->alpha = c->tc_alpha;
    }

    char path[1200];
    if (mkdir_p(c->out)) {
        fprintf(stderr, "error: cannot create %s\n", c->out);
        return 1;
    }
    snprintf(path, sizeof path, "%s/top", c->out);
    mkdir_p(path);
    snprintf(path, sizeof path, "%s/replays", c->out);
    mkdir_p(path);
    if (c->snapshot_weights) {
        snprintf(path, sizeof path, "%s/weights", c->out);
        mkdir_p(path);
    }
    write_meta(c, n);

    snprintf(path, sizeof path, "%s/milestones.csv", c->out);
    double elapsed0 = c->resume ? previous_elapsed(path) : 0.0;
    FILE *csv = fopen(path, c->resume ? "a" : "w");
    if (!csv) return 1;
    if (!c->resume) io_csv_header(csv);
    FILE *eval_csv = NULL;
    if (c->eval_games) {
        snprintf(path, sizeof path, "%s/eval.csv", c->out);
        eval_csv = fopen(path, c->resume ? "a" : "w");
        if (eval_csv && !c->resume) io_csv_header(eval_csv);
    }

    worker_t *ws = calloc((size_t)c->threads, sizeof *ws);
    if (!ws) return 1;
    for (int i = 0; i < c->threads; i++) {
        ws[i].net = n;
        rng_seed(&ws[i].rng, c->seed + 0x1000003ULL * (uint64_t)i + n->games_trained); //@ Jeder Thread: eigener, reproduzierbarer Zufall.
    }

    size_t nm;
    mark_t *marks = build_schedule(c, n->games_trained, &nm); //@ Fahrplan ab dem aktuellen Stand (bei Resume nicht bei 0).
    fprintf(stderr, "net=%s tuples=%d stages=%d memory=%.0f MB threads=%d games=%llu..%llu\n", n->name, n->n_tuples, n->n_stages,
            (double)net_bytes(n) / 1e6, c->threads, (unsigned long long)n->games_trained, (unsigned long long)c->games);

    int best_ever = 0;
    double t0 = now_s(), t_last = t0;
    for (size_t i = 0; i < nm; i++) {
        stats_t st;
        if (c->tc && !n->tc && n->games_trained >= c->tc_after) { //@ OTD → OTD+TC: ab jetzt passt jedes Gewicht seine Lernrate selbst an.
            if (net_enable_tc(n)) return 1;
            n->alpha = c->tc_alpha;
            fprintf(stderr, "TC learning enabled at %llu games\n", (unsigned long long)n->games_trained);
        }
        run_segment(ws, c->threads, n->games_trained, marks[i].games, 0, &st); //@ Bis zum nächsten Meilenstein parallel spielen und lernen.
        n->games_trained = marks[i].games;
        double t = now_s();
        if (st.max_exp > best_ever) best_ever = st.max_exp;
        io_csv_row(csv, n->games_trained, elapsed0 + t - t0, (double)st.games / (t - t_last + 1e-9), &st, best_ever); //@ Eine Zeile Lernkurve.
        if (marks[i].snap) {
            snapshot(c, n, ws, eval_csv, elapsed0 + t - t0);
            fprintf(stderr, "[%7.0fs] games=%-9llu avg=%-9.0f max=%-8llu 2048=%5.1f%% 8192=%5.1f%% 16384=%5.1f%% %.0f games/s\n", elapsed0 + t - t0,
                    (unsigned long long)n->games_trained, (double)st.sum_score / (double)(st.games ? st.games : 1),
                    (unsigned long long)st.max_score, 100.0 * (double)st.reach[11] / (double)(st.games ? st.games : 1),
                    100.0 * (double)st.reach[13] / (double)(st.games ? st.games : 1),
                    100.0 * (double)st.reach[14] / (double)(st.games ? st.games : 1), (double)st.games / (t - t_last + 1e-9));
        }
        t_last = now_s(); //@ Snapshot-Zeit nicht in die Spiele-pro-Sekunde einrechnen.
    }

    for (int i = 0; i < c->threads; i++) free(ws[i].path.steps);
    free(ws);
    free(marks);
    fclose(csv);
    if (eval_csv) fclose(eval_csv);
    net_free(n);
    free(n);
    return 0;
}
