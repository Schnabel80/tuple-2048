/*@ ## ntuple.c – Tupel anlegen, lernen, speichern

Diese Datei enthält:
- die **Tupel-Formen** (welche Zellen gehören zusammen),
- das Erzeugen der 8 **Symmetrie-Varianten**,
- die **Lern-Regel** `net_update` – die einzige Stelle, an der sich das Wissen
  des Agenten verändert,
- Speichern und Laden der Gewichte. */
#include "ntuple.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

/*@ ### Die Tupel-Formen

Zellen sind von 0 (oben links) bis 15 (unten rechts) nummeriert:

```
 0  1  2  3
 4  5  6  7
 8  9 10 11
12 13 14 15
```

- **small**: zwei gerade 4er-Reihen (Rand und zweite Reihe) und zwei
  2×2-Quadrate. Klein (1 MB), schnell, gut zum Anschauen – spielt aber nur
  mittelmäßig.
- **strong**: vier 6er-Tupel (zwei 2×3-Rechtecke, zwei Reihen-Paare) nach
  Yeh et al. – der Standard für einen guten 2048-Agenten.
- **strong8**: acht 6er-Tupel nach Matsuzaki (2016), systematisch ausgesucht.
  Noch stärker, braucht doppelt so viel Speicher.

Warum 6 Zellen? Größere Tupel „sehen“ mehr Zusammenhang (etwa eine sauber
absteigende Kette 2048-1024-512 entlang des Randes), aber jede Zelle mehr
versechzehnfacht die Tabelle. 6 ist der praktische Kompromiss. */
typedef struct {
    const char *name;
    int n, len;
    uint8_t cells[MAX_TUPLES][MAX_TUPLE_LEN];
} preset_t;

static const preset_t PRESETS[] = {
    {"small", 4, 4, {{0, 1, 2, 3}, {4, 5, 6, 7}, {0, 1, 4, 5}, {1, 2, 5, 6}}},
    {"strong", 4, 6, {{0, 1, 2, 3, 4, 5}, {4, 5, 6, 7, 8, 9}, {0, 1, 2, 4, 5, 6}, {4, 5, 6, 8, 9, 10}}},
    {"strong8", 8, 6,
     {{0, 1, 2, 4, 5, 6},
      {1, 2, 5, 6, 9, 13},
      {0, 1, 2, 3, 4, 5},
      {0, 1, 5, 6, 7, 10},
      {0, 1, 2, 5, 9, 10},
      {0, 1, 5, 9, 13, 14},
      {0, 1, 5, 8, 9, 13},
      {0, 1, 2, 4, 6, 10}}},
};

/*@ ### Die 8 Symmetrien einer Zelle

Eine Drehung um 90° bringt Zelle (Zeile r, Spalte c) nach (c, 3−r). Spiegeln an
der senkrechten Achse bringt (r, c) nach (r, 3−c). Variante `s` = erst spiegeln
(falls s ≥ 4), dann `s mod 4`-mal drehen. */
static int iso_cell(int cell, int s) {
    int r = cell / 4, c = cell % 4; //@ Zellnummer → Zeile und Spalte.
    if (s >= 4) c = 3 - c; //@ Spiegeln.
    for (int k = 0; k < (s & 3); k++) {
        int nr = c, nc = 3 - r; //@ Eine 90°-Drehung.
        r = nr;
        c = nc;
    }
    return 4 * r + c;
}

/*@ ### Großen Speicher besorgen

Die Tabellen des starken Netzes sind zusammen ~268 MB groß, und jeder
Tabellenzugriff landet an einer zufälligen Stelle. Normale 4-KB-Speicherseiten
bedeuten dann ständig teure Adressübersetzungen (*TLB-Misses*). Mit `mmap` plus
`MADV_HUGEPAGE` bitten wir das Betriebssystem um 2-MB-Seiten – das macht das
Training spürbar schneller. Funktioniert das nicht, läuft es trotzdem. */
static void *big_alloc(size_t bytes) {
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); //@ Anonymer Speicher direkt vom Betriebssystem – bereits mit Nullen gefüllt.
    if (p == MAP_FAILED) return NULL;
