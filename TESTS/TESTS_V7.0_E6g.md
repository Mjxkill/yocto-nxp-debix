# Test Fiche : V7.0 — E6.g (mixer 2-thread + ring SPSC, sans snd_pcm_link)

**Date** : 2026-05-11
**Statut** : **GO** — mixer-pro tient 48 kHz nominal avec 4 bus FX actifs, 0 xrun steady. Latence E2E mixer ~66 ms (trade-off du ring tampon).
**Tag git associé** : `v7.0-e6g`

## Référentiel

| Champ | Valeur |
|---|---|
| Phase | V7.0 — E6.g refactor mixer-pro : retrait `snd_pcm_link` + 2 threads (audio cap+mix / play DSP) + ring SPSC |
| Préalable | E6.f (diagnostic profiling, hotspot identifié = snd_pcm_link + cycles xrun-recover ~500 ms) |
| Branche yocto | `feature/v7.0-multiband-drc-tap` |
| Pas de modif SOF/kernel/DT/firmware | userspace pur |

## Phases et résultats

| Phase | Modif | Résultat | Verdict |
|---|---|---|---|
| **P1** | Retrait `snd_pcm_link` | recover passe de 500 ms à ~60 ms par cycle, mais 16 xrun/s | Insuffisant |
| **P1.5** | `N_PERIODS=4→8` (buffer 16 ms) | Pas d'amélioration (le recover SOF est intrinsèque, pas un manque de marge) | Inutile seul |
| **P2** | 2 threads + ring SPSC 32 périodes | **48 288 Hz steady, 0 xrun, 0 drop** après init | ✓ GO |
| **P2 tuned** | N_PERIODS=4 + prefill ring 2 (au lieu de 4) | Idem perfs, latence ALSA 16→8 ms | ✓ Final |

## Architecture E6.g Phase 2

```
┌─────────────────────────────────────────────────────────────┐
│ Thread audio (SCHED_FIFO prio 80)                            │
│   ┌──────────────────────────────────────────────────────┐  │
│   │ loop {                                                │  │
│   │   snd_pcm_readi DSP cap (blocking 2 ms — master clk) │  │
│   │   snd_pcm_readi UAC2/Phone (NONBLOCK)                │  │
│   │   smooth_gains + mix (473 µs : 4 effets + matrices)  │  │
│   │   push ring SPSC (atomic, ~2 µs)                     │  │
│   │   snd_pcm_writei UAC2/Phone (NONBLOCK)               │  │
│   │ }                                                     │  │
│   └──────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
                              │ ring SPSC (32 périodes × 96 × 8 ch)
                              ▼
┌─────────────────────────────────────────────────────────────┐
│ Thread play DSP (SCHED_FIFO prio 81)                         │
│   ┌──────────────────────────────────────────────────────┐  │
│   │ loop {                                                │  │
│   │   pop ring SPSC (atomic)                             │  │
│   │   snd_pcm_writei DSP play (blocking, peut bloquer    │  │
│   │     ~60 ms sur recover mais ring absorbe)            │  │
│   │ }                                                     │  │
│   └──────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
```

### Ring SPSC lockfree

- Taille : 32 périodes × 96 frames × 8 ch × 4 bytes = **221 KB** (en RAM A53)
- 2 indices `atomic_uint write_idx`, `atomic_uint read_idx` (32-bit aligned, atomic sur ARM64)
- Single Producer (audio_thread push) / Single Consumer (play_thread pop)
- Pas de mutex, pas de cond_var. Wait via `nanosleep(1 ms)` quand ring vide côté pop.
- **Drop policy** : si ring full au push, on avance `read_idx` pour écraser les plus vieux samples. Compteur `ring_drops` exposé via socket.

### Pourquoi 32 périodes ?

Le `snd_pcm_recover` sur DSP play peut bloquer ~60 ms (mesuré E6.f). Pendant ce blocage, le thread audio continue à push à 48 kHz = 2880 frames en 60 ms = 30 périodes. Le ring de 32 périodes absorbe ce cycle sans drop.

## Tests T6g.X

