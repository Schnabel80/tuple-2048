/*@ ## td.h – Spielen und Lernen

Hier treffen Spiel-Engine (`board.h`) und Bewertung (`ntuple.h`) aufeinander:

- `td_best_move` – **Entscheiden:** welcher Zug ist der beste?
- `td_train_game` – **Lernen:** eine komplette Partie spielen und danach aus
  ihr lernen (TD-Learning).
- `td_play_game` – **Vorführen:** eine Partie mit eingefrorenem Wissen spielen
  und jeden Zug protokollieren (für den Replay-Viewer). */
#ifndef T2048_TD_H
#define T2048_TD_H

#include "board.h"
#include "ntuple.h"

/* One recorded step of a training game: the afterstate and the reward that led to it. */
typedef struct {
    board_t after; //@ Das Feld nach dem Verschieben, vor der neuen Kachel.
    uint32_t reward;
} step_t;

/* Per-thread growable path buffer, reused across games. */
typedef struct {
    step_t *steps; //@ Wächst bei Bedarf (realloc) und wird über alle Partien eines Threads wiederverwendet.
    size_t len, cap;
} path_t;

typedef struct {
    uint64_t score;
    uint32_t moves;
    int max_exp;
} game_result_t;

/* Everything the replay writer needs to know about one move. */
typedef struct {
    uint32_t t;
    board_t before, after, next;
    int dir;
    uint32_t reward;
    uint64_t score;
    int spawn_pos, spawn_exp;
    float q[N_DIRS];
    int valid[N_DIRS];
} move_info_t;

typedef void (*move_cb)(void *ctx, const move_info_t *mi); //@ Ein Funktionszeiger: Wer eine Partie aufzeichnen will, übergibt hier seine Schreibfunktion.

int td_best_move(const net_t *n, board_t b, board_t *after, uint32_t *reward, float q[N_DIRS], int valid[N_DIRS]);
game_result_t td_train_game(net_t *n, rng_t *r, path_t *path);
/* Expectimax search (search.c); depth 0 is plain greedy. */
int search_best_move(const net_t *n, board_t b, int depth, board_t *after, uint32_t *reward, float q[N_DIRS], int valid[N_DIRS]);
game_result_t td_play_game(const net_t *n, rng_t *r, int depth, move_cb cb, void *ctx);

#endif
