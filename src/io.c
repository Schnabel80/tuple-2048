/*@ ## io.c – Statistik, Muster und Replays schreiben

Bewusst schlicht: `fprintf` statt JSON-Bibliothek. Die Formate sind klein und
fest, und so hat das Projekt keine einzige externe Abhängigkeit. */
#include "io.h"

#include <stdlib.h>
#include <string.h>

/*@ ### Statistik sammeln

Pro Partie zählen wir Punkte, Zuglänge und die größte Kachel. `reach[e]` wird für
**alle** Exponenten bis zur größten Kachel hochgezählt – wer 4096 erreicht, hat
auch 2048 erreicht. Daraus ergeben sich später die „Erreichquoten“. */
void stats_add(stats_t *s, game_result_t g) {
    s->games++;
    s->sum_score += g.score;
    if (g.score > s->max_score) s->max_score = g.score;
    s->sum_moves += g.moves;
    if (g.max_exp > s->max_exp) s->max_exp = g.max_exp;
    for (int e = 0; e <= g.max_exp && e <= MAX_EXP; e++) s->reach[e]++; //@ Wer 2^k erreicht hat, hat auch alle kleineren Kacheln erreicht.
}

void stats_merge(stats_t *into, const stats_t *from) {
    into->games += from->games;
    into->sum_score += from->sum_score;
    if (from->max_score > into->max_score) into->max_score = from->max_score;
    into->sum_moves += from->sum_moves;
    if (from->max_exp > into->max_exp) into->max_exp = from->max_exp;
    for (int e = 0; e <= MAX_EXP; e++) into->reach[e] += from->reach[e];
}

void io_board_hex(board_t b, char out[17]) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) out[i] = hex[board_get(b, i)]; //@ Zelle i → eine Hex-Ziffer.
    out[16] = 0;
}

/*@ ### Eine Zeile der Lernkurve

Jede Zeile beschreibt das **Fenster** seit dem letzten Meilenstein: wie viele
Partien darin lagen, Durchschnitts- und Bestscore, Zuglänge und welcher Anteil
der Partien 2048, 4096 … erreicht hat. */
void io_csv_header(FILE *f) {
    fprintf(f, "games,elapsed_s,games_per_s,window,avg_score,max_score,avg_moves,max_tile,"
               "rate_2048,rate_4096,rate_8192,rate_16384,rate_32768,best_tile_ever\n");
}

void io_csv_row(FILE *f, uint64_t games, double elapsed, double gps, const stats_t *s, int best_ever) {
    double w = s->games ? (double)s->games : 1.0; //@ Schutz vor Division durch 0 bei leerem Fenster.
    fprintf(f, "%llu,%.1f,%.1f,%llu,%.1f,%llu,%.1f,%u,%.4f,%.4f,%.4f,%.4f,%.4f,%u\n", (unsigned long long)games, elapsed, gps,
            (unsigned long long)s->games, (double)s->sum_score / w, (unsigned long long)s->max_score, (double)s->sum_moves / w,
            s->max_exp ? 1u << s->max_exp : 0u, (double)s->reach[11] / w, (double)s->reach[12] / w, (double)s->reach[13] / w,
            (double)s->reach[14] / w, (double)s->reach[15] / w, best_ever ? 1u << best_ever : 0u);
    fflush(f);
}

/*@ ### Die „Top-Muster“ eines Tupels

Welche Zahlenkonstellationen hält der Agent für besonders wertvoll? Naiv würde
man einfach die größten Gewichte nehmen – das führt aber in die Irre: Die
extremsten Werte sitzen oft auf Belegungen, die der Agent nur ein paarmal
gesehen hat (Rauschen). Darum zählen nur Einträge mit mindestens `min_visits`
Besuchen.

Zusätzlich schreiben wir die **am häufigsten besuchten** Belegungen – das
zeigt, welche Situationen im Spiel des Agenten tatsächlich alltäglich sind.

Technik: Ein kleiner *Min-Heap* der Größe k hält die bisher besten k Einträge.
So kommen wir mit einem einzigen Durchlauf über 16,7 Mio. Einträge aus. */
typedef struct {
    uint32_t idx;
    float key;
} heap_item_t;

static void heap_push(heap_item_t *h, int *n, int k, uint32_t idx, float key) {
    if (*n == k && key <= h[0].key) return; //@ Schlechter als der schwächste der Top-k: ignorieren.
    int i;
    if (*n < k) {
        i = (*n)++;
    } else {
        i = 0; //@ Heap voll: die Wurzel (schwächster Eintrag) wird ersetzt und nach unten gesiebt.
        for (;;) {
            int l = 2 * i + 1, r = l + 1, m = i;
            float km = key;
            if (l < *n && h[l].key < km) m = l, km = h[l].key;
            if (r < *n && h[r].key < km) m = r;
            if (m == i) break;
            h[i] = h[m];
            i = m;
        }
        h[i] = (heap_item_t){idx, key};
        return;
    }
    while (i > 0 && h[(i - 1) / 2].key > key) { //@ Neuer Eintrag: nach oben sieben, solange der Elternknoten größer ist.
        h[i] = h[(i - 1) / 2];
        i = (i - 1) / 2;
    }
    h[i] = (heap_item_t){idx, key};
}

