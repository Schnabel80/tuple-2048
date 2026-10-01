/*@ ## train.h – Einstellungen eines Trainingslaufs

Alle Stellschrauben eines Trainings an einem Ort. Die Kommandozeile (`main.c`)
füllt diese Struktur, `train.c` arbeitet sie ab. */
#ifndef T2048_TRAIN_H
#define T2048_TRAIN_H

#include <stdint.h>

#include "ntuple.h"

typedef struct {
    const char *net;          //@ Preset: small | strong | strong8.
    const char *out;          //@ Ausgabeordner.
    const char *resume;       //@ Optional: Gewichte, mit denen weitertrainiert wird.
    uint64_t games;           //@ Ziel: Gesamtzahl trainierter Partien (inkl. Resume).
    int threads;              //@ Parallel spielende Threads (Hogwild).
    float alpha;              //@ Lernrate (Standard 0,1).
    float init;               //@ OTD-Startwert pro Feld (0 = klassisch).
    int tc;                   //@ TC-Learning an?
    uint64_t tc_after;        //@ TC erst ab dieser Partienzahl einschalten.
    float tc_alpha;           //@ Lernrate ab TC-Start (Standard 1,0 – TC bremst selbst).
    int n_stages;
    int stage_exp[MAX_STAGES];
    uint64_t every;           //@ Statistikzeile alle N Partien.
    uint64_t seed;
    uint64_t demo_seed;       //@ Fester Seed für die Vorführpartien → vergleichbare Replays.
    int demo_depth;
    uint64_t eval_games;      //@ Zusätzliche Bewertungs-Partien mit eingefrorenem Netz pro Snapshot.
    int top_k;
    uint32_t min_visits;
    int snapshot_weights;     //@ Gewichte an jedem Snapshot separat aufbewahren.
} train_cfg_t;

void train_cfg_defaults(train_cfg_t *c);
int train_run(const train_cfg_t *c);

#endif
