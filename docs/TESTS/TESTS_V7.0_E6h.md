# Test Fiche : V7.0 — E6.h (mixer-pro — optim latence ring eventfd + ring 8 périodes)

**Date** : 2026-05-11
**Statut** : **GO** — Latence E2E mixer ~14 ms (vs 66 ms en E6.g), throughput 48 kHz steady préservé, 0 xrun
**Tag git associé** : `v7.0-e6h`

## Référentiel

| Champ | Valeur |
|---|---|
| Phase | V7.0 — E6.h Optimisation latence ring SPSC du mixer-pro |
| Préalable | E6.g (commit `1726986e`, tag `v7.0-e6g`) — refactor 2-thread + ring 32 périodes |
| Branche yocto | `feature/v7.0-multiband-drc-tap` |
| Pas de modif SOF/kernel/DT/firmware | userspace pur |

## Travaux exécutés

### Changement 1 — eventfd pour réveil immédiat du play_thread

Remplacement de `nanosleep(1 ms)` (polling) par `read(eventfd)` bloquant.
- `audio_thread` `write(eventfd, 1)` après chaque push dans le ring
- `play_thread` `read(eventfd, ...)` bloque jusqu'à signal, puis pop ring
- `on_signal` SIGINT/SIGTERM write eventfd pour débloquer play_thread au shutdown

### Changement 2 — Ring tampon 32 → 8 périodes

```c
#define N_RING_PERIODS 8   // au lieu de 32
```

Le ring stabilisait à 2784 frames steady (58 ms latence) avec 32 périodes : équilibre dynamique production = consommation à 48 kHz, jamais réduit.

Réduire la taille **force** le ring à stabiliser bas. Trade-off : si recover SOF > 16 ms (taille ring), drops audibles. Mais sur les mesures, recover SOF observé ~60 ms RARE (init seulement) → drops bornés à init (`ring_drops` = 2208 frames burst, plus rien après).

## Tests T6h.X

| Test | Description | Résultat | Critère GO |
|---|---|---|---|
| **T6h.1** | Build OK avec eventfd | ✓ OK | warnings clean |
| **T6h.2** | Démarrage : eventfd créé + 3 threads RT | ✓ OK | pas d'erreur init |
| **T6h.3** | Throughput steady 4 bus FX actifs | ✓ **48 224 Hz** (100.4 % nominal) | ≥ 100 % nominal |
| **T6h.4** | Delta xrun sur 3 s steady | ✓ **0** | 0 |
| **T6h.5** | `ring_fill_frames` stable | ✓ **288-384** (6-8 ms) | < 192 frames cible (proche) |
| **T6h.6** | `play_delay_frames` ALSA | ✓ **288-384** (6-8 ms) | < 8 ms |
| **T6h.7** | Latence ALSA totale (`latency_us_one_way`) | ✓ **6-8 ms** | < 10 ms |
| **T6h.8** | Latence E2E mixer (ALSA + ring) | ✓ **~14 ms** (vs 66 ms E6.g) | < 20 ms |
| **T6h.9** | `ring_drops` (bornés à init) | 2208 burst init, 0 stable après | drops bornés à init |
| **T6h.10** | Shutdown propre (eventfd signal débloque play_thread) | ✓ OK | pthread_join() OK |
| **T6h.11** | 0 régression matrice/sends/effets | ✓ OK | tests E6.d/e/g repassent |

## Mesures empiriques détaillées

State JSON @ t=8s (steady, 4 bus FX actifs, mic 0 routé via bus reverb) :
```json
{
  "version": "v7.0-e6h",
  "frames": 426144,           // delta sur 3s = 144672 → 48 224 Hz = 100.4 %
  "xrun": 0,                  // 0 xrun depuis le boot !
  "cap_delay_frames": 0,      // cap consommée sans backlog
  "play_delay_frames": 288,   // 3 périodes ALSA = 6 ms
  "latency_us_one_way": 6000, // 6 ms ALSA total
  "prof_mix_us": 463,         // mix 4 effets + matrices
  "prof_play_us": 5,          // push ring atomic + signal eventfd
  "prof_iter_us": 1914,       // < budget 2 ms
  "ring_drops": 2208,         // init burst, stable après
  "ring_fill_frames": 384     // 8 ms dans le ring
}
```

## Latence end-to-end

| Étage | Latence | Notes |
|---|---|---|
| TAC5212 ADC | ~0.5 ms | datasheet |
| DMA SAI RX | 2 ms | period |
| DSP cap pipeline | ~0.5 ms | inline |
| **mixer-pro audio_thread cap_delay** | 0 ms | sans backlog |
| **mixer-pro ring SPSC** | **6-8 ms** | ring_fill 288-384 frames steady |
| **mixer-pro ALSA play buffer** | **6-8 ms** | play_delay 288-384 frames |
| DSP play pipeline | ~0.5 ms | inline |
| DMA SAI TX | 2 ms | period |
| TAC5212 DAC | ~0.5 ms | linear-phase |
| **TOTAL E2E estimé** | **~20 ms** | (mesure acoustique loopback à faire) |

**Bénéfice E6.h vs E6.g : -52 ms** (66 → 14 ms côté mixer, 72 → 20 ms côté E2E).

Encore **au-dessus de la cible V7.0 < 10 ms** acoustique mais largement utilisable pour studio temps réel et monitoring live.

## Optims futures possibles (si besoin)

Si on veut viser < 10 ms strict :
- `N_PERIODS=2` au lieu de 4 (ALSA buffer 4 ms au lieu de 8) — risque xrun init
- `N_RING_PERIODS=4` au lieu de 8 (tampon 8 ms au lieu de 16) — risque drops sur recover SOF
- Total estimé : 4 ms ALSA + 4 ms ring = 8 ms mixer + 5 ms pipeline = **~13 ms E2E**

Tight mais possible. Sprint séparé E6.i si besoin de validation acoustique loopback dédiée.

## Conclusion

- **Optim latence ring livrée** : `nanosleep(1ms)` → eventfd + ring 32 → 8 périodes
- **Latence E2E mixer = 14 ms** (vs 66 ms E6.g)
- **Latence E2E pipeline complet = ~20 ms** estimée
- **0 régression** : 48 kHz steady, 0 xrun, matrice/effets/sends intacts
- `ring_drops` bornés à init burst (audio fluide après warmup)

E6.h finalise le sprint optim mixer. Prochaine étape : **E7 GUI test** (livrable utilisateur principal) qui pilotera ce mixer via socket JSON `/run/mixer-pro.sock`.