#ifdef MADV_HUGEPAGE
    madvise(p, bytes, MADV_HUGEPAGE); //@ Nur eine Bitte an den Kernel – Fehlschlag ist harmlos.
#endif
    return p;
}

static void big_free(void *p, size_t bytes) {
    if (p) munmap(p, bytes);
}

/*@ ### Ein Netz anlegen

`init_value` ist der Startwert, den **ein leeres Netz für jedes Feld**
vorhersagt. Er wird gleichmäßig auf alle Merkmale (Tupel × Symmetrien)
verteilt.

- `init_value = 0`: klassisches TD-Learning. Der Agent kennt nichts und
  „glaubt“ zunächst, kein Feld sei etwas wert.
- `init_value > 0`: **Optimistische Initialisierung (OTD)**. Der Agent glaubt
  am Anfang, *jedes* Feld sei sehr viel wert. Felder, die er oft gesehen hat,
  werden schnell „entzaubert“ – noch nie gesehene Felder behalten ihren hohen
  Wert und wirken dadurch verlockend. So probiert er systematisch Neues aus,
  ohne dass wir ihm Zufallszüge vorschreiben müssen. */
int net_create(net_t *n, const char *preset, int n_stages, const int *stage_exp, float init_value) {
    memset(n, 0, sizeof *n);
    const preset_t *p = NULL;
    for (size_t i = 0; i < sizeof PRESETS / sizeof PRESETS[0]; i++)
        if (strcmp(PRESETS[i].name, preset) == 0) p = &PRESETS[i]; //@ Preset per Namen suchen.
    if (!p) return -1;
    if (n_stages < 1 || n_stages > MAX_STAGES) return -1;

    snprintf(n->name, sizeof n->name, "%s", p->name);
    n->n_tuples = p->n;
    n->n_stages = n_stages;
    n->alpha = 0.1f; //@ Standard-Lernrate: 10 % des Fehlers pro Update.
    for (int s = 0; s < n_stages; s++) n->stage_exp[s] = s == 0 ? 0 : stage_exp[s];
    n->stage_ready[0] = 1; //@ Stufe 0 ist immer aktiv; höhere Stufen werden erst beim ersten Erreichen befördert.

    float per_feature = init_value / (float)(p->n * N_ISO); //@ Startwert gleichmäßig auf alle 8·n Merkmale verteilen.
    for (int t = 0; t < p->n; t++) {
        n->len[t] = p->len;
        memcpy(n->cells[t], p->cells[t], (size_t)p->len);
        n->size[t] = (size_t)1 << (4 * p->len); //@ 16^len Einträge.
        for (int s = 0; s < N_ISO; s++)
            for (int j = 0; j < p->len; j++) n->iso[t][s][j] = (uint8_t)iso_cell(p->cells[t][j], s); //@ Alle 8 Lagen jedes Tupels einmal vorab ausrechnen – im Training wird nur noch nachgeschlagen.
        for (int st = 0; st < n_stages; st++) {
            n->w[st][t] = big_alloc(n->size[t] * sizeof(float));
            n->visits[st][t] = big_alloc(n->size[t] * sizeof(uint32_t)); //@ mmap liefert genullten Speicher – Besuche starten bei 0.
            if (!n->w[st][t] || !n->visits[st][t]) return -2;
            if (per_feature != 0.0f)
                for (size_t i = 0; i < n->size[t]; i++) n->w[st][t][i] = per_feature; //@ OTD: jedes Gewicht startet mit seinem Anteil am optimistischen Startwert.
        }
    }
    return 0;
}

int net_enable_tc(net_t *n) {
    for (int st = 0; st < n->n_stages; st++)
        for (int t = 0; t < n->n_tuples; t++) {
            if (n->tc_e[st][t]) continue;
            n->tc_e[st][t] = big_alloc(n->size[t] * sizeof(float));
            n->tc_a[st][t] = big_alloc(n->size[t] * sizeof(float));
            if (!n->tc_e[st][t] || !n->tc_a[st][t]) return -2;
        }
    n->tc = 1; //@ Ab jetzt nutzt net_update die Kohärenz-Formel.
    return 0;
}

