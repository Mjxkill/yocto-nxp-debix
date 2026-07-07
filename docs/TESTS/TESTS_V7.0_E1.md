# Test Fiche : V7.0 — E1 (topology simplifiée + ALSA low-lat)

**Date** : 2026-05-10
**Statut** : **GO** — Test utilisateur OUI (« quand stabilisé tout est OK »)
**Tag git associé** : `v7.0-e1` (à poser après commit)

## Référentiel

| Champ | Valeur |
|---|---|
| Phase | V7.0 — E1 topology simplifiée + ALSA low-lat |
| Préalable | E0 GO (tag `v7.0-e0`, commit `eacf6c20`) |
| Branche yocto | `feature/v7.0-multiband-drc-tap` |
| Branche SOF | `feature/v7.0-multiband-drc-tap` |
| HEAD SOF (avant commit E1) | `0580b5f14` (E6.a baseline) — sera enrichi |
| Firmware md5 board | `b3e1ebcfb937adbb4a499af2756493ec` (E6.a baseline, inchangé) |
| Topology .tplg md5 board | `cb68c1baf3836fabe4d32aa72452398e` (V7.0-E1 nouveau) |
| Kernel Image md5 | `e32b7bcaec429f392fc286c3f32761e9` (V7.0-E0 inchangé) |

## Travaux exécutés

| Domaine | Action |
|---|---|
| SOF topology | Créé `sof/tools/topology/topology1/sof-imx8mp-tac5212-V7.0.m4` : PIPE 1 cap inchangé (eq_iir + drc D3 + pga), PIPE 2 play simplifié (host PCM 1 → B0 → SAI7 TX direct, plus de mixer16 / deinterleave_8 / interleave_8) |
| SOF pipe macro custom | Créé `sof/tools/topology/topology1/sof/pipe-passthrough-8ch-playback.m4` : variante 8 ch du standard SOF avec PCM_CAPABILITIES aligné stack Debix (`192, 16384, 65536, 65536`) → autorise period play jusqu'à 6 frames @ 8 ch S32 (125 µs min), au lieu de 768 frames (16 ms) du standard inadapté |
| Build .tplg | `m4 + alsatplg` → `sof-imx8mp-tac5212-V7.0.tplg` md5 `cb68c1ba...` |
| Deploy | scp `.tplg` → `/lib/firmware/imx/sof-tplg/sof-imx8mp-tac5212.tplg` + reboot |
| Linux user tool | Créé `/root/tests/loopback-c-lowlat.c` : variant low-lat de loopback-c avec `mlockall(MCL_CURRENT\|MCL_FUTURE)` + SCHED_FIFO prio cap=80/play=81 + defaults period=96/n_periods=2 |

## Build & deploy

| Étape | Commande | Résultat |
|---|---|---|
| Topology m4 | `m4 -I . -I m4 -I common -I platform/common -I sof sof-imx8mp-tac5212-V7.0.m4 > .conf` | OK 33187 octets |
| alsatplg | `alsatplg -c .conf -o .tplg` | OK 11860 octets |
| Deploy tplg | `scp .tplg root@192.168.0.9:/lib/firmware/imx/sof-tplg/sof-imx8mp-tac5212.tplg` | md5 cohérent |
| Reboot | `systemctl reboot` | OK, dmesg propre |
| Compile loopback-c-lowlat | `gcc -O2 -Wall -D_GNU_SOURCE loopback-c-lowlat.c -lasound -lm -lpthread` | OK, no warning |

## Tests T1.X

| Test | Description | Résultat | Critère GO |
|---|---|---|---|
| **T1.1** | Topology compile + signature | ✓ OK | tplg généré sans erreur |
| **T1.2** | Board boot dmesg pipelines OK | ✓ OK (2 pipelines, 0 erreur ipc/V6, kcontrols visibles) | 2 pipelines instanciées |
| **T1.3** | kcontrols toujours présents | ✓ OK (58 kcontrols TAC0) | list ≥ T0.5 (50) |
| **T1.4** | Loopback ALSA RT mesuré | partiel — RTT non chiffré (alsaaudio python absent board) | RTT mesuré, valeur consignée |
| **T1.5** | Latence end-to-end < 10 ms | ⏳ reporté E1.b (kernel non-PREEMPT_RT, mesure outil manquant) | RTT mesuré < 10 ms |
| **T1.6** | Steady state xrun_play stable | ✓ OK (cumul figé à 73-75 après init, +0/tick stable 8+ ticks) | delta xrun_play stable en steady state |
| **T1.7** | Steady state ring_drop stable | ✓ OK (cumul figé à 1893-1952, ring_fill 192-384 oscille bornée) | delta ring_drop stable |
| **T1.8** | Test utilisateur | ✓ OUI 2026-05-10 (« quand stabilisé tout est OK ») | « son OK, perception rapide » |

