/* Unit tests for the engine, the n-tuple network and the learner.
 * No framework: CHECK() counts failures, main() returns non-zero on any failure. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/board.h"
#include "../src/io.h"
#include "../src/ntuple.h"
#include "../src/td.h"

static int failures = 0, checks = 0;
#define CHECK(cond, ...)                                              \
    do {                                                              \
        checks++;                                                     \
        if (!(cond)) {                                                \
            failures++;                                               \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);      \
            fprintf(stderr, __VA_ARGS__);                             \
            fprintf(stderr, "\n");                                    \
        }                                                             \
    } while (0)

/* ---- naive reference implementation on a plain 4x4 array ---- */

static void to_grid(board_t b, int g[4][4]) {
    for (int i = 0; i < 16; i++) g[i / 4][i % 4] = board_get(b, i);
}
static board_t from_grid(int g[4][4]) {
    board_t b = 0;
    for (int i = 0; i < 16; i++) b = board_set(b, i, g[i / 4][i % 4]);
    return b;
}

/* Slide one line of 4 exponents towards index 0. */
static uint32_t ref_line(int *v[4]) {
    int in[4], n = 0, out[4] = {0}, o = 0;
    uint32_t score = 0;
    for (int i = 0; i < 4; i++)
        if (*v[i]) in[n++] = *v[i];
    for (int i = 0; i < n; i++) {
        if (i + 1 < n && in[i] == in[i + 1] && in[i] < 15) {
            out[o++] = in[i] + 1;
            score += 1u << (in[i] + 1);
            i++;
        } else {
            out[o++] = in[i];
        }
    }
    for (int i = 0; i < 4; i++) *v[i] = out[i];
    return score;
}

static board_t ref_move(board_t b, int dir, uint32_t *score) {
    int g[4][4];
    to_grid(b, g);
    *score = 0;
    for (int k = 0; k < 4; k++) {
        int *v[4];
        for (int i = 0; i < 4; i++) {
            switch (dir) {
            case DIR_LEFT: v[i] = &g[k][i]; break;
            case DIR_RIGHT: v[i] = &g[k][3 - i]; break;
            case DIR_UP: v[i] = &g[i][k]; break;
            default: v[i] = &g[3 - i][k]; break;
            }
        }
        *score += ref_line(v);
    }
    return from_grid(g);
}

static board_t random_board(rng_t *r, int max_exp, int empty_pct) {
    board_t b = 0;
    for (int i = 0; i < 16; i++)
        if ((int)rng_below(r, 100) >= empty_pct) b = board_set(b, i, 1 + (int)rng_below(r, (uint32_t)max_exp));
    return b;
}

/* Apply symmetry s to a whole board (same convention as ntuple.c: mirror, then rotate). */
static board_t iso_board(board_t b, int s) {
    board_t out = 0;
    for (int cell = 0; cell < 16; cell++) {
        int r = cell / 4, c = cell % 4;
        if (s >= 4) c = 3 - c;
        for (int k = 0; k < (s & 3); k++) {
            int nr = c, nc = 3 - r;
            r = nr;
            c = nc;
        }
        out = board_set(out, 4 * r + c, board_get(b, cell));
    }
    return out;
}

/* ---- engine tests ---- */

static void test_moves_against_reference(void) {
    rng_t r;
    rng_seed(&r, 42);
    int bad = 0;
    for (int n = 0; n < 200000; n++) {
        board_t b = random_board(&r, n % 2 ? 15 : 5, (int)rng_below(&r, 80));
        for (int d = 0; d < 4; d++) {
            uint32_t s1, s2;
            board_t a = board_move(b, d, &s1), e = ref_move(b, d, &s2);
            if (a != e || s1 != s2) bad++;
        }
    }
    CHECK(bad == 0, "%d move mismatches vs. reference", bad);
}