void net_free(net_t *n) {
    for (int st = 0; st < n->n_stages; st++)
        for (int t = 0; t < n->n_tuples; t++) {
            big_free(n->w[st][t], n->size[t] * sizeof(float));
            big_free(n->visits[st][t], n->size[t] * sizeof(uint32_t));
            big_free(n->tc_e[st][t], n->size[t] * sizeof(float));
            big_free(n->tc_a[st][t], n->size[t] * sizeof(float));
        }
    memset(n, 0, sizeof *n);
}

size_t net_bytes(const net_t *n) {
    size_t b = 0;
    for (int t = 0; t < n->n_tuples; t++) b += n->size[t] * (sizeof(float) + sizeof(uint32_t) + (n->tc ? 2 * sizeof(float) : 0));
    return b * (size_t)n->n_stages;
}

/*@ ### Stufe befördern (Multi-Stage)

Erreicht der Agent zum ersten Mal eine neue Spielphase, startet deren Tabelle
nicht bei null, sondern als **Kopie der vorherigen Stufe** („weight
promotion“, Jaśkowski 2016). Das bisher Gelernte ist ein guter Startpunkt;
danach entwickeln sich die Stufen getrennt weiter. */
static pthread_mutex_t promote_lock = PTHREAD_MUTEX_INITIALIZER;

void net_promote_stage(net_t *n, int s) {
    if (s <= 0 || s >= n->n_stages || __atomic_load_n(&n->stage_ready[s], __ATOMIC_ACQUIRE)) return;
    pthread_mutex_lock(&promote_lock); //@ Mehrere Threads könnten gleichzeitig die neue Phase erreichen – nur einer kopiert.
    for (int k = 1; k <= s; k++) {
        if (n->stage_ready[k]) continue;
        for (int t = 0; t < n->n_tuples; t++) memcpy(n->w[k][t], n->w[k - 1][t], n->size[t] * sizeof(float));
        __atomic_store_n(&n->stage_ready[k], 1, __ATOMIC_RELEASE); //@ Erst nach dem Kopieren freischalten.
    }
    pthread_mutex_unlock(&promote_lock);
}

void net_ensure_stage(net_t *n, board_t b) {
    if (n->n_stages == 1) return;
    int m = board_max_exp(b), s = 0;
    for (int i = 1; i < n->n_stages; i++)
        if (m >= n->stage_exp[i]) s = i;
    if (s > 0 && !__atomic_load_n(&n->stage_ready[s], __ATOMIC_ACQUIRE)) net_promote_stage(n, s);
}

