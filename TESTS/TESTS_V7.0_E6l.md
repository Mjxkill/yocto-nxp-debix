# Test Fiche : V7.0 — E6.l (mixer-pro — single-thread + N_PERIODS=2) — **KO REVERT**

**Date** : 2026-05-11
**Statut** : **KO — non commité** (working tree restored)
**Tag git** : aucun (échec)

## Référentiel

| Champ | Valeur |
|---|---|
| Phase | V7.0 — E6.l Single-thread (suppression ring SPSC + play_thread + eventfd) + N_PERIODS=2 |
| Préalable | E6.h baseline 14 ms, après revert E6.i/j/k |
| Branche yocto | `feature/v7.0-multiband-drc-tap` |
| Investigation critic | job `f9547d24-d644-432f-ad2d-060369191853` (6/6 workers, 688s) |
| Phase 1 critic_analyze | approved=true (qwen 65, factual_errors flagged) |

## Hypothèse testée

> Convergence 3/5 workers : éliminer le ring SPSC (6-8 ms steady) en fusionnant
> `play_thread` dans `audio_thread` (cap → mix → writei play_dsp blocking
> directement). Avec N_PERIODS=2 (ALSA 4 ms), cible :
> ring 0 ms + ALSA 4 ms = **4 ms mixer total** → E2E acoustique ~10 ms.

## Implémentation E6.l

- Suppression complète : `ring_buf`, `ring_write_idx`, `ring_read_idx`,
  `ring_drops`, `ring_event_fd`, fonction `play_thread()`, eventfd init/close,
  `pthread_create(th_play)`, `pthread_join(th_play)`
- Remplacement push ring → `snd_pcm_writei(play_dsp, blocking)` direct dans
  `audio_thread`, AVANT writei UAC2/Phone NONBLOCK (suggestion critic Phase 1)
- `mixer-pro.h` : `N_PERIODS 4 → 2`, `MIXER_VERSION v7.0-e6l`
- Suppression `N_RING_PERIODS` / `RING_FRAMES`
- Prefill ALSA standard (style UAC2/Phone) pour play_dsp

## Mesures empiriques (DSP-only, --no-uac2 --no-phone, board déjà reboot)

| t | frames | Δ Hz | xrun | Δ xrun /s | prof_iter_us | prof_cap_us | prof_play_us |
|---|---|---|---|---|---|---|---|
| 5s | 7584 | — | 78 | — | **126 470** | **61 703** | **64 312** |
| 8s | 12192 | 1536 | 127 | 16/s | 1588 | 526 | 607 |
| 13s | 20064 | 1574 | 208 | 16/s | **122 489** | **61 716** | **60 319** |
| 23s | 35424 | 1536 | 369 | 16/s | 1590 | 527 | 608 |

## Verdict

| Métrique | Cible | E6.l mesuré | Verdict |
|---|---|---|---|
| Throughput | ≥ 48 000 Hz | **1 546 Hz (3.2 %)** | **❌ effondrement** |
| Δ xrun /s | 0 | **16/s** | **❌** |
| `prof_iter_us` | < 2000 µs | **122 ms toutes les 5 s** | **❌ cycle catastrophique** |
| `prof_cap_us` | ~500 µs | **60 ms périodiques** | **❌ cap bloqué par recover** |
| `prof_play_us` | ~500 µs | **60 ms périodiques** | **❌ play bloqué par recover** |
| Latence côté play | 4 ms | indéterminée (instable) | **❌** |

## Cause racine de l'échec

Le ring SPSC d'E6.h **n'était pas un caprice** — il absorbait les recover SOF
60 ms en isolant cap et play sur deux threads séparés. Une fois fusionnés dans
un seul thread, **chaque recover bloque cap+play en cascade** :

1. `prof_cap_us=60 ms` : `snd_pcm_readi` (cap_dsp) attend que cap recover (60 ms)
2. `prof_play_us=60 ms` : ensuite `snd_pcm_writei` (play_dsp) recover (60 ms)
3. Total : 122 ms par recover → cap retard à 60 frames sur 96 nominal
4. Audio_thread reprend, mais avec N_PERIODS=2 (ALSA 4 ms) **aucune marge**
5. Au prochain jitter ou recover → re-xrun → re-recover → cercle vicieux

