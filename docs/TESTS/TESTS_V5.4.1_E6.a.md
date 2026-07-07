# Test Fiche : V5.4.1 — E6.a

**Date** : 2026-05-02
**Statut** : GO

## Référentiel

| Champ | Valeur |
|---|---|
| Phase de référence | Phase 1a.3 |
| Version de référence | V5.4.1 |
| Étape | E6.a — matrix 16×8 PIPE 2 play (Phase A passthrough, identity matrix) |
| Commit SOF (patch + tplg) | `0580b5f14` (branch `feature/audio-platform-v2`, github fork Mjxkill/sof) |
| Topologie de référence | `sof/tools/topology/topology1/sof-imx8mp-tac5212-V5.4.1-E6a.m4` |
| Pipe inclus | `sof/tools/topology/topology1/sof/pipe-matrix-playback.m4` |
| Firmware sof-imx8m.ri md5 board | `3a2a7d1969303ba640b3680811728bbd` |
| Topology .tplg md5 board | `2ab3b7823f483e600acead5c8352a128` |

## Architecture validée

```
PIPE 1 cap (E5.e.2-D3 inchangé) :
  SAI7 RX 8ch → eq_iir(8ch, 8 EQ indép) → drc(8ch, 8 DRC indép D3)
              → pga(8ch, 8 vols indép) → host PCM 0 (ASIO IN)

PIPE 2 play (E6.a nouveau) :
  PCM 1 (ASIO play 8ch) → B0(8ch) → deinterleave_8 → 8 mono ASIO play
                       → mixer16 (16 sources × 8 sinks, identity matrix par défaut)
                       →  8 mono outputs → interleave_8 → B100(8ch) → SAI7 TX 8ch
```

