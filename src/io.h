/*@ ## io.h – was das Programm nach außen schreibt

Das C-Programm hat keine Grafik. Alles, was die Webseite zeigt, kommt aus
kleinen Textdateien, die hier erzeugt werden:

- **Statistik** (`milestones.csv`) – eine Zeile pro Meilenstein,
- **gelernte Muster** (`top/top_<N>.json`) – die wertvollsten und die am
  häufigsten gesehenen Tupel-Belegungen,
- **Replays** (`replays/replay_<N>.jsonl`) – eine Vorführpartie, ein Zug pro
  Zeile.

Spielfelder werden überall als **16 Hex-Ziffern** geschrieben: Ziffer i ist der
Exponent von Zelle i (`0` = leer, `b` = 2048, `f` = 32768). */
#ifndef T2048_IO_H
#define T2048_IO_H

#include <stdio.h>

#include "ntuple.h"
#include "td.h"

#define FORMAT_VERSION 1

/* Aggregated statistics over a set of games. */
typedef struct {
    uint64_t games, sum_score, max_score, sum_moves;
    int max_exp;
    uint64_t reach[MAX_EXP + 2]; //@ reach[e] = Anzahl Partien, deren größte Kachel ≥ 2^e war.
} stats_t;

void stats_add(stats_t *s, game_result_t g);
void stats_merge(stats_t *into, const stats_t *from);

void io_board_hex(board_t b, char out[17]);
void io_csv_header(FILE *f);
void io_csv_row(FILE *f, uint64_t games, double elapsed, double gps, const stats_t *s, int best_ever);
int io_write_top(const net_t *n, int stage, int k, uint32_t min_visits, const char *path);

/* Replay writer: pass io_replay_cb + an io_replay_t as move callback. */
typedef struct {
    FILE *f;
} io_replay_t;
void io_replay_header(FILE *f, const net_t *n, uint64_t seed, int depth);
void io_replay_cb(void *ctx, const move_info_t *mi);
void io_replay_footer(FILE *f, game_result_t g);

#endif
