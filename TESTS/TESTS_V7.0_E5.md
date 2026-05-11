# Test Fiche : V7.0 — E5 (Tap OUT post-FX)

**Date** : 2026-05-11
**Statut** : **GO** — preuve empirique post-FX (PGA modulation visible sur tap-out)
**Tag git associé** : `v7.0-e5` (posé après OUI)

## Référentiel

| Champ | Valeur |
|---|---|
| Phase | V7.0 — E5 Tap OUT post-FX (signal play après multiband_drc + pga) |
| Préalable | E4 GO (tag `v7.0-e4`, commit yocto `d2a9808f`, commit SOF `6ee842c67`) |
| Branche yocto | `feature/v7.0-multiband-drc-tap` |
| Branche SOF | `feature/v7.0-multiband-drc-tap` |
| HEAD SOF (E5) | `6ee842c67` (inchangé — plomberie tap-out dual-tap déjà en place depuis E4) |
| Firmware md5 | `d1cb6c2d15c2b2a4f357da506d6b1a92` (inchangé E4) |
| DTB md5 | `d95cf917b71ea6229703c0acfdc7457a` (inchangé E4) |
| imx-audio-tap.ko md5 | `460d1f7943ec2f786a310f6dd0195777` (inchangé E4) |
| Topology .tplg md5 | `0363fd2806b9d2adfbd4f4cf994dc6e9` (inchangé E3) |

## Particularité E5

E5 = validation empirique **sans modif code**. Toute la plomberie firmware/kernel a été livrée en E4 (hook playback existant V3.2.2 préservé dans le refactor dual-tap + sentinelle `npu_tap_out_owner` + reserved-memory `tap_out_buffer@942B0000` + node DT `imx_audio_tap_out` + device `/dev/imx-audio-tap-out`). L'étape E5 démontre que le signal capté à cet endroit est bien **post-effets DSP play** (multiband_drc + pga 8 strips).

## Architecture validée

```
PIPE 2 play (E3 + hook tap-out @ E4-merge V3.2.2) :
  PCM 1 (sine 440Hz -6dB)
     ↓
  multiband_drc(8 ch indép)         ← compression dynamique (coeffs default)
     ↓
  pga(8 strips OUT)                 ← volumes kcontrols ALSA pilotés userspace
     ↓
  dd->local_buffer (post-pipeline)
     ↓
  dma_buffer_copy_to → dd->dma_buffer
     ↓
  [HOOK dai_dma_cb tap-out]         ← copy mirror vers tap_out_buffer @0x942B0000
     ↓                                       ↓
  SAI7 TX (haut-parleurs)         /dev/imx-audio-tap-out (mmap A53)
```

## Travaux exécutés

| Domaine | Action |
|---|---|
| Code | **Aucune modification** — la plomberie est en place depuis E4 |
| App test | `npu_tap_reader --device /dev/imx-audio-tap-out` (option E4) — utilisable tel quel |
| Test data | `/root/tests/sine440_8ch.wav` généré par `sox -n -c 8 -r 48000 -b 32 -e signed synth 5 sine 440 vol 0.5` (5 s, S32_LE, 8 ch, -6 dBFS) |

## Tests T5.X

| Test | Description | Résultat | Critère GO |
|---|---|---|---|
| **T5.1** | Build firmware (no source change) | ✓ N/A | — |
| **T5.2** | `/dev/imx-audio-tap-out` présent au boot | ✓ OK (créé par module multi-instance E4) | device présent |
| **T5.3** | Magic NPAT visible sous aplay | ✓ OK (version=4, ring_size=261120, period=3072, ch=8) | header valide |
| **T5.4** | Tap-out montre la chaîne strips OUT | ✓ **OK** — modulation PGA Strip1 (-40 dB via amixer sset) → tap-out peak passe de -6.02 dB → -46.03 dB (delta exact -40 dB) | tap-out ≠ entrée mixer |
| **T5.5** | Tap-IN + tap-OUT simultanés sans conflit | ✓ OK (3 s parallèle : in 3.7 MB, out 3.9 MB, 0 race) | 2 ring buffers indépendants OK |
| **T5.6** | Epoch increment au redémarrage pipeline | ✓ OK (epoch reset détecté quand aplay re-démarre) | R1 ordering préservé |
| **T5.7** | 0 régression loopback E3 | ✓ OK (test user E4 « ça marche » couvre E5 — même firmware/kernel) | son loopback inchangé |
| **T5.8** | Test utilisateur — wav tap-out audible avec voix/sinus | ⏳ à valider sur écoute (wav 8 ch S32_LE 48 kHz récupérable via scp) | « le wav contient bien le son traité » |

## Mesure empirique post-FX (preuve T5.4)

