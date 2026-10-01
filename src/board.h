/*@ ## board.h – das Spielfeld als eine einzige Zahl

Das 4×4-Feld von 2048 hat 16 Zellen. In jeder Zelle liegt entweder nichts oder
eine Zweierpotenz 2, 4, 8, … 32768. Wir speichern **nicht die Zahl selbst**,
sondern nur ihren *Exponenten*: 0 = leer, 1 = „2“, 2 = „4“, 3 = „8“, …, 11 = „2048“,
15 = „32768“.

Ein Exponent von 0 bis 15 passt genau in **4 Bit** (ein *Nibble*). 16 Zellen ×
4 Bit = **64 Bit** – das ganze Spielfeld ist also eine einzige `uint64_t`-Zahl.
Man nennt das ein **Bitboard**.

```
Zelle:  0  1  2  3      Bits:  [ 3..0 ] [ 7..4 ] [11..8 ] [15..12]   ← Zeile 0
        4  5  6  7             [19..16] ...                         ← Zeile 1
        8  9 10 11             ...
       12 13 14 15             ...                [63..60]          ← Zeile 3
```

Zelle `i` liegt in den Bits `4·i … 4·i+3`. Eine **Zeile** sind 16 Bit am Stück.

Der Vorteil: Ein Spielfeld kopieren, vergleichen oder speichern kostet so viel
wie eine einzelne Zahl. Und weil eine Zeile nur 16 Bit hat, gibt es nur
2¹⁶ = 65 536 mögliche Zeilen – für **jede** davon können wir das Ergebnis eines
Zuges *vorab* ausrechnen (Tabellen, siehe `board.c`). Ein Zug ist dann nur noch
vier Tabellen-Nachschläge. Das macht die Engine extrem schnell.

Die zeitkritischen Funktionen stehen als `static inline` direkt hier im Header,
damit der Compiler sie in die Lernschleife hineinkopieren (*inlinen*) kann. */
#ifndef T2048_BOARD_H
#define T2048_BOARD_H

#include <stdint.h>
#include "rng.h"

typedef uint64_t board_t; //@ Ein komplettes Spielfeld = 64 Bit.

/*@ ### Die vier Zugrichtungen

Die Reihenfolge ist willkürlich, aber überall gleich. Für die Ausgabe in Dateien
wird jede Richtung als Buchstabe geschrieben (`DIR_CHARS`). */
enum { DIR_LEFT = 0, DIR_RIGHT = 1, DIR_UP = 2, DIR_DOWN = 3, N_DIRS = 4 };
#define DIR_CHARS "LRUD"

#define MAX_EXP 15 //@ Größte darstellbare Kachel: 2^15 = 32768 (4 Bit sind voll).

/* Lookup tables, filled once by board_init_tables(). */
extern uint16_t row_left_table[65536];  //@ Ergebnis „Zeile nach links schieben“ für jede der 65 536 Zeilen.
extern uint16_t row_right_table[65536]; //@ Dasselbe nach rechts.
extern uint32_t row_score_table[65536]; //@ Punkte, die das Verschmelzen in dieser Zeile bringt.

void board_init_tables(void);

/*@ ### Eine Zelle lesen und schreiben

`(b >> 4i) & 0xF` schiebt die gewünschte Zelle ganz nach rechts und blendet
alle anderen Bits aus. Schreiben geht umgekehrt: alte 4 Bit löschen, neue
hineinodern. */
static inline int board_get(board_t b, int i) { return (int)((b >> (4 * i)) & 0xF); }

static inline board_t board_set(board_t b, int i, int e) {
    return (b & ~(0xFULL << (4 * i))) | ((board_t)e << (4 * i)); //@ Maske löscht genau die 4 Bit von Zelle i, dann neuen Wert einsetzen.
}

/*@ ### Transponieren – aus Spalten werden Zeilen

Unsere Tabellen kennen nur **Zeilen** (links/rechts). Für hoch/runter spiegeln
wir das Feld an der Diagonale (*transponieren*): Spalte 0 wird zu Zeile 0 usw.
Dann ist „hoch“ einfach „links“ und „runter“ einfach „rechts“ – danach
transponieren wir zurück.

Das Transponieren geschieht ohne Schleife mit drei Masken in zwei Runden: erst
werden einzelne Nibbles innerhalb von 2×2-Blöcken getauscht, dann ganze 2×2-Blöcke.
Die Diagonale bleibt dabei stehen. */
static inline board_t board_transpose(board_t x) {
    board_t a1 = x & 0xF0F00F0FF0F00F0FULL; //@ Nibbles, die in Runde 1 an ihrem Platz bleiben.
    board_t a2 = x & 0x0000F0F00000F0F0ULL; //@ Diese wandern 12 Bit nach oben (eine Zeile tiefer, eine Spalte links) …
    board_t a3 = x & 0x0F0F00000F0F0000ULL; //@ … und diese 12 Bit nach unten.
    board_t a = a1 | (a2 << 12) | (a3 >> 12);
    board_t b1 = a & 0xFF00FF0000FF00FFULL; //@ Runde 2: dieselbe Idee mit 2×2-Blöcken (8 Bit breit).
    board_t b2 = a & 0x00FF00FF00000000ULL;
    board_t b3 = a & 0x00000000FF00FF00ULL;
    return b1 | (b2 >> 24) | (b3 << 24);
}

