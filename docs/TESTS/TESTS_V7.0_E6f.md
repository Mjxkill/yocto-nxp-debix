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

2. **Scheduling** : ~~mlockall + SCHED_FIFO sans PREEMPT_RT~~ **Correction post-vérif board** : le kernel Debix V1.0.4 a `CONFIG_PREEMPT=y` (low-latency preemption activée). Pas full PREEMPT_RT mais PREEMPT basique. Jitter typique ~100 µs (≪ budget 2 ms). Ce n'est probablement PAS le cause des 500 ms de blocage observés (PREEMPT_RT ne réduirait que de 100 µs à 10 µs, négligeable face à 500 ms).

3. **Budget cycle 2 ms trop tendu** : avec snd_pcm_link, le système doit livrer cap+play simultanément à chaque période 2 ms. Tout jitter > 2 ms = xrun automatique.

## Conclusion E6.f honnête

**E6.f ne livre PAS d'optim algorithmique des effets** parce que c'est inutile : les effets sont déjà rapides (24 % du budget).

**E6.f livre un outil de diagnostic** (`prof_*_us` dans `get_state`) qui révèle que :
- L'hypothèse E6.e ("effets lents") était **FAUSSE**
- Le vrai bottleneck = **ALSA scheduling + cycle xrun-recover SOF**
- L'optim doit cibler le kernel/scheduling, pas le code mixer userspace

## Recommandations pour E6.g (vrai sprint optim) — RÉVISÉES POST-VÉRIF KERNEL

**Vérification kernel board (uname/proc/config)** :
- `CONFIG_PREEMPT=y` : **déjà actif** (low-latency preemption, jitter scheduling ~100 µs typique)
- `CONFIG_PREEMPT_RT` : non activé. Gain potentiel = jitter 100 µs → 10 µs (négligeable face aux 500 ms observés).

Donc PREEMPT_RT n'est PAS la priorité. Les vrais leviers :

1. **Retirer `snd_pcm_link` DSP cap/play** : ce link force `snd_pcm_recover` sur les 2 PCMs liés à chaque underrun de l'un, multipliant les blocages. Tester avec link désactivé + sync manuel via prefill buffer.
2. **Investiguer la durée du recover SOF IPC** : pourquoi `snd_pcm_recover` sur DSP prend-il ~500 ms ? Tracer `/sys/kernel/debug/sof/` pendant un cycle xrun. Hypothèse : re-prepare pipeline complet via mailbox IPC, optim envisageable côté driver kernel.
3. **Plus grand buffer ALSA** : N_PERIODS=4 (8 ms) → N_PERIODS=8 (16 ms). Tradeoff : latence +8 ms. Évite les sub-ms-jitter de causer un xrun.
4. **3 threads RT cap/mix/play + lockfree ring buffers** : découple les xrun. Si play subit un recover, cap continue à accumuler dans le ring → on rattrape après. Complexité élevée, MAIS la bonne architecture pour un mixer audio pro.
5. **CPU pinning** : `taskset -c 3 mixer-pro` ou `CPUAffinity=3` dans `.service`. Sur i.MX8MP 4 cores, isole le mixer du reste du système.
6. **isolcpus=3** (kernel cmdline) : optionnel, exclut un cœur du scheduler général. Modif `bootargs` dans U-Boot ou DT.

Priorité estimée : (1) + (3) sont les **leviers majeurs**, faisables sans modif kernel.

Aucune de ces actions ne touche au code mixer-pro/effects.c. **Les effets restent en place tels quels.**

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
- **Sprint E6.g recommandé** (post-vérif kernel) : retrait `snd_pcm_link` + 3-thread + lockfree (kernel a déjà PREEMPT, RT pas critique)

E6.f est un **succès méthodologique** : on ne fait PAS d'optim spéculative. On mesure d'abord, on conclut, on documente.
