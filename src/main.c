/*@ ## main.c – die Kommandozeile

Ein Programm, fünf Befehle:

| Befehl | Was es tut |
|---|---|
| `t2048 train` | Trainiert ein Netz und schreibt Lernkurve, Muster, Replays, Gewichte |
| `t2048 demo`  | Spielt eine Partie mit gespeicherten Gewichten und schreibt ein Replay |
| `t2048 eval`  | Spielt viele Partien eingefroren (optional mit Vorausschau) und misst die Stärke |
| `t2048 top`   | Exportiert die gelernten Top-Muster aus einer Gewichtsdatei |
| `t2048 bench` | Misst die Geschwindigkeit von Engine und Bewertung |

Die Argumente werden von Hand geparst – ohne Bibliothek, damit das Programm
überall mit einem nackten C-Compiler baut. */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "board.h"
#include "io.h"
#include "ntuple.h"
#include "td.h"
#include "train.h"

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}

static void usage(void) {
    fprintf(stderr,
            "usage:\n"
            "  t2048 train [--net small|strong|strong8] [--games N] [--threads T] [--alpha A] [--init V]\n"
            "              [--tc] [--tc-after G] [--tc-alpha A] [--stages E1,E2,..] [--every N]\n"
            "              [--seed S] [--demo-seed S] [--demo-depth D] [--eval N] [--top-k K]\n"
            "              [--min-visits V] [--snapshot-weights] [--resume weights.bin] --out DIR\n"
            "  t2048 demo  --weights FILE [--seed S] [--depth D] [--out FILE.jsonl] [--delay MS]\n"
            "  t2048 eval  --weights FILE [--games N] [--depth D] [--threads T] [--seed S]\n"
            "  t2048 top   --weights FILE [--k K] [--min-visits V] [--stage S] --out FILE.json\n"
            "  t2048 bench [--seconds S]\n");
}

/* Tiny option parser: --name value pairs and bare flags. */
static const char *opt(int argc, char **argv, const char *name) {
    for (int i = 2; i < argc - 1; i++)
        if (strcmp(argv[i], name) == 0) return argv[i + 1];
    return NULL;
}
static int flag(int argc, char **argv, const char *name) {
    for (int i = 2; i < argc; i++)
        if (strcmp(argv[i], name) == 0) return 1;
    return 0;
}
static uint64_t opt_u64(int argc, char **argv, const char *name, uint64_t def) {
    const char *v = opt(argc, argv, name);
    return v ? strtoull(v, NULL, 10) : def;
}
static double opt_f(int argc, char **argv, const char *name, double def) {
    const char *v = opt(argc, argv, name);
    return v ? strtod(v, NULL) : def;
}

static int load_or_die(net_t *n, const char *path) {
    if (!path) {
        fprintf(stderr, "error: --weights required\n");
        return 1;
    }
    int rc = net_load(n, path);
    if (rc) fprintf(stderr, "error: cannot load %s (%d)\n", path, rc);
    return rc;
}

/*@ ### train

Übersetzt die Optionen in eine `train_cfg_t` und startet `train_run`.
`--stages 13,14` bedeutet: eigene Tabellen ab der ersten 8192 (2¹³) und ab der
ersten 16384 (2¹⁴). */
static int cmd_train(int argc, char **argv) {
    train_cfg_t c;
    train_cfg_defaults(&c);
    if (opt(argc, argv, "--net")) c.net = opt(argc, argv, "--net");
    if (opt(argc, argv, "--out")) c.out = opt(argc, argv, "--out");
    c.resume = opt(argc, argv, "--resume");
    c.games = opt_u64(argc, argv, "--games", c.games);
    c.threads = (int)opt_u64(argc, argv, "--threads", 1);
    if (c.threads < 1) c.threads = 1;
    c.alpha = (float)opt_f(argc, argv, "--alpha", c.alpha);
    c.init = (float)opt_f(argc, argv, "--init", 0.0);
    c.tc = flag(argc, argv, "--tc") || opt(argc, argv, "--tc-after"); //@ --tc-after schaltet TC automatisch mit ein.
    c.tc_after = opt_u64(argc, argv, "--tc-after", 0);
    c.tc_alpha = (float)opt_f(argc, argv, "--tc-alpha", c.tc_alpha);
    c.every = opt_u64(argc, argv, "--every", c.every);
    c.seed = opt_u64(argc, argv, "--seed", c.seed);
    c.demo_seed = opt_u64(argc, argv, "--demo-seed", c.demo_seed);
    c.demo_depth = (int)opt_u64(argc, argv, "--demo-depth", 0);
    c.eval_games = opt_u64(argc, argv, "--eval", 0);
    c.top_k = (int)opt_u64(argc, argv, "--top-k", (uint64_t)c.top_k);
    c.min_visits = (uint32_t)opt_u64(argc, argv, "--min-visits", c.min_visits);
    c.snapshot_weights = flag(argc, argv, "--snapshot-weights");
    const char *st = opt(argc, argv, "--stages");
    if (st) {
        char buf[64];
        snprintf(buf, sizeof buf, "%s", st);
        for (char *tok = strtok(buf, ","); tok && c.n_stages < MAX_STAGES; tok = strtok(NULL, ","))
            c.stage_exp[c.n_stages++] = atoi(tok); //@ Stufe 0 ist implizit, jede Angabe fügt eine Stufe hinzu.
    }
    return train_run(&c);
}

