# CR Plugins LV2 — classification + bench CPU (V9.5.21)

Date : 2026-06-17  ·  Board i.MX8MP, audio_thread RT99 core 2, période 2 ms (budget 2000 µs)

Baseline insert vide : **126.0 µs**.  345 plugins RT, 316 testés (hors multi-canaux purs ai/ao>2).

## Classification (345 plugins)

| Catégorie | Nombre | Sélectionnable insert |
|---|---|---|
| effet | 285 | 264 (mono 1/1 ou stéréo 2/2) |
| graphique (meters/analyseurs) | 47 | non (visualisation) |
| instrument (synthés/samplers) | 9 | non (pas d'entrée audio) |
| midi (séquenceurs) | 4 | non (pas d'audio) |

## ⚠ Crashes mixer-pro (2 réels + 3 faux positifs)

Re-test individuel (2026-06-17) : **seuls les 2 meters crashent vraiment**.
Les 3 LSP étaient des FAUX POSITIFS du bench — plugins si lourds (1400-1500 µs)
que `get_state` dépassait le timeout 3 s → le bench croyait à un crash. Re-testés
seuls : `sc_mb_gate_mono` 1499 µs OK, `mb_compressor_stereo` 1397 µs OK,
`multisampler_x24_stereo` refusé proprement. Aucun ne plante.


| Plugin | Quand | Cause racine |
|---|---|---|
| Signal Distribution Histogram | run | assertion `capacity>920` sur port atom out (meters.lv2) — bug host buffer atom |
| Bit Meter | run | idem (meters.lv2) |
| LSP Multiband Compressor Stereo x8 | load | plugin lourd (>1100 µs), SIGSEGV/alloc |
| LSP Multi-Sampler x24 Stereo | run | sampler lourd, SIGSEGV/alloc |
| LSP Sidechain Multiband Gate Mono x8 | run | plugin lourd, SIGSEGV/alloc |

Meters (Histogram, Bit Meter) → déjà non-sélectionnables (catégorie graphique)
→ **② résolu par la catégorisation**. Bug host résiduel : assertion atom out
`capacity>920` (n'affecte que ces meters, non utilisables en insert de toute façon).

## Top 25 CPU (effets)

| µs | % budget | Plugin |
|---|---|---|
| 2690 | 135% | LSP Multi-Sampler x48 Stereo |
| 1301 | 65% | LSP Sidechain Multiband Expander MidSide x8 |
| 1284 | 64% | LSP Sidechain Multiband Compressor MidSide x8 |
| 1250 | 62% | LSP Sidechain Multiband Expander LeftRight x8 |
| 1226 | 61% | LSP Multiband Expander MidSide x8 |
| 1223 | 61% | LSP Multiband Gate MidSide x8 |
| 1222 | 61% | LSP Sidechain Multiband Expander Mono x8 |
| 1209 | 60% | LSP Multiband Expander LeftRight x8 |
| 1198 | 60% | LSP Multiband Expander Mono x8 |
| 1188 | 59% | LSP Multiband Compressor Mono x8 |
| 1187 | 59% | LSP Sidechain Multiband Compressor Mono x8 |
| 1184 | 59% | LSP Sidechain Multiband Gate LeftRight x8 |
| 1182 | 59% | LSP Sidechain Multiband Gate MidSide x8 |
| 1177 | 59% | LSP Multiband Gate Mono x8 |
| 1174 | 59% | LSP Multiband Gate Stereo x8 |
| 1172 | 59% | LSP Multiband Compressor LeftRight x8 |
| 1169 | 58% | LSP Sidechain Multiband Expander Stereo x8 |
| 1166 | 58% | LSP Sidechain Multiband Gate Stereo x8 |
| 1164 | 58% | LSP Sidechain Multiband Compressor LeftRight x8 |
| 1162 | 58% | LSP Multiband Expander Stereo x8 |
| 1158 | 58% | LSP Multiband Compressor MidSide x8 |
| 1154 | 58% | LSP Multiband Gate LeftRight x8 |
| 1138 | 57% | LSP Sidechain Multiband Compressor Stereo x8 |
| 1108 | 55% | LSP Sidechain Limiter Stereo |
| 1101 | 55% | LSP Sidechain Limiter Mono |

## Répartition CPU des effets

| Tranche µs | Nombre |
|---|---|
| 0–20 | 11 |
| 20–50 | 71 |
| 50–100 | 87 |
| 100–200 | 35 |
| 200–500 | 24 |
| >500 | 29 |

## Effets légers recommandés (<30 µs)

B.Spacr, Simple Amplifier, Vihda, Example MIDI Gate, ZamHeadX2, GxSlowGear, LSP Delay Compensator Stereo, ZamAutoSat, LSP Delay Compensator Mono, LSP Profiler Stereo, Bitta, Masha, Whaaa, LSP Loudness Compensator Mono, LSP Impulse Responses Stereo, LSP Impulse Responses Mono, GxMicroAmp, LSP Loudness Compensator Stereo, LSP Profiler Mono

## Refusés proprement (17 — pas de crash)

Calf Monosynth, Calf Organ, Calf Wavetable, MDA DX10, MDA ePiano, MDA JX10, MDA Piano, Example Fifths, Example Metronome, Example Parameters, B.Low, B.SEQuencer, B.Schaffl, ZaMultiComp, ZamComp, ZamDynamicEQ, ZamGate
