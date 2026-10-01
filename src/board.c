/*@ ## board.c – die Zug-Tabellen

Hier passiert die eigentliche Spielregel – aber nur **einmal beim Programmstart**.
Wir gehen alle 65 536 möglichen Zeilen durch, schieben jede nach links,
verschmelzen gleiche Nachbarn und merken uns das Ergebnis. Danach wird im
ganzen Programm nie wieder „gerechnet“, sondern nur nachgeschlagen.

Kosten: 65 536 × (2 + 2 + 4 Byte) ≈ 512 KB Speicher, ein paar Millisekunden
Rechenzeit beim Start. Gewinn: ein Zug kostet danach nur noch Nanosekunden. */
#include "board.h"

#include <stdio.h>

uint16_t row_left_table[65536];
uint16_t row_right_table[65536];
uint32_t row_score_table[65536];

/*@ ### Eine Zeile nach links schieben – die Spielregel im Klartext

Die vier Zellen einer Zeile werden ausgepackt. Dann:

1. **Zusammenschieben:** Alle nicht-leeren Kacheln rutschen nach links.
2. **Verschmelzen:** Zwei gleiche Nachbarn werden zu einer Kachel mit dem
   doppelten Wert (Exponent + 1). Jede Kachel darf pro Zug **nur einmal**
   verschmelzen – darum springt die Schleife nach einem Merge über die
   verbrauchte Kachel hinweg. Aus `2 2 2 2` wird so `4 4`, nicht `8`.
3. Der Wert der neuen Kachel (2^Exponent) zählt als Punkte.

Sonderfall: Zwei 32768er (Exponent 15) dürfen nicht verschmelzen, weil 65536
nicht mehr in 4 Bit passt. Im echten Spiel kommt das praktisch nie vor. */
static uint16_t slide_left(uint16_t row, uint32_t *score) {
    int in[4], out[4] = {0, 0, 0, 0}; //@ in = gesammelte Kacheln, out = Ergebniszeile (startet leer).
    int n = 0;
    for (int c = 0; c < 4; c++) {
        int e = (row >> (4 * c)) & 0xF;
        if (e) in[n++] = e; //@ Schritt 1: nur nicht-leere Kacheln einsammeln – das „Zusammenschieben“.
    }
    int o = 0;
    *score = 0;
    for (int i = 0; i < n; i++) {
        if (i + 1 < n && in[i] == in[i + 1] && in[i] < MAX_EXP) { //@ Schritt 2: gleicher Nachbar? Dann verschmelzen.
            out[o++] = in[i] + 1;          //@ Exponent + 1 = doppelter Wert.
            *score += 1u << (in[i] + 1);   //@ Schritt 3: die neue Kachel zählt als Punkte.
            i++;                           //@ Den Partner überspringen – er ist „verbraucht“.
        } else {
            out[o++] = in[i];
        }
    }
    uint16_t res = 0;
    for (int c = 0; c < 4; c++) res |= (uint16_t)(out[c] << (4 * c)); //@ Ergebnis wieder in 16 Bit verpacken.
    return res;
}

/*@ ### Zeile umdrehen

Nach rechts schieben ist dasselbe wie: Zeile spiegeln, nach links schieben,
zurückspiegeln. So brauchen wir die Spielregel nur einmal zu schreiben. */
static uint16_t reverse_row(uint16_t row) {
    return (uint16_t)(((row & 0xF) << 12) | ((row & 0xF0) << 4) | ((row & 0xF00) >> 4) | ((row & 0xF000) >> 12));
}

/*@ ### Alle Tabellen füllen

Für jede mögliche Zeile: Ergebnis links, Ergebnis rechts, Punkte.

Die Punkte-Tabelle gilt für **beide** Richtungen. Das ist kein Zufall: Gleiche
Kacheln verschmelzen paarweise innerhalb einer Folge gleicher Werte, und eine
Folge der Länge k ergibt in beiden Richtungen ⌊k/2⌋ Verschmelzungen derselben
Kacheln – also dieselben Punkte. (Ein Test in `tests/` prüft das.) */
void board_init_tables(void) {
    for (uint32_t row = 0; row < 65536; row++) {
        uint32_t score;
        uint16_t left = slide_left((uint16_t)row, &score);
        row_left_table[row] = left; //@ Nachschlagen statt rechnen: ab jetzt kostet „links“ für diese Zeile nur einen Speicherzugriff.
        row_score_table[row] = score;
        uint32_t dummy;
        row_right_table[row] = reverse_row(slide_left(reverse_row((uint16_t)row), &dummy)); //@ Spiegeln → links → zurückspiegeln.
    }
}

void board_print(board_t b) {
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            int e = board_get(b, 4 * r + c);
            printf("%6u", e ? 1u << e : 0u);
        }
        printf("\n");
    }
}
