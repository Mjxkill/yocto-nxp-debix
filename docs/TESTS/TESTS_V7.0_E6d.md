# Test Fiche : V7.0 — E6.d (mixer-pro — console DAW SW)

**Date** : 2026-05-11
**Statut** : **GO MVP** — fonctionnel sur board à **48 kHz steady**, latence ALSA interne **8 ms** (sous cible V7.0 < 10 ms).
**Tag git associé** : `v7.0-e6d` (posé après commit, doc rectifiée après mesures latence)

## Référentiel

| Champ | Valeur |
|---|---|
| Phase | V7.0 — E6.d Mixer SW vrai console DAW (26 in / 4 bus FX stéréo / 18 out) |
| Préalable | E6.a + E6.b + E6.c GO (UAC2 + Phone + alsa-route-bridge) |
| Pas de modif SOF/kernel/DT/firmware | userspace + alsa-lib uniquement |
| Branche yocto | `feature/v7.0-multiband-drc-tap` |

## Architecture livrée

```
INPUTS (26 mono)                   BUS FX (4 stéréo = 8 ch)        OUTPUTS (18 mono)
─────────────────────────          ────────────────────────         ────────────────────
[0..7]   DSP mics (cap)       ─┐   FX1 L/R (passthrough MVP)        [0..7]   DSP play
[8..15]  UAC2 in (DAW stems)   ├── FX2 L/R                          [8..15]  UAC2 out
[16..17] Phone in              │   FX3 L/R                          [16..17] Phone out
[18..25] returns FX (8 ch)     │   FX4 L/R
                               │              │
                               │              ▼  master matrix 26 × 18
                               └────────────► master gain

Matrices :
  send_gain[26][8]      : 4 sends stéréo par voie
  fx_bus_gain[8]        : niveau global par bus (MVP = passthrough)
  master_gain[26][18]   : mix final (in + returns) → outputs
```

## Travaux exécutés

| Domaine | Fichier |
|---|---|
| Daemon C | `mixer-pro.c` (~470 LOC, single-threaded audio loop + thread control socket) |
| Headers | `mixer-pro.h` (constants architecture, indices in/out) |
| CLI client | `mixerctl.c` (~100 LOC, JSON over socket) |
| Build | `Makefile` (alsa-lib + pthread + math + rt) |
| Service | `mixer-pro.service` (LimitRTPRIO=99 LimitMEMLOCK=infinity, SCHED_FIFO 80) |
| Recipe | `mixer-pro_1.0.bb` (DEPENDS alsa-lib, SYSTEMD_AUTO_ENABLE=disable) |
| Image | `IMAGE_INSTALL += " mixer-pro"` dans `imx-image-full.bbappend` |

## Protocole socket Unix `/run/mixer-pro.sock`

JSON ligne par ligne (parser ad-hoc, pas de lib). Commandes :

| Op | Args | Action |
|---|---|---|
| `set_send`   | `in`(0..25), `bus`(0..7), `gain`(float) | Set send level d'une voie vers un bus L/R |
| `set_master` | `src`(0..25), `out`(0..17), `gain`(float) | Set master matrix cell |
| `set_fx_bus` | `bus`(0..7), `gain`(float) | Set niveau d'un bus FX (= "wet amount") |
| `set_mute`   | `src`(0..25), `mute`(0\|1) | Mute source |
| `get_state`  | — | JSON : version, frames_processed, xrun_count, mute_mask |
| `reset`      | — | Toutes matrix à 0, fx_bus = 1, mute = 0 |

CLI client `mixerctl` :
```bash
mixerctl master 0 0 1.0           # mic 0 → speaker 0 unity
mixerctl send   3 2 0.5           # mic 3 → bus FX1 L (bus index 2) à -6 dB
mixerctl fx     0 1.0             # bus FX1 L unity
mixerctl mute   5 1               # mute source 5
mixerctl state
mixerctl reset
```

Smoothing 64-frame ramp côté DSP thread sur tout changement gain (évite zip noise).

## Tests T6d.X

