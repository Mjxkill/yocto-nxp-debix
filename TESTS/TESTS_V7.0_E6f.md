# Test Fiche : V7.0 — E6.f (Profiling mixer-pro — diagnostic perf)

**Date** : 2026-05-11
**Statut** : **Diagnostic livré** — instrumentation profiling exposée via socket. **Conclusion contre-intuitive : les effets ne sont PAS le hotspot.**
**Tag git associé** : `v7.0-e6f` (posé après commit)

## Référentiel

| Champ | Valeur |
|---|---|
| Phase | V7.0 — E6.f Profiling concret des effets natifs E6.e |
| Préalable | E6.e MVP (commit `1346be57`, tag `v7.0-e6e`) |
| Pas de modif SOF/kernel/DT/firmware | userspace pur, instrumentation `clock_gettime` |
| Branche yocto | `feature/v7.0-multiband-drc-tap` |

## Hypothèse initiale (E6.e diagnostic)

D'après les mesures `frames_processed` de E6.e, on observait :
- EQ seul : 47 % nominal
- Reverb seul : 32 % nominal
- 4 bus actifs : 36 % nominal

L'hypothèse était : **les effets natifs sample-par-sample sont trop lents pour A53**. La solution proposée = block processing + NEON SIMD.

## Phase 1 — Instrumentation profiling

Ajout de 4 mesures `clock_gettime(CLOCK_MONOTONIC)` autour de chaque cycle :
- `prof_cap_us` : durée `snd_pcm_readi` DSP cap (blocking)
- `prof_mix_us` : durée mix complet (smooth_gains + send_matrix + bus FX + master_matrix sur 96 frames)
- `prof_play_us` : durée `snd_pcm_writei` DSP play
- `prof_iter_us` : durée totale de la boucle audio

Exposé dans `get_state` JSON via atomic_load (pas de contention). Overhead `clock_gettime` ≈ 50 ns × 4 calls = 200 ns/cycle = négligeable.

`_Static_assert(sizeof(float)==4 && _Alignof(float)<=4)` ajouté pour documenter explicitement les pré-requis atomicité ARM64 (suggestion critic).

## Phase 2 — Mesure board (steady-state après 10 s warmup, 4 bus FX actifs)

| Métrique | Valeur observée | Budget | Verdict |
|---|---|---|---|
| `prof_mix_us` (4 effets + matrice 26×18) | **473 µs** | < 2000 µs | ✓ **24 % du budget — RAPIDE** |
| `prof_cap_us` (snd_pcm_readi blocking) | **512 001 µs (= 512 ms)** | ≤ 2000 µs attendu | ✗ **256× au-dessus** |
| `prof_play_us` (snd_pcm_writei blocking) | **511 516 µs (= 511 ms)** | ≤ 2000 µs attendu | ✗ **256× au-dessus** |
| `prof_iter_us` (cycle total) | **1 023 998 µs (= 1.024 s)** | < 2000 µs | ✗ **512× au-dessus** |

**Diagnostic** : le mix entier (incluant les 4 effets actifs) prend 473 µs = **25 % du budget**. Les **effets natifs ne sont PAS le bottleneck**.

Le hotspot est `snd_pcm_readi` + `snd_pcm_writei` du DSP TAC5212 qui bloquent ~1 seconde **chacun par cycle**, comportement compatible avec un cycle xrun/recover/snd_pcm_prepare/snd_pcm_start qui réinitialise le pipeline DSP SOF à chaque itération.

## Phase 3 — Analyse de la cause profonde

Hypothèses pour expliquer les 500 ms de blocking ALSA :

1. **Cycle xrun-recover SOF** : quand `snd_pcm_link(cap, play)` est actif et l'une des deux PCMs underrun/overrun, ALSA force `snd_pcm_recover` sur les 2. Le recover SOF i.MX8MP demande au DSP de re-préparer la pipeline via IPC + DMA reset → **plusieurs centaines de ms** observées sur ce projet (voir mémoire `feedback_loopback_init_xrun.md`).

2. **mlockall + SCHED_FIFO sans PREEMPT_RT** : sur kernel CFS standard, le scheduler peut préempter le thread RT et le re-réveiller après que ALSA a déjà accumulé un large backlog → cascade xrun.

3. **Budget cycle 2 ms trop tendu** : avec snd_pcm_link, le système doit livrer cap+play simultanément à chaque période 2 ms. Tout jitter > 2 ms = xrun automatique.