**Setup** : `aplay -D hw:softac5212tdm,0 sine440_8ch.wav` (sinus -6 dBFS, 8 ch) en parallèle de `npu_tap_reader --device /dev/imx-audio-tap-out --dump tap_out.wav --time 3`.

| Configuration | Peak tap-out (sox) | RMS tap-out | Delta vs source -6 dBFS |
|---|---|---|---|
| PGA Strip1 = 50 (0 dB nominal) | -6.02 dB | -9.08 dB | 0 dB (preservé) |
| PGA Strip1 = 10 (-40 dB) | **-46.03 dB** | **-49.08 dB** | **-40 dB** (PGA atténuation visible) |

→ Variation de 40 dB du PGA Strip1 produit exactement -40 dB de variation sur le tap-out. **Preuve éclatante** que le tap-out capture les samples APRÈS la pipeline DSP (post-multiband_drc + post-pga), pas avant.

## Tap-IN vs Tap-OUT simultanés (T5.5)

Avec `arecord` (mics ambiants) + `aplay sine440` simultanés et 2 instances `npu_tap_reader` (`-in` + `-out`) lancés en parallèle 3 s :

| Stream | Peak | RMS | Contenu |
|---|---|---|---|
| `/dev/imx-audio-tap-in` (cap brut) | -81 à -99 dB | -94 à -114 dB | Plancher de bruit micros (pas de signal externe injecté) |
| `/dev/imx-audio-tap-out` (play post-FX) | -6.02 dB | -9.19 dB | Sine 440 Hz post-DSP visible sur 8 voies |

→ 2 ring buffers indépendants, contenus radicalement différents, **aucune contamination cross-tap**. Sentinelles `npu_tap_in_owner` + `npu_tap_out_owner` indépendantes fonctionnent.

## Notes techniques

### Pourquoi E5 ne touche pas le code

Le hook firmware playback existait déjà V3.2.2 (`dai_dma_cb` branche PLAYBACK lignes 131-237 de `dai-legacy.c`). E4 a fait 2 choses :
1. Renommé la sentinelle globale `npu_tap_owner` → `npu_tap_out_owner` (sémantique sans changement comportemental)
2. Ajouté une **branche capture** miroir pour le tap-in

La plomberie tap-out V3.2.2 a été préservée à l'identique côté logique copy. E5 = juste valider empiriquement.

### Mapping kcontrol Strip1 et 8 canaux

Curiosité observée : modulation `PGA2.0 2 Out Strip1` atténue les 8 voies du tap-out (pas seulement la voie 1). Hypothèse : le SOF pga component interprète mal le channel-map FL/FR↔SOF channel-id, et applique le scale Strip1 sur les 8 voies. C'est cohérent avec le verdict E3 T3.4 « blob 8ch dupliqué, 8 configs identiques — infra OK, différenciation en E3.b ». La preuve post-FX reste valide indépendamment de cette curiosité (le scale est observable).

À investiguer en E3.b (futur sprint).

### Sox stats : 9 colonnes pour 8 ch

sox affiche 8 colonnes par canal + 1 colonne globale (somme/moyenne). C'est attendu.

## Conclusion

- **Tap-OUT post-FX** : empiriquement validé (modulation PGA -40 dB visible exactement)
- **Tap-IN + Tap-OUT simultanés** : OK (2 ring buffers indépendants, contenus distincts)
- **Aucune régression** : firmware/kernel inchangés vs E4
- **Plomberie complète** dual-tap fonctionnelle bout-en-bout DSP→A53

**Action immédiate** : commit fiche + ARCHI E5 GO + tag `v7.0-e5`. Si test utilisateur écoute (T5.8) confirme OUI, on passe à **E6 (USB gadget 8×8 + téléphone 2×2 dans le mixer Linux)**.

## Annexes

- App user E4 (réutilisée) : `meta-local/audio-tools/npu_tap_reader.c`
- Test data sur board : `/root/tests/sine440_8ch.wav` (généré par sox)
- Reproductibilité (commandes sur board) :
  ```bash
  cd /root/tests
  # Generate test signal once
  sox -n -c 8 -r 48000 -b 32 -e signed sine440_8ch.wav synth 5 sine 440 vol 0.5

  # Capture tap-out under aplay
  (./npu_tap_reader --device /dev/imx-audio-tap-out --dump tap_out.wav --time 3 &)
  sleep 0.3
  aplay -D hw:softac5212tdm,0 sine440_8ch.wav
  sox tap_out.wav -n stats 2>&1 | grep -E "Pk lev|RMS lev"

  # Modulate PGA Strip1 -40 dB to prove post-FX
  amixer -c softac5212tdm sset "PGA2.0 2 Out Strip1" 10
  # repeat capture
  ```
