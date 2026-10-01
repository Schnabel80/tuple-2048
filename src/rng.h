/*@ ## rng.h – der Würfel des Spiels

2048 ist ein Zufallsspiel: Nach jedem Zug erscheint auf einem zufälligen leeren
Feld eine neue Kachel. Wir benutzen dafür **nicht** die C-Funktion `rand()`,
sondern einen eigenen, kleinen Zufallsgenerator (*xoshiro256\*\**).

Warum?
- **Reproduzierbarkeit:** Mit demselben Startwert (*Seed*) kommt exakt dieselbe
  Zahlenfolge heraus. Dadurch lässt sich jedes Trainings-Experiment wiederholen.
- **Geschwindigkeit:** Ein paar Bit-Operationen, kein Funktionsaufruf in eine
  Bibliothek, keine versteckten Sperren bei mehreren Threads.
- **Qualität:** `rand()` ist auf vielen Systemen statistisch schwach.

Jeder Trainings-Thread bekommt seinen eigenen Generator – so stören sich die
Threads nicht gegenseitig. */
#ifndef T2048_RNG_H
#define T2048_RNG_H

#include <stdint.h>

typedef struct {
    uint64_t s[4]; //@ Der komplette innere Zustand: 4 × 64 Bit = 256 Bit.
} rng_t;

/*@ ### SplitMix64 – Seed aufbereiten

xoshiro darf nicht mit lauter Nullen starten und mag „schlechte“ Seeds wie 1, 2, 3
nicht besonders. SplitMix64 verwirbelt eine einzelne Zahl so gründlich, dass aus
jedem Seed ein gut gemischter Startzustand entsteht. */
static inline uint64_t splitmix64(uint64_t *x) {
    uint64_t z = (*x += 0x9E3779B97F4A7C15ULL); //@ Addiere die „goldene Zahl“ (2^64 / φ) – verteilt aufeinanderfolgende Seeds weit.
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL; //@ Bits nach unten schieben und mit großer ungerader Zahl multiplizieren: mischt hohe in niedrige Bits.
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static inline void rng_seed(rng_t *r, uint64_t seed) {
    for (int i = 0; i < 4; i++) r->s[i] = splitmix64(&seed); //@ Vier verwirbelte Zahlen füllen den Zustand.
}

static inline uint64_t rotl64(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

/*@ ### Die nächste Zufallszahl

Der eigentliche xoshiro256\*\*-Schritt: Ein Ergebnis wird aus `s[1]` geformt,
danach wird der Zustand mit XOR, Shift und Rotation weitergedreht. */
static inline uint64_t rng_next(rng_t *r) {
    uint64_t *s = r->s;
    uint64_t result = rotl64(s[1] * 5, 7) * 9; //@ „Scrambler“: macht aus dem Zustand eine gut verteilte Ausgabe.
    uint64_t t = s[1] << 17;
    s[2] ^= s[0];
    s[3] ^= s[1];
    s[1] ^= s[2];
    s[0] ^= s[3];
    s[2] ^= t;
    s[3] = rotl64(s[3], 45);
    return result;
}

/*@ ### Zufallszahl im Bereich 0 … n−1

Statt `x % n` (leicht verzerrt) nehmen wir die oberen 32 Bit und multiplizieren
mit `n`: Das Ergebnis der Multiplikation, um 32 Bit nach rechts geschoben, liegt
gleichmäßig in `[0, n)` – schnell und für unsere kleinen `n` (≤ 16) praktisch
fehlerfrei. */
static inline uint32_t rng_below(rng_t *r, uint32_t n) {
    return (uint32_t)(((rng_next(r) >> 32) * (uint64_t)n) >> 32);
}

#endif