/*@ ### demo

Eine einzelne Vorführpartie. Mit `--delay` wird nach jedem Zug kurz gewartet und
die Zeile sofort ausgegeben – so kann man der Partie im Terminal (oder per Pipe
in einem anderen Programm) live zuschauen. */
typedef struct {
    io_replay_t rep;
    int delay_ms;
} demo_ctx_t;

static void demo_cb(void *ctx, const move_info_t *mi) {
    demo_ctx_t *d = ctx;
    io_replay_cb(&d->rep, mi);
    if (d->delay_ms) {
        fflush(d->rep.f);
        usleep((useconds_t)d->delay_ms * 1000); //@ Absichtlich langsam: zum Zuschauen.
    }
}

static int cmd_demo(int argc, char **argv) {
    net_t *n = calloc(1, sizeof *n);
    if (!n || load_or_die(n, opt(argc, argv, "--weights"))) return 1;
    uint64_t seed = opt_u64(argc, argv, "--seed", 2048);
    int depth = (int)opt_u64(argc, argv, "--depth", 0);
    const char *out = opt(argc, argv, "--out");
    FILE *f = out ? fopen(out, "w") : stdout;
    if (!f) return 1;
    demo_ctx_t ctx = {{f}, (int)opt_u64(argc, argv, "--delay", 0)};
    rng_t r;
    rng_seed(&r, seed);
    io_replay_header(f, n, seed, depth);
    game_result_t g = td_play_game(n, &r, depth, demo_cb, &ctx);
    io_replay_footer(f, g);
    if (out) fclose(f);
    fprintf(stderr, "score=%llu moves=%u max_tile=%u\n", (unsigned long long)g.score, g.moves, 1u << g.max_exp);
    net_free(n);
    free(n);
    return 0;
}

/*@ ### eval

Misst die echte Spielstärke: viele Partien mit eingefrorenem Netz, parallel auf
mehreren Threads, optional mit Expectimax-Vorausschau (`--depth`). Ausgabe ist
eine CSV-Zeile im selben Format wie die Lernkurve. */
typedef struct {
    const net_t *n;
    int depth;
    _Atomic uint64_t *next;
    uint64_t end;
    rng_t rng;
    stats_t st;
    pthread_t tid;
} eval_worker_t;

static void *eval_main(void *arg) {
    eval_worker_t *w = arg;
    for (;;) {
        uint64_t g = atomic_fetch_add(w->next, 1);
        if (g >= w->end) break;
        stats_add(&w->st, td_play_game(w->n, &w->rng, w->depth, NULL, NULL));
    }
    return NULL;
}

static int cmd_eval(int argc, char **argv) {
    net_t *n = calloc(1, sizeof *n);
    if (!n || load_or_die(n, opt(argc, argv, "--weights"))) return 1;
    uint64_t games = opt_u64(argc, argv, "--games", 1000), seed = opt_u64(argc, argv, "--seed", 99);
    int depth = (int)opt_u64(argc, argv, "--depth", 0), nt = (int)opt_u64(argc, argv, "--threads", 1);
    if (nt < 1) nt = 1;
    _Atomic uint64_t next = 0;
    eval_worker_t *ws = calloc((size_t)nt, sizeof *ws);
    if (!ws) return 1;
    double t0 = now_s();
    for (int i = 0; i < nt; i++) {
        ws[i] = (eval_worker_t){.n = n, .depth = depth, .next = &next, .end = games};
        rng_seed(&ws[i].rng, seed + 1000003ULL * (uint64_t)i); //@ Jeder Bewertungs-Thread bekommt einen anderen, aber reproduzierbaren Zufall.
        pthread_create(&ws[i].tid, NULL, eval_main, &ws[i]);
    }
    stats_t st = {0};
    for (int i = 0; i < nt; i++) {
        pthread_join(ws[i].tid, NULL);
        stats_merge(&st, &ws[i].st);
    }
    double t = now_s() - t0;
    io_csv_header(stdout);
    io_csv_row(stdout, n->games_trained, t, (double)games / t, &st, st.max_exp);
    free(ws);
    net_free(n);
    free(n);
    return 0;
}