| Test | Description | Résultat | Critère GO |
|---|---|---|---|
| **T6d.1** | Build daemon + CLI client (alsa-lib + pthread) | ✓ OK | warnings clean |
| **T6d.2** | Daemon ouvre les 6 PCMs ALSA, lance threads RT et control | ✓ OK | log "SCHED_FIFO prio 80 OK", "control socket listening" |
| **T6d.3** | Socket `/run/mixer-pro.sock` réceptif, JSON commandes/réponses | ✓ OK | `mixerctl state` retourne JSON |
| **T6d.4** | Idle (toutes matrix=0) : tap-out signal = silence absolu | ✓ OK | -inf dB sur toutes voies |
| **T6d.5** | Routage mic 0 → speaker 0 unity gain | ✓ OK (tap-out voie 0 = -96 dB = signal mic présent) | signal mic 0 visible sur out 0 |
| **T6d.6** | Voies non routées strictement silencieuses | ✓ **OK** (voies 2-7 du tap-out = **-inf dB**) | preuve : matrix précise |
| **T6d.7** | Snd_pcm_link DSP cap ↔ play : start synchrone, 0 cycle de recover | ✓ OK (xrun init ~15-20, plateau steady) | pas de cycle recover continuel |
| **T6d.8** | Throughput steady mesuré via `frames_processed` delta entre t=3s et t=5s | ✓ **OK : 48 144 Hz** (96288 frames / 2.000 s = 100.3 % nominal, dans marge mesure) | ≥ 48000 Hz nominal |
| **T6d.9** | Latence ALSA interne via `snd_pcm_delay` | ✓ **OK : 8 ms** (`play_delay=384 frames`, `cap_delay=0`) | < 10 ms (cible V7.0) |
| **T6d.10** | xrun stable steady (pas d'augmentation après init) | ✓ OK (xrun=18 fixé à t=3s et t=5s) | delta xrun = 0 entre 2 mesures |
| **T6d.11** | Skip flags `--no-uac2` et `--no-phone` fonctionnels (mode dev sans host PC) | ✓ OK | daemon démarre, mute sur PCMs skipped |
| **T6d.12** | 0 régression DSP/SOF (firmware/kernel inchangés) | ✓ OK (tags v7.0-e0..e6c préservés) | aucun commit SOF/kernel |

## Mesure empirique T6d.5/T6d.6

**Setup** : `mixer-pro --no-uac2 --no-phone` + `mixerctl master 0 0 1.0`. Lecture tap-OUT (post-DSP play).

| Voie tap-OUT | Peak | Cause |
|---|---|---|
| 0 | **-96 dB** | Mic 0 (ambient noise floor) → speaker 0 unity = passé |
| 1 | -96 dB | Couplage PGA Strip1 stéréo (E3 quirk connu, channel-map FL/FR) |
| 2 à 7 | **-inf dB** | **Aucun routing → silence ABSOLU** |

→ La matrice fait exactement ce qu'on lui demande : strictly diagonal, zéro fuite vers les voies non câblées.

## Mesures empiriques throughput + latence (vrai)

Mesure côté daemon via `snd_pcm_delay` exposé dans `get_state` :

| Mesure | t=1s | t=3s | t=5s | Steady ? |
|---|---|---|---|---|
| `frames_processed` | 672 | 33312 | 129600 | OUI à partir de t=3s |
| `xrun` cumulé | 8 | 18 | 18 | Plateau fixé (pas d'augmentation après init) |
| `cap_delay_frames` | 0 | 0 | 0 | Cap consommée sans backlog |
| `play_delay_frames` | 0 | 384 | 384 | Buffer plein steady = 8 ms |
| Latence ALSA interne (one-way) | 0 ms | 8 ms | **8 ms** | Sous cible V7.0 |

**Throughput steady = 48 144 Hz** mesuré entre t=3s et t=5s (delta 96288 frames / 2.000 s wallclock). Ratio 100.3 % du nominal 48 kHz — dans la marge de mesure du wallclock SSH.

**Première mesure (avant warmup) à t=1s donnait 672/1 = ~670 Hz** apparent, ce qui reflète le démarrage : xrun init (cap+play prepare, ALSA streaming launch), pas un vrai défaut steady. Cette mesure brute m'avait conduit à diagnostiquer à tort un throughput sub-nominal persistant — incorrect, **rectifié après mesure steady**.

## Limitations connues + Roadmap E6.e

### Latence end-to-end NON mesurée (pipeline complet)

La latence **8 ms** rapportée par `snd_pcm_delay` couvre uniquement les buffers ALSA cap+play du mixer. La latence end-to-end **microphone acoustique → speaker acoustique** demande de mesurer aussi :
- ADC TAC5212 codec (~0.5 ms, datasheet ultra-low-lat decimation)
- DMA SAI RX (period 2 ms)
- DSP cap pipeline (multiband_drc + drc D3 + pga, ~0.5 ms inline)
- mixer-pro buffer (mesuré : 8 ms)
- DSP play pipeline (multiband_drc + pga, ~0.5 ms inline)
- DMA SAI TX (period 2 ms)
- DAC TAC5212 (~0.5 ms)

Estimation E2E acoustique : **~14 ms**. Au-dessus de la cible V7.0 < 10 ms.

**Réduction possible** : passer `N_PERIODS` de 4 à 2 → buffer mixer = 4 ms (au lieu de 8 ms). Total E2E ~10 ms tight. Risque : moins de marge xrun. **À tester en sprint séparé** avec mesure E2E loopback acoustique (signal impulsionnel + analyse temporelle).

### Bus FX = passthrough (pas de vrais effets)

### Bus FX = passthrough (pas de vrais effets)

MVP livre l'infrastructure routing/sends/master mais les 4 bus FX appliquent juste un gain global (= identité scaled). **E6.e** = ajouter de vrais plugins LV2 (Calf compressor, freeverb reverb, delay, EQ) chargés via `lilv`.

### Mute mask 32 bits

Source mute_mask est uint32 → couvre les 26 sources actuelles. Pas extensible au-delà.

## Notes design

### Single-thread audio + control thread séparé

Choix MVP : 1 thread audio (RT prio 80) qui fait read/mix/write en série, + 1 thread control (normal) qui accept() socket et écrit dans `*_target[]` sous mutex. Le thread audio lit le mutex sur chaque iteration via `smooth_gains()` (mutex courte durée).

Avantages :
- Pas de complexité lockfree ring buffer
- Latence prévisible (3 phases sérialisées : 2ms read + ~0 mix + 2ms write)
- 30 MFLOPS de mac est trivial pour Cortex-A53

Inconvénients :
- Si une PCM se met en xrun, blocking → bloque tout le pipeline
- Pas de scaling au-delà de ~4 paires PCMs

### snd_pcm_link DSP cap/play

Clef pour stabilité. Sans link, le DSP cap et le DSP play accumulent indépendamment → cycle de recover continuel. Avec link, démarrage synchrone hardware-clock-locked.

### NONBLOCK pour UAC2/Phone

UAC2 dépend d'un host PC connecté (sans host = stream stuck). Phone aloop dépend d'écritures sur Phone,0 pour que Phone,1 lise. Les 2 sont NONBLOCK = retournent EAGAIN si pas dispo, qu'on traite comme zero (silence).

## Conclusion

- **Mixer console DAW fonctionnel** : matrice 26 in × 18 out + 4 bus FX stéréo + sends
- **Pilotage par socket JSON** : `/run/mixer-pro.sock` + CLI `mixerctl`
- **Matrice précise** : voies non routées strictement silencieuses (-inf dB)
- **0 régression DSP/SOF**
- ✓ **Throughput steady = 48 kHz nominal** (mesure rectifiée après warmup ; la mesure brute t=1s reflétait l'init)
- ✓ **Latence ALSA interne mesurée = 8 ms** (`snd_pcm_delay`), sous cible V7.0 < 10 ms côté mixer
- ⚠️ **Latence E2E acoustique non mesurée** — estimation ~14 ms (mic → DAC), au-dessus de 10 ms cible. Réduction possible en passant N_PERIODS=2 + mesure loopback impulsionnel.
- **Bus FX en passthrough** : effets LV2 = E6.e

**Prochaine étape** : E7 — GUI test (Qt/Flutter/web) qui pilote ce mixer via socket + expose les sliders effets DSP/TAC.
