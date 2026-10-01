/*@ ## search.c – Vorausschauen mit Expectimax

Das gelernte Netz bewertet ein Feld „aus dem Bauch heraus“ – ein einziger Blick.
Beim **Vorführen** (nicht beim Training) können wir zusätzlich ein paar Züge
**vorausdenken** und das Bauchgefühl erst am Ende des Gedankengangs befragen.

2048 ist ein Spiel gegen den Zufall, nicht gegen einen Gegner. Darum wechseln
sich im Suchbaum zwei Arten von Knoten ab:

- **Max-Knoten** (der Agent ist dran): Er nimmt den **besten** der vier Züge.
- **Zufalls-Knoten** (das Spiel ist dran): Eine neue Kachel erscheint – wir
  nehmen den **Durchschnitt** über alle leeren Felder, gewichtet mit 90 % für
  eine „2“ und 10 % für eine „4“.

Daher der Name: *Expect*(ation) + *Max*. Mit `depth = 1` schaut der Agent eine
neue Kachel und einen weiteren eigenen Zug voraus. Jede Stufe kostet etwa den
Faktor 100 an Rechenzeit – darum gibt es eine kleine **Merktabelle**
(*Transposition Cache*): Dieselbe Stellung, die über verschiedene Zugfolgen
erreicht wird, wird nur einmal ausgerechnet. */
#include <stdlib.h>
#include <string.h>

#include "td.h"

#define CACHE_BITS 20
#define CACHE_SIZE (1u << CACHE_BITS)

typedef struct {
    board_t key;
    uint32_t gen; //@ „Generation“: Einträge aus früheren Zügen gelten automatisch als leer.
    int8_t depth;
    float val;
} cache_entry_t;

static __thread cache_entry_t *cache; //@ Jeder Thread hat seine eigene Merktabelle – kein Sperren nötig.
static __thread uint32_t cache_gen;

static inline uint32_t hash_board(board_t b) {
    b ^= b >> 33;
    b *= 0xFF51AFD7ED558CCDULL; //@ Schnelle Misch-Funktion (aus MurmurHash3): verteilt ähnliche Felder auf weit entfernte Plätze.
    b ^= b >> 33;
    return (uint32_t)b & (CACHE_SIZE - 1);
}

static float chance_node(const net_t *n, board_t after, int depth);

/*@ ### Max-Knoten: der Agent wählt

Der Wert einer Stellung ist der beste erreichbare Zugwert. Gibt es keinen
gültigen Zug, ist das Spiel vorbei – Wert 0. */
static float max_node(const net_t *n, board_t b, int depth) {
    float best = 0.0f;
    for (int d = 0; d < N_DIRS; d++) {
        uint32_t r;
        board_t a = board_move(b, d, &r);
        if (a == b) continue;
        float v = (float)r + chance_node(n, a, depth - 1);
        if (v > best) best = v;
    }
    return best;
}

/*@ ### Zufalls-Knoten: das Spiel würfelt

Ist die Suchtiefe aufgebraucht, fragen wir das Netz (`net_value`). Sonst
durchlaufen wir alle leeren Felder und beide möglichen neuen Kacheln und
bilden den gewichteten Durchschnitt der folgenden Max-Knoten. */
static float chance_node(const net_t *n, board_t after, int depth) {
    if (depth <= 0) return net_value(n, after); //@ Ende des Gedankengangs: jetzt entscheidet das gelernte Bauchgefühl.

    cache_entry_t *e = &cache[hash_board(after)];
    if (e->gen == cache_gen && e->key == after && e->depth == depth) return e->val; //@ Schon einmal ausgerechnet? Dann nachschlagen.

    uint64_t m = board_empty_mask(after);
    int cnt = __builtin_popcountll(m); //@ Anzahl leerer Felder = Anzahl möglicher Positionen für die neue Kachel.
    float sum = 0.0f;
    while (m) {
        int pos = __builtin_ctzll(m) >> 2;
        m &= m - 1;
        sum += 0.9f * max_node(n, after | ((board_t)1 << (4 * pos)), depth); //@ Eine „2“ erscheint (90 %).
        sum += 0.1f * max_node(n, after | ((board_t)2 << (4 * pos)), depth); //@ Eine „4“ erscheint (10 %).
    }
    float v = cnt ? sum / (float)cnt : 0.0f; //@ Jedes leere Feld ist gleich wahrscheinlich.

    e->key = after;
    e->gen = cache_gen;
    e->depth = (int8_t)depth;
    e->val = v;
    return v;
}

/*@ ### Der beste Zug mit Vorausschau

Mit `depth = 0` ist das exakt `td_best_move` (reines Bauchgefühl). Mit
`depth > 0` wird jeder der vier Züge durch die Suche bewertet. Die `q`-Werte
werden genauso zurückgegeben, damit die Webseite sie anzeigen kann. */
int search_best_move(const net_t *n, board_t b, int depth, board_t *after, uint32_t *reward, float q[N_DIRS], int valid[N_DIRS]) {
    if (depth <= 0) return td_best_move(n, b, after, reward, q, valid);
    if (!cache) {
        cache = calloc(CACHE_SIZE, sizeof *cache); //@ Beim ersten Aufruf: Merktabelle anlegen (1 Mio. Einträge, ~24 MB).
        if (!cache) abort();
    }
    if (++cache_gen == 0) { //@ Generationszähler übergelaufen: Tabelle einmal wirklich leeren.
        memset(cache, 0, CACHE_SIZE * sizeof *cache);
        cache_gen = 1;
    }
    int best = -1;
    float best_q = 0.0f;
    for (int d = 0; d < N_DIRS; d++) {
        uint32_t r;
        board_t a = board_move(b, d, &r);
        int ok = a != b;
        float v = ok ? (float)r + chance_node(n, a, depth) : 0.0f; //@ Zugwert = Punkte + erwarteter Wert nach dem Zufall, rekursiv ausgerechnet.
        if (q) q[d] = v;
        if (valid) valid[d] = ok;
        if (ok && (best < 0 || v > best_q)) {
            best = d;
            best_q = v;
            *after = a;
            *reward = r;
        }
    }
    return best;
}
