# Test Fiche : V5.4.1 — E5.d passthrough 8ch via deinterleave_8 + interleave_8

**Date** : 2026-04-28
**Statut** : OK runtime — capture 8ch sans erreur

## Référentiel

| Champ | Valeur |
|---|---|
| Phase de référence | Phase 1a.3 |
| Version de référence | V5.4.1 |
| Étape | E5.d — passthrough 8ch via deinterleave_8 + interleave_8 (1 seule pipeline, sans cross-pipeline) |
| Commit de référence | `54f6cd35` |
| Topology m4 md5 | `b1bdfa00b5e0dfada2d7b25998809b5e` |
| Topology tplg md5 | `fa41ef7b5bf7c30ccad99961d36bfdd1` (11880 B) |
| Firmware sof-imx8m.ri md5 | `78347079f8ec69b00a61d260b698b549` |

## Architecture testée

```
Pipeline 1 (capture, 8ch) :
  SAI7 RX 8ch → B0(8ch) → deinterleave_8 → B1..B8(mono internes) → interleave_8 → B9(8ch) → host PCM 0 (ASIO_IN 8ch)

Pipeline 2 (playback, 8ch — V3.2.2 baseline) :
  host PCM 1 (ASIO_OUT 8ch) → volume → SAI7 TX 8ch
```

→ Conforme cahier des charges Phase 1a.3 : **2 PCMs ALSA tous 8ch**, les 8 mono restent internes au DSP.

## Patches firmware appliqués

1. **deinterleave_8.c** : ajout `.trigger` callback PRE_START → `audio_stream_set_overrun(true)` sur sinks cross-pipeline (pattern `mux.c:demux_trigger:437-460`)
2. **deinterleave_8_prepare** : override channels via `audio_stream_set_channels` (source=8, sinks=1) après framework propagation
3. **interleave_8_prepare** : override channels (sources=1, sink=8)
4. **tee_1to2.c** : trigger overrun_permitted + prepare preserve source channels (utile pour étapes ultérieures)

## Tests réalisés

| # | Test | Résultat |
|---|---|---|
| E5.d.0 | Compile m4 + alsatplg | ✅ tplg 11880 B md5 `fa41ef7b...` |
| E5.d.1 | Build firmware + sign rimage (Reef à 0x2e0) | ✅ md5 `78347079...` |
| E5.d.2 | Deploy + reboot | ✅ tplg load OK |
| E5.d.3 | Card 2 sof-tac5212-tdm instantiée | ✅ ASIO_IN (cap 8ch) + ASIO_OUT (play 8ch) |
| E5.d.4 | `arecord -Dhw:2,0 -f S32_LE -c 8 -d 2` | ✅ 3 MB capturés, aucune erreur dmesg |
| E5.d.5 | Aucune erreur `ipc tx error` ni `hw_params failed` | ✅ |

## Déblocages obtenus vs E5.a/b/c

| Bug précédent | Status E5.d |
|---|---|
| E5.a `-110 timeout` (8 dapm sinks dans 1 pipeline) | **RÉSOLU** — fan-out 8 sinks fonctionne après patches firmware |
| E5.b.1 `-110 timeout` (buffers 8ch quick-fix) | **RÉSOLU** par override channels in prepare |
| E5.c.1 `-22 EINVAL route_setup` (cross-pipeline naïf) | Évité — pas de cross-pipeline en E5.d |
| E5.c.2 `-22 hw_params` (PCM mono vs DAI 8ch) | Évité — PCMs ALSA tous 8ch en E5.d |

## Test utilisateur

| Champ | Valeur |
|---|---|
| Test utilisateur effectué | **OUI (2026-04-28)** |
| Type de test | Plein duplex 8ch — `aplay -Dhw:2,1 /tmp/siren_8ch.wav` (siren stéréo upmixé 8ch via sox) en parallèle de `arecord -Dhw:2,0 -c 8 -d 6 /tmp/asio_in_siren.wav` |
| Résultat | ✅ Play OK, Cap 9.2 MB capturés en 6s (taille théorique 9.216 MB ✅), aucune erreur dmesg, aucun XRUN/underrun |
| Test audibilité utilisateur final | À faire par l'utilisateur (récupération wav `scp ... /tmp/asio_in_siren.wav .` + écoute) |
| Test bit-perfect vs V3.2.2 | À faire (compare capture E5.d avec capture V3.2.2 baseline sur même signal d'entrée) |
| Commentaires | E5.d valide la chaîne deinterleave_8 → 8 mono internes → interleave_8 en duplex 8ch sans erreur. Architecture conforme cahier des charges (PCMs ALSA tous 8ch, mono internes DSP). |

## Prochaines étapes

E5.d valide les briques de base (deinterleave_8 + interleave_8 + max_sinks=8 + override channels in prepare).
Suite : insérer les 8 strips IN (eq_iir + drc + 2× volume) entre `deinterleave_8` et `interleave_8`.

## Références

- Fiches précédentes (NOK) : `TESTS_V5.4.1_E5.a.md`, `E5.b.1.md`, `E5.c.1.md`, `E5.c.2.md`
- Investigation `e2a7da86` (e5b multi-workers)
- Pattern référence trigger : `sof/src/audio/mux/mux.c:437-460` (demux_trigger)
- Pattern override channels in prepare : `sof/src/audio/pipeline/pipeline-params.c:42-49` commentaire
