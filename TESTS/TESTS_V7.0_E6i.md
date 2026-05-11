# Test Fiche : V7.0 — E6.i (mixer-pro — drainage agressif + N_PERIODS=2)

**Date** : 2026-05-11
**Statut** : **EN ATTENTE BOARD** — code commité, build en cours, mesures à valider
**Tag git associé** : `v7.0-e6i` (à créer après validation)

## Référentiel

| Champ | Valeur |
|---|---|
| Phase | V7.0 — E6.i Drainage agressif play_thread + N_PERIODS 4→2 |
| Préalable | E6.h (commit `add54d8d`, tag `v7.0-e6h`) — eventfd + ring 8 périodes |
| Branche yocto | `feature/v7.0-multiband-drc-tap` |
| Pas de modif SOF/kernel/DT/firmware | userspace pur |
| Investigation critic | job `55f42bac-b5ff-417c-af86-83a55772d27e` (5/6 workers, convergence forte) |

## Cause racine identifiée (5/5 workers critic)

> Le ring SPSC stabilise à 6-8 ms steady parce que `play_thread` consomme exactement
> 1 période par signal eventfd. Production audio_thread (48 kHz) = consommation
> play_thread (48 kHz) → équilibre dynamique : le ring ne se vide jamais sous son
> niveau initial. Le `snd_pcm_writei` blocking entretient cet équilibre.

## Travaux exécutés

### Changement 1 — boucle de drainage interne play_thread

`mixer-pro.c:493-554` — remplacement du `pop 1 période / cycle` par une boucle interne
qui consomme TOUT le ring tant qu'ALSA accepte, jusqu'à `MAX_DRAIN_PERIODS=4` consécutives.

```c
while (running) {
    read(eventfd);                     // bloque jusqu'au prochain push
    for (drained < MAX_DRAIN_PERIODS) {
        if (ring_avail < PERIOD) break;
        pop 1 période → writei blocking;
        if (recover) break;
    }
}
```

ALSA `writei` blocking régule mécaniquement à 48 kHz. Le tampon ring est transféré
dans le buffer ALSA SANS ajouter de latence, et l'équilibre dynamique est cassé.

Garde `MAX_DRAIN_PERIODS=4` (8 ms max consécutif) pour éviter starvation audio_thread.

### Changement 2 — N_PERIODS 4 → 2

`mixer-pro.h:60` — buffer ALSA 8 ms → 4 ms. Le ring SPSC (16 ms) absorbe le jitter
scheduler PREEMPT (~100 µs).

### Changement 3 — signal eventfd post-prefill ring

`mixer-pro.c:344-347` — déclenche le drainage de la période de silence du prefill
immédiatement, sans attendre le 1er push audio_thread. Évite 1 période d'asymétrie au
démarrage.

### Changement 4 — bump version

`MIXER_VERSION "v7.0-e6h" → "v7.0-e6i"` dans `mixer-pro.h:29`.

## Tests T6i.X — à compléter board

| Test | Description | Cible | Résultat |
|---|---|---|---|
| **T6i.1** | Build OK clean warnings | warnings = 0 | TODO |
| **T6i.2** | Démarrage 3 threads RT, eventfd créé | log `play_thread SCHED_FIFO prio 81` | TODO |
| **T6i.3** | Throughput steady 30 s 4 bus FX actifs | ≥ 48 kHz nominal | TODO |
| **T6i.4** | `ring_fill_frames` steady | < 96 (cible 0-96) | TODO |
| **T6i.5** | `play_delay_frames` steady | 96-192 (avec N_PERIODS=2) | TODO |
| **T6i.6** | `latency_us_one_way` ALSA only | ≤ 4000 µs | TODO |
| **T6i.7** | Latence mixer totale (ALSA + ring) | ≤ 8 ms | TODO |
| **T6i.8** | `xrun` delta sur 30 s | 0 | TODO |
| **T6i.9** | `ring_drops` bornés à init burst, stable après | bornés init | TODO |
| **T6i.10** | Régression matrice/sends/effets/4 bus FX | tests E6.d/e/g passent | TODO |
| **T6i.11** | Shutdown propre (SIGINT) | pthread_join() OK | TODO |
| **T6i.12** | Latence E2E acoustique loopback (impulsion) | < 12 ms | TODO |
| **T6i.13** | `prof_iter_us` audio_thread pendant drain max | < 2000 µs (pas de starvation) | TODO |

## Mesures empiriques détaillées

À compléter board.

```json
{
  "version": "v7.0-e6i",
  "frames": "...",
  "xrun": "...",
  "cap_delay_frames": "...",
  "play_delay_frames": "...",
  "latency_us_one_way": "...",
  "prof_mix_us": "...",
  "prof_play_us": "...",
  "prof_iter_us": "...",
  "ring_drops": "...",
  "ring_fill_frames": "..."
}
```

## Latence end-to-end projetée

| Étage | Avant E6.h | Cible E6.i |
|---|---|---|
| TAC5212 ADC | ~0.5 ms | ~0.5 ms |
| DMA SAI RX | 2 ms | 2 ms |
| DSP cap pipeline | ~0.5 ms | ~0.5 ms |
| **mixer-pro ring SPSC** | **6-8 ms** | **0-2 ms** |
| **mixer-pro ALSA play buffer** | **6-8 ms** | **2-4 ms** |
| DSP play pipeline | ~0.5 ms | ~0.5 ms |
| DMA SAI TX | 2 ms | 2 ms |
| TAC5212 DAC | ~0.5 ms | ~0.5 ms |
| **TOTAL E2E estimé** | **~20 ms** | **~10-12 ms** |

**Bénéfice E6.i vs E6.h : -6 à -8 ms** mixer côté.

Cible V7.0 < 10 ms acoustique : très proche, validation loopback acoustique nécessaire (T6i.12).
Si > 10 ms, sprint E6.j envisagé (CPU pinning + isolcpus).

## Risques surveillés

1. `N_PERIODS=2` → xrun éventuels sous jitter > 2 ms (mitigation : ring SPSC absorbe)
2. Drainage agressif → starvation audio_thread (mitigation : `MAX_DRAIN_PERIODS=4`)
3. Recover SOF 60 ms + N_PERIODS=2 → overflow ring possible si recover après warmup
   (mitigation : E6.h démontre recover = init only ; monitor `ring_drops` après warmup)

## Optims futures possibles (E6.j si E6.i insuffisant)

- CPU pinning : `taskset -c 3 mixer-pro` ou `pthread_setaffinity_np`
- Isolcpus kernel cmdline : `isolcpus=3 nohz_full=3 rcu_nocbs=3`
- NONBLOCK + `snd_pcm_avail_update` : drainage encore plus tight (mais +30 LOC)

## Conclusion

(à compléter post-board)