/*@ ### Lernen: ein Gewicht-Update

Gegeben: ein Feld `b` und der **Fehler** `err` = „was tatsächlich
herauskam“ minus „was das Netz vorhergesagt hat“. Ist `err` positiv, war das
Netz zu pessimistisch; ist er negativ, zu optimistisch.

Jedes der beteiligten Gewichte wird um ein kleines Stück in Richtung des Fehlers
verschoben: `w += α · err / m`. Die Division durch die Anzahl der Merkmale `m`
sorgt dafür, dass sich die Gesamt-Vorhersage um genau `α · err` bewegt – egal
wie viele Tupel das Netz hat. Mit α = 0,1 wandert die Vorhersage also bei
jedem Update 10 % des Weges zum richtigen Wert. Klein genug, um nicht zu
überreagieren, groß genug, um voranzukommen.

**TC-Learning (Temporal Coherence):** Jedes Gewicht führt Buch über seine
bisherigen Korrekturen – die Summe mit Vorzeichen (`E`) und die Summe der
Beträge (`A`). Ist `|E|/A` nahe 1, wurde das Gewicht immer in dieselbe Richtung
korrigiert: Es ist noch weit vom Ziel entfernt und darf schnell lernen. Ist
`|E|/A` nahe 0, pendelt es hin und her: Es ist praktisch am Ziel, weitere
Korrekturen sind nur Rauschen – also wird es fast eingefroren. So bekommt jedes
Gewicht automatisch seine eigene, passende Lernrate. */
float net_update(net_t *n, board_t b, float err) {
    int st = net_stage(n, b);
    int m = n->n_tuples * N_ISO;
    float step = n->alpha * err / (float)m; //@ Anteil jedes einzelnen Merkmals an der Gesamtkorrektur.
    float v = 0.0f;
    for (int t = 0; t < n->n_tuples; t++) {
        float *w = n->w[st][t];
        uint32_t *vis = n->visits[st][t];
        for (int s = 0; s < N_ISO; s++) {
            uint32_t i = net_index(b, n->iso[t][s], n->len[t]);
            if (n->tc) {
                float *e = &n->tc_e[st][t][i], *a = &n->tc_a[st][t][i];
                float coh = *a > 0.0f ? fabsf(*e) / *a : 1.0f; //@ Kohärenz: 1 = „immer gleiche Richtung“, 0 = „pendelt“.
                w[i] += step * coh;
                *e += step; //@ Buchführung: Korrektur mit Vorzeichen aufsummieren …
                *a += fabsf(step); //@ … und ihren Betrag. |E|/A misst, wie einig sich die Korrekturen sind.
            } else {
                w[i] += step; //@ Die eigentliche Lern-Zeile: Gewicht ein Stück in Richtung des Fehlers schieben.
            }
            if (vis[i] != UINT32_MAX) vis[i]++; //@ Besuche zählen (für die Muster-Anzeige), ohne Überlauf.
            v += w[i];
        }
    }
    return v; //@ Neuer Wert des Feldes nach dem Update – wird für den nächsten Rückwärtsschritt gebraucht.
}

/*@ ### Gewichte speichern

Ein einfaches Binärformat: ein kurzer Kopf (Kennung, Version, Tupel-Formen,
Stufen, Anzahl trainierter Partien), danach die rohen Tabellen.

Ist TC-Learning aktiv, werden auch die TC-Summen `E` und `A` mitgespeichert
(Flag-Bit 2). Ohne sie würde ein fortgesetztes Training jedes Gewicht wieder
als „noch weit vom Ziel“ behandeln und mit voller Lernrate korrigieren – das
zerstört Gelerntes. (Genau dieser Fehler ist beim ersten Hauptlauf passiert.) Ein
**Endianness-Marker** (`0x01020304`) verhindert, dass eine Datei auf einem
Rechner mit anderer Byte-Reihenfolge falsch gelesen wird. */
#define WEIGHTS_MAGIC "T2048W\0\0"
#define WEIGHTS_VERSION 2u
#define FLAG_VISITS 1u
#define FLAG_TC 2u
#define ENDIAN_MARK 0x01020304u

static int wr(FILE *f, const void *p, size_t n) { return fwrite(p, 1, n, f) == n ? 0 : -1; }
static int rd(FILE *f, void *p, size_t n) { return fread(p, 1, n, f) == n ? 0 : -1; }