| Test | Description | Résultat | Critère GO |
|---|---|---|---|
| **T6g.1** | Build OK avec ring + play_thread | ✓ OK | warnings clean |
| **T6g.2** | Démarrage : 3 threads (audio, play, control) avec SCHED_FIFO appliqués | ✓ OK (log "play_thread : SCHED_FIFO prio 81", idem audio prio 80) | RT priorities OK |
| **T6g.3** | Throughput steady 4 bus FX actifs | ✓ **48 288 Hz** (100.6 % nominal sur 3 s) | ≥ 80 % nominal — DÉPASSÉ |
| **T6g.4** | Delta xrun sur 3 s steady | ✓ **0** | 0 |
| **T6g.5** | `ring_drops` après init | ✓ 2400 init (= 25 frames stables), puis 0 stable | drops bornés à init |
| **T6g.6** | Latence ALSA `cap_delay + play_delay` | ✓ **8 ms** (96 + 288 frames) | < 10 ms côté ALSA — ATTEINT |
| **T6g.7** | `ring_fill_frames` stable steady | 2784 frames = **58 ms** dans le ring | ⚠️ élevé (artefact d'équilibre) |
| **T6g.8** | 0 régression fonctionnelle (matrice, sends, effets) | ✓ OK | tests E6.d/E6.e/E6.e repassent |

## Mesures empiriques détaillées

State JSON après warmup 5 s, 4 bus FX actifs :
```json
{
  "frames": 425952,            // delta 3s = 144864 → 48 288 Hz
  "xrun": 2,                   // pas d'augmentation
  "cap_delay_frames": 96,      // 1 période backlog (normal)
  "play_delay_frames": 288,    // 3 périodes ALSA buffer = 6 ms
  "latency_us_one_way": 8000,  // 8 ms ALSA cap+play
  "prof_cap_us": 1550,         // 1.55 ms attente cap (≈ 2 ms attendu)
  "prof_mix_us": 489,          // mix 4 effets + matrices = 0.5 ms
  "prof_play_us": 2,           // push ring atomic = 2 µs (vs 60 ms avant)
  "prof_iter_us": 2041,        // total iter = 2 ms = budget
  "ring_drops": 2400,          // burst init, 0 après
  "ring_fill_frames": 2784     // 58 ms tampon stable
}
```

## Latence E2E

| Étage | Latence | Notes |
|---|---|---|
| TAC5212 ADC | ~0.5 ms | datasheet linear-phase |
| DMA SAI RX | 2 ms | period |
| DSP cap pipeline | ~0.5 ms | inline |
| **mixer-pro audio_thread cap_delay** | ~2 ms | cap_delay=96 frames |
| **mixer-pro ring SPSC tampon** | **58 ms** | ring_fill=2784 frames steady |
| **mixer-pro play_thread + ALSA play** | ~6 ms | play_delay=288 frames |
| DSP play pipeline | ~0.5 ms | inline |
| DMA SAI TX | 2 ms | period |
| TAC5212 DAC | ~0.5 ms | linear-phase |
| **TOTAL E2E estimé** | **~72 ms** | |

Latence ALSA mesurable (`latency_us_one_way` exposé) = **8 ms**. Latence ring (calculée depuis `ring_fill_frames`) ajoute ~58 ms.

**Au-dessus de la cible V7.0 < 10 ms acoustique** — c'est le compromis du design 2-thread + ring qui découple le recover DSP des cycles cap. Sans ce ring, le mixer ne tenait pas la cadence (E6.f montrait 1024 ms par iter).

## Optimisations futures (E6.h non couvert)

Le ring stabilise à 58 ms parce que :
1. Au démarrage, l'audio_thread produit (cap accumulé) avant que play_thread soit prêt → ring se remplit
2. En steady, production = consommation 48 kHz, donc le niveau ne baisse plus

**Optimisations possibles** (sprint séparé) :
- Remplacer `nanosleep(1 ms)` du play_thread par `eventfd` ou `cond_var` signalé par le push → drainage immédiat sans polling
- Drain agressif au démarrage : play_thread consomme plusieurs périodes d'affilée jusqu'à ring_fill < seuil
- Réduire `N_RING_PERIODS` (mais risque de drops si recover > taille ring)

Cible optim : ring_fill steady < 192 frames (4 ms) → latence E2E ~16 ms.

## Conclusion

- **Architecture 2-thread + ring SPSC LIVRÉE** : audio_thread (cap+mix) découplé de play_thread (DSP play)
- **48 kHz steady atteint** avec 4 bus FX actifs et 0 xrun
- `snd_pcm_link` retiré, élimine les recover en cascade
- **Latence E2E mixer ~66 ms** (8 ms ALSA + 58 ms ring) — trade-off du design, optim possible E6.h
- **0 régression** matrice/sends/effets
- Tag `v7.0-e6g` posé

E6.g est le **vrai breakthrough** : le mixer fonctionne au débit nominal avec tous les effets actifs, débloquant la suite (E7 GUI).
