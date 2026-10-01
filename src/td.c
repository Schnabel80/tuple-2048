/*@ ## td.c – Temporal-Difference-Learning

**Die Grundidee in einem Satz:** Der Agent sagt vor jedem Zug voraus, wie viele
Punkte er noch holen wird – und korrigiert diese Vorhersage, sobald er einen
Schritt weiter ist und es besser weiß.

Er braucht dafür **kein Spielende abzuwarten** und **keinen Lehrer**: Die
Vorhersage von *jetzt* wird an der Vorhersage von *gleich* gemessen (plus den
Punkten dazwischen). Diese Differenz zwischen zwei zeitlich benachbarten
Vorhersagen heißt **TD-Fehler** (Temporal Difference). Am Anfang sind alle
Vorhersagen Unsinn – aber am Spielende ist die Wahrheit bekannt (danach kommen
0 Punkte), und von dort sickert echtes Wissen Schritt für Schritt nach vorne.

**Afterstates:** Wir bewerten nicht das Feld *vor* dem Zug, sondern das Feld
**direkt nach dem Verschieben, bevor die neue Kachel erscheint** (*afterstate*).
Vorteil: Das Ergebnis eines Zuges ist bis dahin vollständig vorhersehbar. Der
Agent muss nicht über alle möglichen neuen Kacheln mitteln, um einen Zug zu
bewerten – er schiebt einfach in alle vier Richtungen und schaut nach, welches
Ergebnis die Tabelle am besten findet. */
#include "td.h"

#include <stdlib.h>

/*@ ### Den besten Zug finden

Für jede der vier Richtungen: verschieben, die sofortigen Punkte `r` notieren
und das entstehende Feld bewerten. Der **Zugwert** ist `q = r + V(afterstate)`:
*Punkte jetzt* plus *erwartete Punkte danach*. Der Zug mit dem höchsten `q`
gewinnt.

Kein Zufall, kein Ausprobieren (*greedy*). Das klingt riskant, funktioniert
bei 2048 aber gut: Das Spiel selbst ist zufällig genug, sodass der Agent
ohnehin ständig neue Situationen erlebt. */
int td_best_move(const net_t *n, board_t b, board_t *after, uint32_t *reward, float q[N_DIRS], int valid[N_DIRS]) {
    int best = -1;
    float best_q = 0.0f;
    for (int d = 0; d < N_DIRS; d++) {
        uint32_t r;
        board_t a = board_move(b, d, &r);
        int ok = a != b; //@ Hat sich nichts bewegt, ist der Zug verboten.
        float v = ok ? (float)r + net_value(n, a) : 0.0f; //@ q = Punkte jetzt + Bewertung des Feldes danach.
        if (q) q[d] = v;
        if (valid) valid[d] = ok;
        if (ok && (best < 0 || v > best_q)) {
            best = d;
            best_q = v;
            *after = a;
            *reward = r;
        }
    }
    return best; //@ −1 heißt: kein gültiger Zug mehr – Spiel vorbei.
}

static void path_push(path_t *p, board_t after, uint32_t reward) {
    if (p->len == p->cap) {
        p->cap = p->cap ? 2 * p->cap : 4096; //@ Platz verdoppeln: so sind nur wenige realloc-Aufrufe nötig, auch bei Partien mit 20 000 Zügen.
        p->steps = realloc(p->steps, p->cap * sizeof *p->steps);
        if (!p->steps) abort();
    }
    p->steps[p->len++] = (step_t){after, reward};
}

