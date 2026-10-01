/*@ ## ntuple.h – wie der Computer ein Spielfeld „bewertet“

Der Agent braucht eine Antwort auf die Frage: **„Wie gut ist dieses Spielfeld?“**
– genauer: Wie viele Punkte werde ich ab hier voraussichtlich noch holen?
Diese Zahl heißt **Wert** `V(s)` des Feldes `s`.

Es gibt etwa 16¹⁶ ≈ 18 Trillionen mögliche Felder – viel zu viele, um für jedes
einen Wert zu speichern. Ein **N-Tuple-Network** löst das mit einem Trick:

- Ein **Tupel** ist eine feste Auswahl von Zellen, z.B. die 4 Zellen der
  obersten Zeile oder 6 Zellen in L-Form.
- Für jedes Tupel gibt es eine **Tabelle** mit einem Eintrag (*Gewicht*) für
  jede mögliche Belegung dieser Zellen. Bei 6 Zellen sind das 16⁶ ≈ 16,7 Mio.
  Einträge – viel, aber machbar.
- Der Wert eines Feldes ist die **Summe** der Gewichte, die zu den aktuellen
  Belegungen aller Tupel gehören.

Jedes Tupel ist also ein kleiner Experte, der nur auf „seinen“ Ausschnitt schaut
und sagt: „Diese Konstellation hier ist erfahrungsgemäß so-und-so viel wert.“
Die Summe der Expertenmeinungen ergibt die Bewertung.

**Symmetrie:** Ein Muster in der linken oberen Ecke ist genauso gut wie dasselbe
Muster gespiegelt oder gedreht in einer anderen Ecke. Darum wird jedes Tupel in
**8 Varianten** (4 Drehungen × gespiegelt/nicht gespiegelt) auf das Feld gelegt,
und alle 8 benutzen *dieselbe* Tabelle. Das verachtfacht die Lernerfahrung pro
Zug und macht die Bewertung automatisch symmetrisch.

**Mehrere Stufen (Multi-Stage):** Optional gibt es pro Spielphase (z.B. „noch
keine 8192“ / „8192 erreicht“ / „16384 erreicht“) einen eigenen Satz Tabellen,
weil im späten Spiel andere Muster zählen als am Anfang. */
#ifndef T2048_NTUPLE_H
#define T2048_NTUPLE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "board.h"

#define MAX_TUPLES 16
#define MAX_TUPLE_LEN 6
#define N_ISO 8 //@ 4 Drehungen × 2 (gespiegelt / nicht gespiegelt).
#define MAX_STAGES 4
#define MAX_FEATURES (MAX_TUPLES * N_ISO)

/*@ ### Das Netz als Datenstruktur

Alles, was der Agent „weiß“, steckt in den Gewichts-Arrays `w`. Die restlichen
Felder beschreiben nur, *welche* Zellen zu welchem Tupel gehören.

- `iso[t][s][j]` – die j-te Zelle von Tupel t in Symmetrie-Variante s,
  einmal beim Anlegen ausgerechnet.
- `visits` – wie oft jedes Gewicht schon angepasst wurde. Nur für die Anzeige
  („welche Muster hat der Agent oft gesehen?“), nicht fürs Spielen.
- `tc_e`, `tc_a` – Zusatzspeicher für *TC-Learning* (siehe `net_update`). */
typedef struct {
    char name[32];
    int n_tuples;
    int len[MAX_TUPLES];
    uint8_t cells[MAX_TUPLES][MAX_TUPLE_LEN];
    uint8_t iso[MAX_TUPLES][N_ISO][MAX_TUPLE_LEN];
    size_t size[MAX_TUPLES]; //@ 16^Länge – Anzahl Einträge in der Tabelle dieses Tupels.

    int n_stages;
    int stage_exp[MAX_STAGES]; //@ Stufe s gilt ab höchster Kachel ≥ 2^stage_exp[s] (stage_exp[0] = 0).
    volatile int stage_ready[MAX_STAGES]; //@ Wurde Stufe s schon aus Stufe s−1 „befördert“?

    float *w[MAX_STAGES][MAX_TUPLES];          //@ Die gelernten Gewichte – das „Gehirn“.
    uint32_t *visits[MAX_STAGES][MAX_TUPLES];
    float *tc_e[MAX_STAGES][MAX_TUPLES];       //@ TC: Summe der Korrekturen (mit Vorzeichen).
    float *tc_a[MAX_STAGES][MAX_TUPLES];       //@ TC: Summe der Beträge der Korrekturen.

    float alpha;   //@ Lernrate pro Zustand; wird auf alle Merkmale aufgeteilt.
    int tc;        //@ 1 = TC-Learning aktiv.
    uint64_t games_trained;
} net_t;

/* Creation, destruction, persistence. */
int net_create(net_t *n, const char *preset, int n_stages, const int *stage_exp, float init_value);
int net_enable_tc(net_t *n);
void net_free(net_t *n);
int net_save(const net_t *n, const char *path);
int net_load(net_t *n, const char *path);
void net_promote_stage(net_t *n, int s);
void net_ensure_stage(net_t *n, board_t b);
size_t net_bytes(const net_t *n);

/*@ ### Ein Tupel „ablesen“

Für ein Tupel mit den Zellen `c₀, c₁, …` werden die Exponenten dieser Zellen
hintereinander zu einer Zahl zusammengesetzt: `index = e₀ + 16·e₁ + 16²·e₂ + …`.
Diese Zahl ist die **Zeilennummer in der Tabelle** des Tupels. Jede mögliche
Belegung bekommt so genau einen eigenen Tabellenplatz. */
static inline uint32_t net_index(board_t b, const uint8_t *cells, int len) {
    uint32_t idx = 0;
    for (int j = 0; j < len; j++)
        idx |= (uint32_t)((b >> (4 * cells[j])) & 0xF) << (4 * j); //@ Exponent der Zelle holen und an Position j (je 4 Bit) einsetzen.
    return idx;
}

/*@ ### Welche Stufe gilt?

Ohne Multi-Stage gibt es nur Stufe 0. Sonst entscheidet die höchste Kachel.
Ist eine Stufe noch nie trainiert worden, nehmen wir die höchste fertige Stufe
darunter. */
static inline int net_stage(const net_t *n, board_t b) {
    if (n->n_stages == 1) return 0;
    int m = board_max_exp(b), s = 0;
    for (int i = 1; i < n->n_stages; i++)
        if (m >= n->stage_exp[i]) s = i;
    while (s > 0 && !n->stage_ready[s]) s--;
    return s;
}

/*@ ### Der Wert eines Feldes: V(s)

Das Herz der Bewertung – und die am häufigsten ausgeführte Funktion des ganzen
Programms. Für jedes Tupel und jede seiner 8 Symmetrie-Varianten: Index
ausrechnen, Gewicht nachschlagen, aufsummieren. Bei 4 Tupeln sind das 32
Tabellenzugriffe pro Bewertung. */
static inline float net_value(const net_t *n, board_t b) {
    int st = net_stage(n, b);
    float v = 0.0f; //@ Welche Tabellen gelten? (Nur bei Multi-Stage relevant.)
    for (int t = 0; t < n->n_tuples; t++) {
        const float *w = n->w[st][t];
        for (int s = 0; s < N_ISO; s++)
            v += w[net_index(b, n->iso[t][s], n->len[t])]; //@ Ein „Experte“ gibt seine Meinung ab – alle Meinungen werden addiert.
    }
    return v;
}

float net_update(net_t *n, board_t b, float err);

#endif