static int cmd_top(int argc, char **argv) {
    net_t *n = calloc(1, sizeof *n);
    if (!n || load_or_die(n, opt(argc, argv, "--weights"))) return 1;
    const char *out = opt(argc, argv, "--out");
    if (!out) {
        usage();
        return 1;
    }
    int rc = io_write_top(n, (int)opt_u64(argc, argv, "--stage", 0), (int)opt_u64(argc, argv, "--k", 12),
                          (uint32_t)opt_u64(argc, argv, "--min-visits", 1000), out);
    net_free(n);
    free(n);
    return rc ? 1 : 0;
}

/*@ ### bench

Drei Messungen:
1. **Engine pur:** zufällige Züge, so schnell es geht – zeigt, was das Bitboard
   leistet.
2. **Bewertung:** wie viele `V(s)` pro Sekunde das starke Netz schafft – das ist
   der eigentliche Flaschenhals beim Lernen.
3. **Trainingspartien** mit dem kleinen Netz. */
static int cmd_bench(int argc, char **argv) {
    double secs = opt_f(argc, argv, "--seconds", 2.0);
    rng_t r;
    rng_seed(&r, 1);

    uint64_t moves = 0, games = 0;
    double t0 = now_s();
    while (now_s() - t0 < secs) {
        for (int k = 0; k < 1000; k++) {
            board_t b = board_new_game(&r);
            for (;;) {
                uint32_t rew;
                int d0 = (int)rng_below(&r, 4), moved = 0; //@ Zufällige Startrichtung, dann im Kreis die nächste probieren.
                for (int i = 0; i < 4 && !moved; i++) {
                    board_t a = board_move(b, (d0 + i) & 3, &rew);
                    if (a != b) {
                        b = board_spawn(a, &r, NULL, NULL);
                        moved = 1;
                        moves++;
                    }
                }
                if (!moved) break;
            }
            games++;
        }
    }
    double t = now_s() - t0;
    printf("engine (random play): %.1f M moves/s, %.0f k games/s\n", (double)moves / t / 1e6, (double)games / t / 1e3);

    net_t *n = calloc(1, sizeof *n);
    if (!n || net_create(n, "strong", 1, NULL, 0.0f)) return 1;
    uint64_t evals = 0;
    volatile float sink = 0;
    board_t b = board_new_game(&r);
    t0 = now_s();
    while (now_s() - t0 < secs) {
        for (int k = 0; k < 100000; k++) {
            sink += net_value(n, b);
            b = b * 0x9E3779B97F4A7C15ULL + 1; //@ Pseudo-zufällige Felder → realistische Cache-Misses.
        }
        evals += 100000;
    }
    t = now_s() - t0;
    printf("strong net: %.2f M evaluations/s (%.0f ns each)\n", (double)evals / t / 1e6, 1e9 * t / (double)evals);
    net_free(n);

    if (net_create(n, "small", 1, NULL, 0.0f)) return 1;
    path_t path = {0};
    games = moves = 0;
    t0 = now_s();
    while (now_s() - t0 < secs) {
        game_result_t g = td_train_game(n, &r, &path);
        games++;
        moves += g.moves;
    }
    t = now_s() - t0;
    printf("training (small net, 1 thread): %.0f games/s, %.2f M moves/s\n", (double)games / t, (double)moves / t / 1e6);
    free(path.steps);
    net_free(n);
    free(n);
    (void)sink;
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage();
        return 1;
    }
    board_init_tables(); //@ Einmal die Zug-Tabellen bauen – danach ist jeder Zug nur noch Nachschlagen.
    const char *cmd = argv[1];
    if (strcmp(cmd, "train") == 0) return cmd_train(argc, argv); //@ Der erste Parameter wählt den Befehl.
    if (strcmp(cmd, "demo") == 0) return cmd_demo(argc, argv);
    if (strcmp(cmd, "eval") == 0) return cmd_eval(argc, argv);
    if (strcmp(cmd, "top") == 0) return cmd_top(argc, argv);
    if (strcmp(cmd, "bench") == 0) return cmd_bench(argc, argv);
    usage();
    return 1;
}