## Mesures empiriques (steady state V7.0-E1, period=96 + mlockall + SCHED_FIFO)

| Métrique | Valeur stable |
|---|---|
| in fps | 48000 (±96) |
| out fps | 48000 (±96) |
| ALSA period cap | 96 frames (2 ms) |
| ALSA period play | 96 frames (2 ms) — résolu grâce à pipe-passthrough-8ch-playback custom |
| ALSA buffer | 192 frames (4 ms) chaque côté |
| ring_fill | 192-384 frames oscille (delta bornée) |
| xrun_cap cumulé (init + steady) | 6 |
| xrun_play cumulé (init + steady) | 73 (+0/tick steady, stable) |
| ring_drop cumulé (init + steady) | 1893 (+0/tick steady) |

**Note** : les xrun sont concentrés dans la phase d'init (~3-5s premières secondes). Une fois le système stabilisé, **les compteurs ne montent plus** (delta = 0 sur 8+ ticks consécutifs). Comportement reproductible.

## Comparaison vs E0

| Critère | E0 (E6.a baseline) | E1 (V7.0 simplifiée) | Verdict |
|---|---|---|---|
| Pipeline play | mixer16 + deinterleave + interleave | passthrough direct | **simplifié OK** |
| ALSA period play min | 256 frames (5.33 ms) testé | **96 frames (2 ms) acceptée** | **gain low-lat** |
| Kcontrols TAC | 50 | 58 | OK (gain) |
| Audio loopback | OK | OK steady | **OK** |

## Notes techniques

### SCHED_FIFO sur kernel non-PREEMPT_RT

Le kernel actuel n'est PAS PREEMPT_RT. SCHED_FIFO prio 80 sur les threads `loopback-c-lowlat` génère plus de xrun en phase d'init que SCHED_OTHER, car il préempte `ksoftirqd` qui draine les IRQ SDMA.

En steady state, l'effet est moins visible (les IRQ sont déjà alignées). À éva­luer en sprint kernel PREEMPT_RT dédié.

### RTT < 10 ms non chiffré

L'outil Python avec `pyalsaaudio` n'est pas dans l'image. Cible mesurage RTT reportée à E1.b ou plus tard (à équiper avec un binaire C de mesure RTT avec PCM duplex, sans dépendance Python).

Architecturalement, RTT théorique :
- ALSA cap 2 ms + DSP cap DMA 2 ms × 2 + DSP play DMA 2 ms × 2 + ALSA play 2 ms = **~12 ms** théorique
- Si le mixer Linux ajoute 1-2 ms : ~14-16 ms total
- → cible < 10 ms nécessite période plus petite ou bypass mixer Linux

## Test utilisateur

| Critère | OUI / NON |
|---|---|
| Audio loopback audible / fonctionnel | **OUI** (« quand stabilisé tout est OK ») |
| Validation E1 GO | **OUI — GO** — tag `v7.0-e1` à poser |

## Conclusion

- **Topology V7.0 simplifiée** : OK, mixer16 / deinterleave / interleave retirés. Acquis principal de E1.
- **Pipe macro custom** : `pipe-passthrough-8ch-playback.m4` propre, aligné stack Debix.
- **ALSA low-lat outillage** : `/root/tests/loopback-c-lowlat.c` avec mlockall + SCHED_FIFO. Permet period=96 (2 ms) en steady state.
- **RTT < 10 ms non chiffré** : reporté à E1.b (mesurage) + kernel PREEMPT_RT (raffinage). Pas bloquant pour E1 GO.
- **Suite** : E2 — multiband_drc CAP (8 ch indép, patch state arrays modèle drc D3).

## Annexes

- Source loopback low-lat : `/root/tests/loopback-c-lowlat.c` (binaire compilé `/root/tests/loopback-c-lowlat`)
- Backup ancien tplg sur board : `sof-imx8mp-tac5212.tplg.bak-pre-v7e0` (E0) + nouveau backup automatique à venir
- Reproductibilité : `cd /root/tests && /usr/bin/tac-reset && ./loopback-c-lowlat 96 2` (period n_periods)