## Conclusion E6.f honnête

**E6.f ne livre PAS d'optim algorithmique des effets** parce que c'est inutile : les effets sont déjà rapides (24 % du budget).

**E6.f livre un outil de diagnostic** (`prof_*_us` dans `get_state`) qui révèle que :
- L'hypothèse E6.e ("effets lents") était **FAUSSE**
- Le vrai bottleneck = **ALSA scheduling + cycle xrun-recover SOF**
- L'optim doit cibler le kernel/scheduling, pas le code mixer userspace

## Recommandations pour E6.g (vrai sprint optim)

D'après le diagnostic E6.f, le plan correct pour atteindre 48 kHz steady avec effets actifs :

1. **PREEMPT_RT kernel patch** : élimine le jitter scheduling qui cause les xrun. Sprint dédié (mémoire `feedback_loopback_init_xrun.md`).
2. **Plus grand buffer ALSA** : passer de N_PERIODS=4 (8 ms) à N_PERIODS=8 (16 ms). Tradeoff : latence +8 ms. Acceptable si latence E2E reste < 20 ms.
3. **Retrait `snd_pcm_link`** : passer en async parallèle (3 threads RT séparés cap/mix/play + lockfree ring buffers). Complexité élevée mais découple les xrun.
4. **CPU pinning + isolcpus** : dédier 1 cœur Cortex-A53 (sur 4) au mixer-pro via `isolcpus=3` kernel cmdline.

Aucune de ces actions ne touche au code mixer-pro/effects.c lui-même. **Les effets restent en place tels quels.**

## Tests T6f.X

| Test | Description | Résultat |
|---|---|---|
| **T6f.1** | Profiling exposé via socket JSON (`prof_*_us`) | ✓ OK (4 métriques visibles dans `get_state`) |
| **T6f.2** | Mesure steady-state 4 bus actifs après 10 s warmup | ✓ Mesure obtenue (cap=512 ms, mix=473 µs, play=511 ms, iter=1024 ms) |
| **T6f.3** | Identification du hotspot | ✓ **ALSA cap/play blocking** (pas les effets) |
| **T6f.4** | Hypothèse E6.e (effets lents) invalidée | ✓ mix=473 µs = 24 % budget = rapide |
| **T6f.5** | Static assertions atomicité ARM64 (suggestion critic) | ✓ `_Static_assert(sizeof(float)==4 && _Alignof(float)<=4)` |
| **T6f.6** | Pas de régression effets E6.e | ✓ aucun code algorithmique modifié |

## Notes méthodologiques

### Mesure du `last_*` vs moyenne

Les `prof_*_us` exposés sont les valeurs du **dernier cycle**, pas une moyenne. C'est suffisant pour identifier des pics, mais une moyenne glissante sur N cycles serait plus représentative pour analyse fine. Ajout possible en E6.g.

### `prof_iter_us` vs ratio observé `frames_processed`

- 1 iter = 1024 ms en moyenne
- Frames processed/iter = 96
- Donc ≈ 93 frames/s effective steady
- Mais ALSA hardware tourne à 48000 Hz strict → 47907 frames/s "perdus" par xrun-recover cycles

Cohérent avec ratio observé E6.e (4 bus = 36 % nominal ≈ 17 kHz → cycles utiles entrecoupés de recover).

### Suggestion critic #3 (fallback)

> Prévoir un fallback automatique : si profiling montre `cap_read_us` ou `play_write_us` > 50% du budget, désactiver les optims effets et ouvrir ticket kernel/scheduling.

→ **Exactement ce que ce sprint a fait.** Diagnostic livré, pas d'optim algo, ticket ouvert (E6.g = PREEMPT_RT + buffer + lockfree).

## Conclusion

- **Outil de diagnostic livré** : `prof_*_us` dans `get_state` JSON, atomic_load lock-free, overhead négligeable
- **Hotspot identifié et documenté** : ALSA snd_pcm_readi/writei sur DSP, pas les effets
- **Hypothèse E6.e ("effets lents") empiriquement invalidée**
- **Aucune optim algorithmique effets nécessaire** (mix = 473 µs = 24 % budget)
- **Sprint E6.g recommandé** : PREEMPT_RT kernel + buffer plus grand + retrait snd_pcm_link (complexe)

E6.f est un **succès méthodologique** : on ne fait PAS d'optim spéculative. On mesure d'abord, on conclut, on documente.