static void test_known_rows(void) {
    /* 2 2 2 2 -> 4 4 _ _ (not 8): each tile merges at most once */
    board_t b = 0;
    for (int i = 0; i < 4; i++) b = board_set(b, i, 1);
    uint32_t s;
    board_t a = board_move(b, DIR_LEFT, &s);
    CHECK(board_get(a, 0) == 2 && board_get(a, 1) == 2 && board_get(a, 2) == 0 && s == 8, "2222 left");
    /* 4 _ 2 2 right -> _ _ 4 4 */
    b = board_set(board_set(board_set(0, 0, 2), 2, 1), 3, 1);
    a = board_move(b, DIR_RIGHT, &s);
    CHECK(board_get(a, 3) == 2 && board_get(a, 2) == 2 && board_get(a, 0) == 0 && s == 4, "4_22 right");
    /* two 32768 tiles do not merge */
    b = board_set(board_set(0, 0, 15), 1, 15);
    a = board_move(b, DIR_LEFT, &s);
    CHECK(a == b && s == 0, "32768 cap");
}

static void test_score_symmetric(void) {
    int bad = 0;
    for (uint32_t row = 0; row < 65536; row++) {
        uint32_t s;
        board_t b = row; /* row 0 only */
        board_move(b, DIR_RIGHT, &s);
        if (s != row_score_table[row]) bad++;
    }
    CHECK(bad == 0, "left/right score differs for %d rows", bad);
}

static void test_transpose(void) {
    rng_t r;
    rng_seed(&r, 7);
    int bad = 0;
    for (int n = 0; n < 10000; n++) {
        board_t b = rng_next(&r), t = board_transpose(b);
        if (board_transpose(t) != b) bad++;
        for (int i = 0; i < 16; i++)
            if (board_get(t, 4 * (i % 4) + i / 4) != board_get(b, i)) bad++;
    }
    CHECK(bad == 0, "transpose wrong (%d)", bad);
}

static void test_spawn(void) {
    rng_t r;
    rng_seed(&r, 3);
    int fours = 0, bad = 0, n = 200000;
    for (int k = 0; k < n; k++) {
        board_t b = random_board(&r, 10, 50);
        if (board_count_empty(b) == 0) continue;
        int pos, e;
        board_t s = board_spawn(b, &r, &pos, &e);
        if (board_get(b, pos) != 0 || board_get(s, pos) != e || (s ^ b) != ((board_t)e << (4 * pos))) bad++;
        if (e == 2) fours++;
    }
    double rate = (double)fours / n;
    CHECK(bad == 0, "spawn on occupied cell / wrong value (%d)", bad);
    CHECK(rate > 0.09 && rate < 0.11, "4-spawn rate %.4f not ~0.10", rate);

    /* uniform over empty cells: empty board, 160000 spawns, each cell ~10000 */
    int hist[16] = {0};
    for (int k = 0; k < 160000; k++) {
        int pos;
        board_spawn(0, &r, &pos, NULL);
        hist[pos]++;
    }
    int ok = 1;
    for (int i = 0; i < 16; i++) ok &= hist[i] > 9500 && hist[i] < 10500;
    CHECK(ok, "spawn position not uniform");
}

static void test_game_over(void) {
    /* checkerboard of 2/4: no move possible */
    board_t b = 0;
    for (int i = 0; i < 16; i++) b = board_set(b, i, ((i / 4 + i % 4) & 1) ? 1 : 2);
    CHECK(board_game_over(b), "checkerboard should be game over");
    CHECK(!board_game_over(board_set(b, 5, 0)), "empty cell -> not over");
    CHECK(!board_game_over(board_set(b, 0, 1) /* now equals neighbour 1 */), "merge possible -> not over");
    CHECK(board_count_empty(0) == 16 && board_count_empty(b) == 0, "empty count");
}

/* ---- network tests ---- */

static void randomize(net_t *n, rng_t *r) {
    for (int t = 0; t < n->n_tuples; t++)
        for (size_t i = 0; i < n->size[t]; i++) n->w[0][t][i] = (float)(rng_below(r, 2000)) - 1000.0f;
}