Les recover SOF en E6.h étaient observés "init only" dans la mesure DSP-only.
**Mais avec single-thread + N_PERIODS=2, ils deviennent endémiques** car le
système n'a aucune marge d'absorption.

## Convergence avec théorème de Little (claude-code worker)

> "La somme des planchers incompressibles (TAC ADC 0.5 + DMA RX 2 + cap pipe 0.5 +
> ALSA cap min 2 + mix 0.5 + ALSA play min 2 + DMA TX 2 + DAC 0.5 = 10 ms)
> dépasse déjà la cible AVANT toute marge anti-xrun. La cible <10 ms acoustique
> est mathématiquement inatteignable avec l'architecture V7.0 actuelle."

E6.l a tenté de retirer la marge anti-xrun → cercle vicieux → **preuve
empirique que le worker claude-code avait raison**.

## Action exécutée

```bash
ssh root@192.168.0.9 'pkill -9 mixer-pro'
git restore meta-local/recipes-audio/mixer-pro/files/mixer-pro.{c,h}
```

Aucun commit code E6.l — modifications annulées en working tree.

## Bilan global des 4 itérations userspace V7.0

| Sprint | Tentative | Verdict | Latence mesurée |
|---|---|---|---|
| **E6.h** | Eventfd + ring 8 périodes (référence) | ✓ GO | 14 ms côté mixer, 20 ms E2E estimé |
| **E6.i** | Drainage blocking + N_PERIODS=2 (MAX_DRAIN=4) | ❌ KO reverted | ring saturé 16 ms |
| **E6.j** | Drainage NONBLOCK + avail_update | ❌ KO non commité | EAGAIN systématique, drops 50k/s |
| **E6.k** | N_PERIODS=2 seul (logic E6.h conservée) | ❌ KO non commité | ring saturé 16 ms, +6 ms régression |
| **E6.l** | Single-thread + N_PERIODS=2 | ❌ KO non commité | recover cascade 122 ms, throughput 3.2 % |

**Conclusion** : E6.h (14 ms) est la limite atteignable proprement en userspace
sur ce SOF i.MX8MP. La cible V7.0 < 10 ms acoustique est **architecturalement
hors d'atteinte** sans refonte (mixer DSP SOF natif V8.0).

## Décision

E6.h = **baseline V7.0 finale**. Cible < 10 ms acoustique reportée à V8.0
avec architecture mixer DSP SOF natif (piste 2 worker convergence).

Suite immédiate : **E7 GUI HTTP** (livrable utilisateur principal V7.0) qui
pilotera ce mixer-pro stable via socket JSON `/run/mixer-pro.sock`.

## Tests T6l.X — partiellement effectués

| Test | Résultat |
|---|---|
| T6l.0 Vérif kernel config | ✓ CPU_ISOLATION=y, ❌ NO_HZ_FULL, ❌ RCU_NOCB_CPU |
| T6l.1 Build OK | ✓ warnings 0 |
| T6l.2 Démarrage 2 threads RT | ✓ audio + control |
| T6l.3 Throughput steady | ❌ 1546 Hz (3.2 %) |
| T6l.6 ring_fill = 0 | ✓ (ring supprimé) |
| T6l.7 play_delay 96-192 | ⚠️ 0 ou 96, instable |
| T6l.9 xrun delta = 0 | ❌ 16/s |
| T6l.10 prof_iter < 2000 µs | ❌ 122 ms cycliques |
| T6l.14 Loopback acoustique | ❌ non atteint, conditions instables |

## Note isolcpus non testé

L'investigation a montré que sur kernel 6.6.36 NXP :
- `CONFIG_NO_HZ_FULL=y` ❌ ABSENT
- `CONFIG_RCU_NOCB_CPU=y` ❌ ABSENT
- Seul `CONFIG_CPU_ISOLATION=y` ✓ disponible

`isolcpus=3` seul (sans nohz_full/rcu_nocbs) apporterait un gain marginal et
nécessiterait une modif kernel cmdline (DT rebuild + reflash). N'a pas été
tenté car la cause empirique (recover SOF cascade) n'est pas un problème de
jitter scheduler — c'est un problème architectural d'absence de tampon
anti-recover.