static int cmp_desc(const void *a, const void *b) {
    float x = ((const heap_item_t *)a)->key, y = ((const heap_item_t *)b)->key;
    return (x < y) - (x > y);
}

static void write_entries(FILE *f, const net_t *n, int stage, int t, heap_item_t *h, int cnt) {
    qsort(h, (size_t)cnt, sizeof *h, cmp_desc);
    for (int i = 0; i < cnt; i++) {
        fprintf(f, "%s{\"exps\":[", i ? "," : "");
        for (int j = 0; j < n->len[t]; j++) fprintf(f, "%s%u", j ? "," : "", (h[i].idx >> (4 * j)) & 0xF); //@ Index zurück in Exponenten zerlegen.
        fprintf(f, "],\"value\":%.2f,\"visits\":%u}", n->w[stage][t][h[i].idx], n->visits[stage][t][h[i].idx]);
    }
}

int io_write_top(const net_t *n, int stage, int k, uint32_t min_visits, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    heap_item_t *hv = malloc((size_t)k * sizeof *hv), *hc = malloc((size_t)k * sizeof *hc);
    if (!hv || !hc) {
        fclose(f);
        free(hv);
        free(hc);
        return -1;
    }
    fprintf(f, "{\"format_version\":%d,\"net\":\"%s\",\"games\":%llu,\"stage\":%d,\"min_visits\":%u,\"tuples\":[", FORMAT_VERSION, n->name,
            (unsigned long long)n->games_trained, stage, min_visits);
    for (int t = 0; t < n->n_tuples; t++) {
        int nv = 0, nc = 0;
        const float *w = n->w[stage][t];
        const uint32_t *vis = n->visits[stage][t];
        for (size_t i = 0; i < n->size[t]; i++) {
            if (!vis[i]) continue;
            if (vis[i] >= min_visits) heap_push(hv, &nv, k, (uint32_t)i, w[i]); //@ Nur oft gesehene Einträge kommen in die Wert-Rangliste.
            heap_push(hc, &nc, k, (uint32_t)i, (float)vis[i]); //@ Rangliste nach Häufigkeit – unabhängig vom Wert.
        }
        fprintf(f, "%s{\"id\":%d,\"cells\":[", t ? "," : "", t);
        for (int j = 0; j < n->len[t]; j++) fprintf(f, "%s%u", j ? "," : "", n->cells[t][j]);
        fprintf(f, "],\"top_value\":[");
        write_entries(f, n, stage, t, hv, nv);
        fprintf(f, "],\"top_visited\":[");
        write_entries(f, n, stage, t, hc, nc);
        fprintf(f, "]}");
    }
    fprintf(f, "]}\n");
    free(hv);
    free(hc);
    return fclose(f);
}

/*@ ### Replay-Datei

Format *JSON Lines*: jede Zeile ist ein eigenständiges JSON-Objekt. Zuerst ein
Kopf, dann ein Objekt pro Zug, am Ende eine Zusammenfassung. Vorteil: Die Datei
kann schon gelesen werden, während die Partie noch läuft (Streaming), und ein
Abbruch zerstört nur die letzte Zeile.

Pro Zug: Feld vorher, gewählte Richtung, Punkte, Feld danach, die neue Kachel
(Position + Exponent) und die vier `q`-Werte (`null` = Zug nicht möglich). */
void io_replay_header(FILE *f, const net_t *n, uint64_t seed, int depth) {
    fprintf(f, "{\"type\":\"header\",\"format_version\":%d,\"net\":\"%s\",\"games_trained\":%llu,\"seed\":%llu,\"depth\":%d}\n",
            FORMAT_VERSION, n->name, (unsigned long long)n->games_trained, (unsigned long long)seed, depth);
}

void io_replay_cb(void *ctx, const move_info_t *mi) {
    FILE *f = ((io_replay_t *)ctx)->f;
    char before[17], after[17];
    io_board_hex(mi->before, before);
    io_board_hex(mi->after, after);
    fprintf(f, "{\"t\":%u,\"board\":\"%s\",\"dir\":\"%c\",\"reward\":%u,\"score\":%llu,\"after\":\"%s\",\"spawn\":[%d,%d],\"q\":[", mi->t,
            before, DIR_CHARS[mi->dir], mi->reward, (unsigned long long)mi->score, after, mi->spawn_pos, mi->spawn_exp);
    for (int d = 0; d < N_DIRS; d++) {
        if (mi->valid[d])
            fprintf(f, "%s%.1f", d ? "," : "", mi->q[d]);
        else
            fprintf(f, "%snull", d ? "," : ""); //@ Ungültige Richtung: kein Wert.
    }
    fprintf(f, "]}\n");
}

void io_replay_footer(FILE *f, game_result_t g) {
    fprintf(f, "{\"type\":\"end\",\"score\":%llu,\"moves\":%u,\"max_tile\":%u}\n", (unsigned long long)g.score, g.moves,
            g.max_exp ? 1u << g.max_exp : 0u);
}