static void test_symmetry(void) {
    net_t n;
    CHECK(net_create(&n, "small", 1, NULL, 0.0f) == 0, "create small");
    rng_t r;
    rng_seed(&r, 11);
    randomize(&n, &r);
    int bad = 0;
    for (int k = 0; k < 2000; k++) {
        board_t b = random_board(&r, 12, 30);
        float v = net_value(&n, b);
        for (int s = 1; s < 8; s++)
            if (fabsf(net_value(&n, iso_board(b, s)) - v) > 1e-2f) bad++;
    }
    CHECK(bad == 0, "value not symmetric (%d)", bad);
    net_free(&n);
}

static void test_init_value(void) {
    net_t n;
    net_create(&n, "strong", 1, NULL, 1000.0f);
    rng_t r;
    rng_seed(&r, 5);
    CHECK(fabsf(net_value(&n, random_board(&r, 10, 40)) - 1000.0f) < 0.5f, "OTD init value");
    net_free(&n);
}

static void test_update(void) {
    net_t n;
    net_create(&n, "strong", 1, NULL, 0.0f);
    rng_t r;
    rng_seed(&r, 9);
    board_t b = 0;
    for (int i = 0; i < 16; i++) b = board_set(b, i, i + 1 > 15 ? 15 : i); /* asymmetric, distinct features */
    float v0 = net_value(&n, b);
    float v1 = net_update(&n, b, 100.0f);
    CHECK(fabsf(v1 - v0 - 10.0f) < 1e-3f, "update moves value by alpha*err: %f", v1 - v0);
    CHECK(fabsf(net_value(&n, b) - v1) < 1e-4f, "update returns new value");
    /* TC: +err then -err leaves E = 0, A > 0 -> coherence 0 -> the third update is frozen */
    CHECK(net_enable_tc(&n) == 0, "enable tc");
    n.alpha = 0.1f;
    float a = net_update(&n, b, 100.0f);
    float c = net_update(&n, b, -100.0f);
    float d = net_update(&n, b, 100.0f);
    CHECK(fabsf(a - v1 - 10.0f) < 1e-3f, "tc first step full");
    CHECK(fabsf(c - v1) < 1e-3f, "tc second step full (coherence still 1)");
    CHECK(fabsf(d - c) < 1e-3f, "tc oscillating weight frozen: %f", d - c);
    net_free(&n);
}

static void test_save_load(void) {
    net_t n, m;
    net_create(&n, "small", 2, (int[]){0, 13}, 0.0f);
    rng_t r;
    rng_seed(&r, 21);
    randomize(&n, &r);
    n.games_trained = 12345;
    n.visits[0][1][77] = 99;
    char path[] = "/tmp/t2048_test_weights_XXXXXX";
    int fd = mkstemp(path);
    close(fd);
    CHECK(net_save(&n, path) == 0, "save");
    CHECK(net_load(&m, path) == 0, "load");
    CHECK(m.games_trained == 12345 && m.n_stages == 2 && m.stage_exp[1] == 13, "header roundtrip");
    CHECK(memcmp(n.w[0][2], m.w[0][2], n.size[2] * sizeof(float)) == 0 && m.visits[0][1][77] == 99, "data roundtrip");
    /* TC state must survive a save/load cycle, otherwise resumed training restarts at full step size */
    net_free(&m);
    CHECK(net_enable_tc(&n) == 0, "enable tc");
    n.tc_e[1][3][4242] = 1.5f;
    n.tc_a[1][3][4242] = 2.5f;
    CHECK(net_save(&n, path) == 0 && net_load(&m, path) == 0, "save/load with tc");
    CHECK(m.tc && m.tc_e[1][3][4242] == 1.5f && m.tc_a[1][3][4242] == 2.5f, "tc roundtrip");
    unlink(path);
    net_free(&n);
    net_free(&m);
}

