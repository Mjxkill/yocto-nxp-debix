# Test Fiche : V7.0 — E6.j (mixer-pro — drainage NONBLOCK + avail) — **KO REVERT**

**Date** : 2026-05-11
**Statut** : **KO — non commité** (working tree restored)
**Tag git** : aucun (échec)

## Référentiel

| Champ | Valeur |
|---|---|
| Phase | V7.0 — E6.j Drainage NONBLOCK play_thread + N_PERIODS=2 |
| Préalable | E6.h (commit `add54d8d`, tag `v7.0-e6h`) après revert E6.i |
| Branche yocto | `feature/v7.0-multiband-drc-tap` |
| Pas de modif SOF/kernel/DT/firmware | userspace pur |

## Hypothèse testée

> Drainage NONBLOCK avec `snd_pcm_writei` qui retourne immédiatement ou `EAGAIN`.
> Permet au play_thread de transférer le surplus du ring vers ALSA plus vite
> que ALSA ne consomme → ring vidé steady → ring_fill 0-96 + ALSA 4 ms = 6 ms total.
> Path recommandé par workers deepseek-v4-pro + glm-5.1 (critic 55f42bac).

## Implémentation

- `snd_pcm_nonblock(g_st.play_dsp.pcm, 1)` au démarrage play_thread
- Pop ring SANS avancer `ri` (puis avance après write success)
- writei NONBLOCK : si `-EAGAIN` → break (attente eventfd)
- Garde `MAX_DRAIN_LOOPS=8` anti-runaway
- Partial write géré : avance `ri` du nombre réellement écrit
- N_PERIODS=2 (ALSA 4 ms)

## Mesures empiriques (DSP-only, --no-uac2 --no-phone, board reboot propre)

| t | frames | Δ frames (Hz) | xrun | Δ xrun /s | ring_fill | ring_drops | Δ drops /s |
|---|---|---|---|---|---|---|---|
| 5s | 229536 | — | 29 | — | 672 | 194400 | — |
| 8s | 374496 | 48 320 | 61 | 11 | 3456 | 430944 | 78 848 |
| 13s | 615264 | 48 154 | 96 | 7 | 672 | 633984 | 40 608 |
| 18s | 852096 | 47 366 | 134 | 7 | 672 | 881952 | 49 600 |

## Verdict

| Métrique | Cible | E6.j mesuré | Verdict |
|---|---|---|---|
| Throughput | 48 kHz | 47.4-48.3 kHz | ✓ |
| **Δ xrun /s** | 0 | **7-11/s steady** | **❌** |
| **Δ ring_drops /s** | 0 stable | **~50 000/s continu** | **❌** |
| ring_fill steady | 0-96 | 672 + spikes 3456 | ❌ |
| play_delay | 96-192 | 0-192 oscillant | ❌ instable |

**Régression majeure** : drops 50 000 frames/s + xrun continus.

## Cause racine probable

`snd_pcm_writei` en mode NONBLOCK semble retourner `-EAGAIN` systématiquement
ou presque sur SOF i.MX8MP. Le play_thread break immédiatement, ne draine
quasiment rien, et le ring se remplit. audio_thread déclenche la drop policy
en boucle (50 000 frames/s).

Warning anticipé par les workers deepseek-v4-pro + glm-5.1 lors de l'investigation
55f42bac :
> snd_pcm_avail_update sur SOF i.MX8MP : comportement non vérifié sur certains
> drivers SOF.

Comportement empiriquement confirmé sur SOF firmware actuel (hash inconnu).

## Action exécutée

```bash
ssh root@192.168.0.9 'pkill -9 mixer-pro'
ssh root@192.168.0.9 'reboot'  # ALSA en état "Invalid argument" après crash
# Après reboot : ré-test pour valider, mêmes drops massifs
git restore meta-local/recipes-audio/mixer-pro/files/mixer-pro.{c,h}
```

Pas commité — modifs `mixer-pro.c` + `mixer-pro.h` annulées en working tree.

## Leçon apprise

NONBLOCK ALSA sur SOF i.MX8MP **n'est pas une option fonctionnelle** pour
`snd_pcm_writei` sur le PCM DSP play. Le path doit être abandonné pour
toute optim latence userspace.

## Conclusion

E6.j NONBLOCK **non viable sur ce hardware/firmware**. Suite : E6.k (N_PERIODS=2
seul, blocking conservé) — testé indépendamment avec verdict KO également.
Puis E6.l (investigation critic alternative).