/*@ ### Ein Zug

Wir zerlegen das Feld in seine vier Zeilen, schlagen für jede Zeile das Ergebnis
in der Tabelle nach und setzen die Ergebnisse wieder zusammen. Gleichzeitig
summieren wir die Punkte.

Wichtig für das Lernen: `reward` sind **nur die Punkte dieses einen Zuges**
(die Werte aller neu entstandenen Kacheln). Gibt die Funktion dasselbe Feld
zurück, das hineinging, war der Zug **ungültig** (nichts hat sich bewegt). */
static inline board_t board_move_rows(board_t b, const uint16_t *table, uint32_t *reward) {
    board_t out = 0;
    uint32_t r = 0;
    for (int row = 0; row < 4; row++) {
        uint32_t line = (uint32_t)((b >> (16 * row)) & 0xFFFF); //@ 16 Bit = eine Zeile herausschneiden.
        out |= (board_t)table[line] << (16 * row);                //@ Nachgeschlagenes Ergebnis an dieselbe Stelle zurück.
        r += row_score_table[line];                               //@ Punkte sind für links und rechts identisch (siehe board.c).
    }
    *reward = r;
    return out;
}

static inline board_t board_move(board_t b, int dir, uint32_t *reward) {
    switch (dir) {
    case DIR_LEFT:  return board_move_rows(b, row_left_table, reward);
    case DIR_RIGHT: return board_move_rows(b, row_right_table, reward);
    case DIR_UP:    return board_transpose(board_move_rows(board_transpose(b), row_left_table, reward)); //@ Hoch = transponieren, links, zurücktransponieren.
    default:        return board_transpose(board_move_rows(board_transpose(b), row_right_table, reward));
    }
}

/*@ ### Leere Felder finden

Ein Trick ohne Schleife: Eine Zelle ist leer, wenn **alle vier** ihrer Bits 0
sind. Wir odern die Bits jeder Zelle auf ihr unterstes Bit zusammen
(`b | b>>1`, dann `| >>2`) und invertieren. Übrig bleibt pro leerer Zelle genau
ein gesetztes Bit an Position `4·i`. */
static inline uint64_t board_empty_mask(board_t b) {
    uint64_t x = b | (b >> 1);
    x |= x >> 2;                              //@ Bit 4i ist jetzt 1, sobald irgendein Bit der Zelle i gesetzt war.
    return ~x & 0x1111111111111111ULL;        //@ Invertieren und nur das unterste Bit jeder Zelle behalten.
}

static inline int board_count_empty(board_t b) { return __builtin_popcountll(board_empty_mask(b)); }

/*@ ### Neue Kachel erscheinen lassen

Regel des Originalspiels: Auf ein zufälliges leeres Feld kommt mit 90 % eine „2“
(Exponent 1) und mit 10 % eine „4“ (Exponent 2). Wir wählen die k-te leere Zelle,
indem wir k-mal das unterste gesetzte Bit der Leer-Maske löschen
(`m &= m − 1`). `__builtin_ctzll` zählt dann die Nullen bis zum nächsten Bit –
geteilt durch 4 ergibt das die Zellennummer. */
static inline board_t board_spawn(board_t b, rng_t *r, int *pos_out, int *exp_out) {
    uint64_t m = board_empty_mask(b);
    int n = __builtin_popcountll(m);
    if (n == 0) return b; //@ Kein Platz – kann nach einem gültigen Zug nie passieren, schützt aber vor Fehlbenutzung.
    uint32_t k = rng_below(r, (uint32_t)n);
    while (k--) m &= m - 1;                           //@ Unterstes gesetztes Bit löschen, k-mal.
    int pos = __builtin_ctzll(m) >> 2;                //@ Bitposition / 4 = Zellennummer.
    int e = rng_below(r, 10) == 0 ? 2 : 1;            //@ 1 von 10 Fällen: eine „4“, sonst eine „2“.
    if (pos_out) *pos_out = pos;
    if (exp_out) *exp_out = e;
    return b | ((board_t)e << (4 * pos));
}

/*@ ### Spielende

Das Spiel ist vorbei, wenn **keine** der vier Richtungen das Feld verändert. */
static inline int board_game_over(board_t b) {
    uint32_t r;
    for (int d = 0; d < N_DIRS; d++)
        if (board_move(b, d, &r) != b) return 0;
    return 1;
}

static inline int board_max_exp(board_t b) {
    int m = 0;
    for (int i = 0; i < 16; i++) {
        int e = board_get(b, i);
        if (e > m) m = e;
    }
    return m;
}

/* Initial board: two spawned tiles on an empty field. */
static inline board_t board_new_game(rng_t *r) { return board_spawn(board_spawn(0, r, 0, 0), r, 0, 0); }

void board_print(board_t b);

#endif