int net_save(const net_t *n, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    uint32_t hdr[5] = {WEIGHTS_VERSION, ENDIAN_MARK, (uint32_t)n->n_tuples, (uint32_t)n->n_stages, FLAG_VISITS | (n->tc ? FLAG_TC : 0u)};
    int rc = wr(f, WEIGHTS_MAGIC, 8) | wr(f, hdr, sizeof hdr) | wr(f, &n->games_trained, 8); //@ Fehler werden per | gesammelt – ein einziger Fehlschlag macht rc ≠ 0.
    char name[32] = {0};
    memcpy(name, n->name, sizeof name - 1);
    rc |= wr(f, name, sizeof name);
    for (int s = 0; s < n->n_stages; s++) {
        uint32_t se = (uint32_t)n->stage_exp[s], ready = (uint32_t)n->stage_ready[s];
        rc |= wr(f, &se, 4) | wr(f, &ready, 4);
    }
    for (int t = 0; t < n->n_tuples; t++) {
        uint32_t len = (uint32_t)n->len[t];
        rc |= wr(f, &len, 4) | wr(f, n->cells[t], len);
    }
    for (int s = 0; s < n->n_stages && !rc; s++)
        for (int t = 0; t < n->n_tuples && !rc; t++) {
            rc |= wr(f, n->w[s][t], n->size[t] * sizeof(float)) | wr(f, n->visits[s][t], n->size[t] * sizeof(uint32_t)); //@ Die Tabellen werden roh geschrieben: 4 Byte pro Gewicht, 4 Byte pro Besuchszähler.
            if (n->tc) rc |= wr(f, n->tc_e[s][t], n->size[t] * sizeof(float)) | wr(f, n->tc_a[s][t], n->size[t] * sizeof(float)); //@ TC-Zustand gehört zum Lernstand dazu.
        }
    if (fclose(f)) rc = -1;
    return rc;
}

/*@ ### Gewichte laden

Liest den Kopf, prüft Kennung, Version und Byte-Reihenfolge, baut über den
Preset-Namen dieselbe Netz-Struktur wieder auf und liest dann die Tabellen ein.
Stimmen die gespeicherten Tupel-Zellen nicht mit dem Preset überein, wird
abgebrochen – lieber ein klarer Fehler als ein Netz, das stillschweigend
Unsinn rechnet. */
int net_load(net_t *n, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char magic[8], name[32];
    uint32_t hdr[5];
    uint64_t games;
    if (rd(f, magic, 8) || memcmp(magic, WEIGHTS_MAGIC, 8) || rd(f, hdr, sizeof hdr) || (hdr[0] != 1u && hdr[0] != WEIGHTS_VERSION) ||
        hdr[1] != ENDIAN_MARK || rd(f, &games, 8) || rd(f, name, sizeof name)) {
        fclose(f);
        return -2;
    }
    name[31] = 0;
    int n_stages = (int)hdr[3];
    if (n_stages < 1 || n_stages > MAX_STAGES) {
        fclose(f);
        return -2;
    }
    int stage_exp[MAX_STAGES];
    uint32_t ready[MAX_STAGES];
    for (int s = 0; s < n_stages; s++) {
        uint32_t se;
        if (rd(f, &se, 4) || rd(f, &ready[s], 4)) {
            fclose(f);
            return -2;
        }
        stage_exp[s] = (int)se;
    }
    if (net_create(n, name, n_stages, stage_exp, 0.0f) || (uint32_t)n->n_tuples != hdr[2]) { //@ Gleiche Struktur wie beim Speichern aufbauen; die Werte werden gleich überschrieben.
        fclose(f);
        return -3;
    }
    for (int t = 0; t < n->n_tuples; t++) {
        uint32_t len;
        uint8_t cells[MAX_TUPLE_LEN];
        if (rd(f, &len, 4) || len != (uint32_t)n->len[t] || rd(f, cells, len) || memcmp(cells, n->cells[t], len)) {
            fclose(f);
            net_free(n);
            return -3;
        }
    }
    int has_tc = hdr[0] >= 2u && (hdr[4] & FLAG_TC); //@ Version 1 kannte noch keinen TC-Zustand.
    if (has_tc && net_enable_tc(n)) {
        fclose(f);
        net_free(n);
        return -4;
    }
    for (int s = 0; s < n_stages; s++) {
        n->stage_ready[s] = (int)ready[s];
        for (int t = 0; t < n->n_tuples; t++)
            if (rd(f, n->w[s][t], n->size[t] * sizeof(float)) || rd(f, n->visits[s][t], n->size[t] * sizeof(uint32_t)) ||
                (has_tc && (rd(f, n->tc_e[s][t], n->size[t] * sizeof(float)) || rd(f, n->tc_a[s][t], n->size[t] * sizeof(float))))) {
                fclose(f);
                net_free(n);
                return -4;
            }
    }
    n->games_trained = games;
    fclose(f);
    return 0;
}