/*@ ### Eine Trainingspartie

**Phase 1 – Spielen:** Der Agent spielt mit seinem aktuellen Wissen eine
komplette Partie und merkt sich jedes Afterstate samt der Punkte des Zuges,
der dorthin geführt hat.

**Phase 2 – Rückblickend lernen:** Wir gehen die Partie **vom Ende zum Anfang**
durch. Für das letzte Afterstate ist das Ziel 0 (danach kam nichts mehr). Für
jedes frühere Afterstate ist das Ziel: *Punkte des nächsten Zuges + (gerade
verbesserter) Wert des nächsten Afterstates*.

```
Ziel(a_t)  = r_{t+1} + V(a_{t+1})       (am Ende: 0)
Fehler     = Ziel − V(a_t)
V(a_t)    += α · Fehler                  (verteilt auf alle Tupel)
```

Warum rückwärts? Vorwärts würde eine Erkenntnis vom Spielende pro Partie nur
**einen** Schritt nach vorne wandern. Rückwärts läuft sie in **einer** Partie
durch die ganze Kette – das Lernen wird dadurch deutlich schneller, bei
gleicher Rechenarbeit. */
game_result_t td_train_game(net_t *n, rng_t *r, path_t *path) {
    game_result_t res = {0, 0, 0};
    path->len = 0; //@ Den Speicher der letzten Partie wiederverwenden – nur den Füllstand zurücksetzen.
    board_t b = board_new_game(r);
    for (;;) {
        board_t after;
        uint32_t reward;
        if (td_best_move(n, b, &after, &reward, NULL, NULL) < 0) break; //@ Kein gültiger Zug mehr → Partie vorbei.
        if (n->n_stages > 1) net_ensure_stage(n, after); //@ Multi-Stage: neue Spielphase erreicht? Dann Tabelle befördern.
        path_push(path, after, reward); //@ Merken für Phase 2.
        res.score += reward;
        b = board_spawn(after, r, NULL, NULL); //@ Zufall: die neue Kachel erscheint.
    }
    res.moves = (uint32_t)path->len;
    res.max_exp = board_max_exp(b);

    float target = 0.0f; //@ Nach dem letzten Zug kommen 0 Punkte – das einzige, was der Agent sicher weiß.
    for (size_t i = path->len; i-- > 0;) { //@ Rückwärts: vom letzten Zug bis zum ersten.
        const step_t *s = &path->steps[i];
        float err = target - net_value(n, s->after); //@ TD-Fehler: wie sehr lag die Vorhersage daneben?
        float v_new = net_update(n, s->after, err);  //@ Gewichte in Richtung Ziel schieben.
        target = (float)s->reward + v_new;           //@ Ziel für das vorherige Afterstate: Punkte dieses Zuges + neuer Wert.
    }
    return res;
}

/*@ ### Eine Vorführpartie

Wie eine Trainingspartie, aber **ohne Lernen** – das Wissen ist eingefroren.
Optional mit Vorausschau (`depth > 0`, siehe `search.c`). Nach jedem Zug wird
der Callback mit allen Details aufgerufen: Feld vorher/nachher, gewählte
Richtung, die `q`-Werte **aller** vier Richtungen und die neue Kachel. Daraus
entsteht die Replay-Datei, mit der die Webseite zeigt, *warum* der Agent
welchen Zug gewählt hat. */
game_result_t td_play_game(const net_t *n, rng_t *r, int depth, move_cb cb, void *ctx) {
    game_result_t res = {0, 0, 0};
    board_t b = board_new_game(r);
    for (;;) {
        move_info_t mi;
        mi.before = b;
        int d = search_best_move(n, b, depth, &mi.after, &mi.reward, mi.q, mi.valid); //@ Mit depth = 0 identisch zu td_best_move, sonst mit Vorausschau.
        if (d < 0) break;
        res.score += mi.reward;
        mi.t = ++res.moves;
        mi.dir = d;
        mi.score = res.score;
        mi.next = board_spawn(mi.after, r, &mi.spawn_pos, &mi.spawn_exp); //@ Die neue Kachel wird mitprotokolliert – damit lässt sich die Partie exakt nachspielen.
        if (cb) cb(ctx, &mi); //@ Zug an den Aufzeichner übergeben (z.B. io_replay_cb).
        b = mi.next;
    }
    res.max_exp = board_max_exp(b);
    return res;
}