**Caractéristiques** :
- Mixer16 = 1 instance avec matrice 16×8 Q1.31 (128 cellules, 512 octets blob)
- Identity matrix par défaut : sources [0..7] (ASIO play) → sinks [0..7] passthrough, sources [8..15] (mics) à gain=0 (mute)
- Sources [8..15] = mics post-strips-IN, **NON CONNECTÉES** Phase A (E6.b future via cross-pipeline tap)
- Buffers mono branched intra-PIPE 2 lockés à 1ch automatiquement par F++ auto-detection (commit 7499505ec, pipeline_complete walk #2)

## Bug corrigé

E6.a a révélé un bug latent dans `pipeline_comp_copy` (pipeline-stream.c) :
- `pipeline_for_each_comp` utilise un flag `buffer->walking` per-buffer reset après récursion
- Protection contre cycle via *même buffer* mais pas re-entry via *chemins alternatifs*
- Pour split/merge intra-pipeline (deinterleave_8 + mixer16 + interleave_8) : explosion combinatoire 137 appels/tick au lieu de 4 → DSP saturé → DMA TX starve à 3840 fps

**Fix** : guard `copy_seq` per-tick (`uint32_t` dans `comp_dev` + `pipeline_data`, compteur monotone global incrémenté par `pipeline_copy`). Court-circuit re-entry sur même comp dans le même tick.

ABI/comportement strict back-compat : topologies linéaires single-fanout = 1 visite par comp par tick (inchangé). Cross-pipeline déjà protégé par `is_single_ppl`. Le guard ne déclenche QUE sur les topologies multi-fanout intra-pipeline.

## Investigation critic

| Job | Type | Workers | Résultat |
|---|---|---|---|
| `37e42ffb` | diagnostic 6 workers | 5 actifs | 3 hypothèses (claude-code récursion / qwen channels lock / kimi race source-sink) |
| `8c10cabf` | arbitrage 6 workers | 6 actifs | **5/6 confirment H1 (récursion combinatoire)**, math exact 3840 fps. H2 + H3 explicitement éliminées par lecture code |

## Devices ALSA + audio

| Device | Card | Rôle | Format |
|---|---|---|---|
| `hw:softac5212tdm,0` cap | 2 | ASIO IN 8ch (PCM 0) | S32_LE 48 kHz 8 ch |
| `hw:softac5212tdm,1` play | 2 | ASIO OUT 8ch (PCM 1) | S32_LE 48 kHz 8 ch |

## Tests réalisés (Claude — automatisés sur board)

### Régression E6.a (avant vs après patch)

| Test | Avant patch (firmware D3 5320d61d) | Après patch (firmware 3a2a7d19) |
|---|---|---|
| in fps | ~47 000 | ~47 000 |
| **out fps** | **3 840** (starved) | **~47 000** (steady) |
| xrun_play | 16+ croissant | 7 stable |
| ring_drop | 150+ croissant | 74 stable |

Facteur ×12.4 de récupération sur le débit playback = match exact prédiction H1.

### Régression E5.e.2-D3 sur firmware patché

```
in=47872 (+47872 f/s) out=41984 (+41984 f/s) xrun cap=0 play=2 ring_drop=21
in=96000 (+48128 f/s) out=89856 (+47872 f/s) xrun cap=0 play=2 ring_drop=21
in=140288(+44288 f/s) out=131584(+41728 f/s) xrun cap=1 play=3 ring_drop=32
in=188416(+48128 f/s) out=179456(+47872 f/s) xrun cap=1 play=3 ring_drop=32
```

Identique à avant le patch — **aucune régression** sur topologie linéaire.

## Test utilisateur (réel, in-the-loop)

| Champ | Valeur |
|---|---|
| Test utilisateur effectué | OUI |
| Type de test | écoute audio temps réel via loopback-c (matrix passthrough) |
| Résultat | « le son est bon, drops rares » |
| Commentaires | Audio correct, quelques micro-drops occasionnels suspects côté Linux (ring_drop dans loopback-c). User estime que c'est à corriger plus tard côté ALSA host (cf TODO "ALSA low latency sprint" — MMAP + SCHED_FIFO + mlockall). |

## Logs significatifs

Boot firmware E6.a (extrait dmesg) :
```
sof-audio-of-imx8m 3b6e8000.dsp: DT DSP detected
sof-audio-of-imx8m 3b6e8000.dsp: Firmware: ABI 3:29:0 Kernel ABI 3:23:0
sof-audio-of-imx8m 3b6e8000.dsp: tplg: config SAI7 fmt 0x4004 mclk 12288000 width 32 slots 8 mclk id 0
tac5212 3-0050: TAC5212 initialized (I2C 0x50, slots 0-1)
```

loopback-c output post-patch (steady-state) :
```
in=47872 (+47872 f/s) out=27648 (+27648 f/s) xrun cap=0 play=7 ring_drop=73
in=96000 (+48128 f/s) out=75264 (+47616 f/s) xrun cap=0 play=7 ring_drop=74
in=143872(+47872 f/s) out=123136(+47872 f/s) xrun cap=0 play=7 ring_drop=74
```

## Conclusion

V5.4.1 E6.a **GO**. La matrix 16×8 (mixer16 component existant) fonctionne en intra-pipeline avec deinterleave_8/interleave_8 grâce au fix `copy_seq` re-entry guard. Identity matrix par défaut → passthrough ASIO play 8ch vers SAI TX (mics input non connectés Phase A).

Capacités V5 jusqu'à E6.a :
- 8 EQ IIR indépendants par voie cap (E5.e.2 step1)
- 8 DRC indépendants par voie cap (E5.e.2-D3 patch)
- 8 Volumes indépendants par voie cap (E5.e.2 step1)
- **16×8 matrix routing en playback** (E6.a, mixer16 + fix copy_seq)

## Étapes suivantes possibles

- **E6.b** : cross-pipeline tap mics (post-strips-IN cap) → matrix sources [8..15]. Risque -22 historique à évaluer empiriquement.
- **E6.c** : 128 ALSA controls weights ou 1 control bytes 512 octets pour configurer la matrice user-side.
- **E7** : 8 strips OUT (multiband_drc + pga + drc) après matrix outputs → SAI TX.
- **ALSA low latency sprint** (mémoire `project_alsa_low_latency_todo.md`) : MMAP + SCHED_FIFO + mlockall pour réduire les drops Linux-side observés en loopback-c.

## Référence (autres docs/spec liées)

- Investigation critic :
  - `37e42ffb` (diagnostic initial, 5/6 workers)
  - `8c10cabf` (arbitrage 3 hypothèses, 6/6 workers, **5/6 confirment H1**)
- Fiches précédentes : `TESTS_V5.4.1_E5.e.1.md`, `TESTS_V5.4.1_E5.e.2-step1.md`, `TESTS_V5.4.1_E5.e.2-D3.md`
- Mémoires : `sof_pivot_1comp_8ch.md`, `sof_drc_d3_per_channel.md`, `sof_e5e1_optionFpp_validated.md`
