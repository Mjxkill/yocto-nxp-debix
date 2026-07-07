# Test Fiche : V7.0 — E6.i (mixer-pro — drainage agressif blocking) — **KO REVERT**

**Date** : 2026-05-11
**Statut** : **KO — revert** (commit `e32d184c` annulé par `493d8217`)
**Tag git** : aucun (revert)

## Référentiel

| Champ | Valeur |
|---|---|
| Phase | V7.0 — E6.i Drainage agressif blocking play_thread + N_PERIODS=2 |
| Préalable | E6.h (commit `add54d8d`, tag `v7.0-e6h`) — eventfd + ring 8 périodes |
| Branche yocto | `feature/v7.0-multiband-drc-tap` |
| Pas de modif SOF/kernel/DT/firmware | userspace pur |
| Investigation critic | job `55f42bac-b5ff-417c-af86-83a55772d27e` (6/6 workers) |

## Hypothèse testée

> Boucle de drainage interne play_thread (blocking writei, cap MAX_DRAIN_PERIODS=4)
> couplée à N_PERIODS=2 → ring vide steady → latence E2E < 10 ms

## Mesures empiriques (DSP-only, --no-uac2 --no-phone)

| Métrique | Cible | E6.i mesuré | E6.h ref | Verdict |
|---|---|---|---|---|
| throughput | ≥ 48 kHz | **48 192 Hz** (100.4 %) | 48 224 Hz | ✓ |
| xrun delta sur 3s | 0 | **0** | 0 | ✓ |
| **ring_fill_frames steady** | 0-96 | **768 (16 ms !)** | 288-384 | **❌** |
| play_delay_frames | 96-192 | 96-192 | 288 | ✓ |
| **Latence côté play (ring + ALSA)** | < 8 ms | **20 ms** | **12 ms** | **❌ régression -8 ms** |
| ring_drops init burst | bornés | 4896 stable après | bornés init | ✓ |

## Cause racine de l'échec

`snd_pcm_writei` blocking **est self-limiting à 48 kHz** : ALSA bloque dès que son
buffer est plein. Donc play_thread ne peut PAS drainer plus vite que la production
de audio_thread (48 kHz strict). Conséquence :

- Audio_thread push 1 période / 2 ms
- Play_thread blocking : pop 1 → writei blocking ~2 ms → pop 1 → writei ~2 ms…
- **Net rate = 0** → équilibre dynamique inchangé
- Pire : avec `MAX_DRAIN_PERIODS=4`, play_thread accumule en interne et ALSA
  régule sur le 2e write → ring se remplit MAX (768 frames) au lieu de se vider

Le critic l'avait flagged en `factual_error #2` lors de Phase 1 :
> Starvation audio_thread : play_thread (prio 81) peut drainer 16 ms de ring
> en continu avant de céder.

L'effet réel n'est pas la starvation mais le **remplissage maximal du ring** —
le drainage blocking ne fait que vider transitoirement le ring, pas baisser
le steady state. Le critic avait raison sur la direction du danger, mauvaise
catégorisation.

## Leçon apprise

**Le drainage agressif blocking ne fonctionne PAS pour vider le ring.** Il faut
un drain qui peut consommer plus vite que ALSA, donc obligatoirement **NONBLOCK
+ snd_pcm_avail_update** (path workers deepseek/glm-5.1). Avec NONBLOCK, write
retourne immédiatement si ALSA plein → drain peut accumuler dans ALSA buffer
le surplus du ring, puis attendre eventfd.

## Plan E6.j (suite logique)

- **NONBLOCK** sur play_dsp PCM
- **snd_pcm_avail_update** dans la boucle de drain pour connaître l'espace ALSA
- Drain par batch `min(ring_avail, alsa_avail)` jusqu'à épuisement ou ALSA plein
- N_PERIODS=2 (ALSA 4 ms) — à conserver car ALSA seul ne plombe pas la latence
- Fallback EAGAIN à gérer
- ~40 LOC selon estimation deepseek/glm

## Tests T6i.X — résultats partiels

| Test | Description | Résultat |
|---|---|---|
| T6i.1 | Build OK clean warnings | ✓ |
| T6i.2 | Démarrage 3 threads RT | ✓ |
| T6i.3 | Throughput steady 30 s DSP-only | ✓ 48 192 Hz |
| **T6i.4** | `ring_fill_frames` steady < 96 | **❌ 768** |
| T6i.5 | `play_delay_frames` < 192 | ✓ 96-192 |
| T6i.6 | `latency_us_one_way` ALSA only ≤ 4 ms | ✓ 2-4 ms |
| **T6i.7** | Latence mixer totale ≤ 8 ms | **❌ ~20 ms** |
| T6i.8 | xrun delta = 0 | ✓ |
| T6i.9 | ring_drops bornés à init | ✓ |
| T6i.10 | Régression matrice/sends/effets | non testé (UAC2 host I/O error) |
| T6i.11 | Shutdown propre | ✓ |
| T6i.12 | Loopback acoustique | non atteint |
| T6i.13 | prof_iter_us pendant drain | ✓ 1971 µs (< 2000 µs) |

## Observation annexe : UAC2 host I/O error

Pendant le test initial (avec UAC2 actif), `prof_cap_us=104 ms`, `prof_play_us=104 ms`,
xrun=4.3/s. Cause : UAC2 host PC en `aplay: pcm_write: Input/output error` —
gadget USB pas consommé côté host. Sans rapport avec E6.i, mais à diagnostiquer
séparément (le câble USB est branché mais host pas en cours d'usage UAC2).

Reproduit en isolation DSP (--no-uac2 --no-phone), problème UAC2 disparaît,
révèle le vrai bug E6.i (ring fill 768).

## Action exécutée

```bash
git revert e32d184c   # Revert commit E6.i
git push              # Force HEAD back to E6.h fonctionnel
```

`MIXER_VERSION` redevient `v7.0-e6h`. État board : `v7.0-e6h` opérationnel
(14 ms total mesuré dans T6h).

## Conclusion

E6.i blocking simple **n'atteint pas l'objectif latence < 10 ms** et **régresse de -8 ms**.
Revert immédiat selon règle "no fallback, revert iter sans effet".
Suite : **E6.j NONBLOCK + avail_update** (path workers deepseek/glm-5.1).