static void test_multistage(void) {
    net_t n;
    net_create(&n, "small", 2, (int[]){0, 11}, 0.0f);
    rng_t r;
    rng_seed(&r, 4);
    randomize(&n, &r);
    board_t big = 0; /* asymmetric board with an 2048 tile -> stage 1, no duplicate features */
    for (int i = 0; i < 16; i++) big = board_set(big, i, i < 11 ? i : (i == 15 ? 11 : 0));
    CHECK(net_stage(&n, big) == 0, "unready stage falls back to 0");
    float v0 = net_value(&n, big);
    net_ensure_stage(&n, big);
    CHECK(net_stage(&n, big) == 1 && fabsf(net_value(&n, big) - v0) < 1e-3f, "promotion copies weights");
    board_t small_b = board_set(big, 15, 10); /* same pattern, max tile 1024 -> stage 0 */
    float s0 = net_value(&n, small_b);
    net_update(&n, big, 50.0f);
    float dv = net_value(&n, big) - v0; /* >= alpha*err; a feature duplicated across symmetries counts twice */
    CHECK(net_stage(&n, small_b) == 0 && net_value(&n, small_b) == s0 && dv >= 4.99f && dv < 6.0f, "stages learn separately (%f)", dv);
    net_free(&n);
}

/* ---- learner tests ---- */

static void test_determinism_and_learning(void) {
    net_t a, b;
    net_create(&a, "small", 1, NULL, 0.0f);
    net_create(&b, "small", 1, NULL, 0.0f);
    rng_t ra, rb;
    rng_seed(&ra, 77);
    rng_seed(&rb, 77);
    path_t pa = {0}, pb = {0};
    uint64_t first = 0, last = 0;
    int same = 1;
    for (int g = 0; g < 3000; g++) {
        game_result_t x = td_train_game(&a, &ra, &pa), y = td_train_game(&b, &rb, &pb);
        same &= x.score == y.score && x.moves == y.moves;
        if (g < 500) first += x.score;
        if (g >= 2500) last += x.score;
    }
    CHECK(same, "same seed must give identical training");
    CHECK(last > 2 * first, "agent should improve: first500=%llu last500=%llu", (unsigned long long)first, (unsigned long long)last);

    /* search depth 0 == greedy */
    rng_t r;
    rng_seed(&r, 1);
    int bad = 0;
    for (int k = 0; k < 500; k++) {
        board_t s = random_board(&r, 10, 30), a1, a2;
        uint32_t r1, r2;
        int d1 = td_best_move(&a, s, &a1, &r1, NULL, NULL), d2 = search_best_move(&a, s, 0, &a2, &r2, NULL, NULL);
        if (d1 != d2) bad++;
        if (d1 >= 0) {
            board_t a3;
            uint32_t r3;
            if (search_best_move(&a, s, 1, &a3, &r3, NULL, NULL) < 0) bad++; /* depth 1 finds a move too */
        }
    }
    CHECK(bad == 0, "search mismatch (%d)", bad);
    free(pa.steps);
    free(pb.steps);
    net_free(&a);
    net_free(&b);
}

static void test_stats(void) {
    stats_t s = {0};
    stats_add(&s, (game_result_t){1000, 100, 11});
    stats_add(&s, (game_result_t){3000, 300, 12});
    stats_add(&s, (game_result_t){10, 5, 3});
    CHECK(s.games == 3 && s.max_score == 3000 && s.reach[11] == 2 && s.reach[12] == 1 && s.max_exp == 12, "stats");
    char hex[17];
    io_board_hex(board_set(board_set(0, 0, 11), 15, 15), hex);
    CHECK(strcmp(hex, "b00000000000000f") == 0, "hex encoding %s", hex);
}

int main(void) {
    board_init_tables();
    test_moves_against_reference();
    test_known_rows();
    test_score_symmetric();
    test_transpose();
    test_spawn();
    test_game_over();
    test_symmetry();
    test_init_value();
    test_update();
    test_save_load();
    test_multistage();
    test_determinism_and_learning();
    test_stats();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
