# Test Fiche : V5.4.1 — E5.a runtime test deinterleave_8 + interleave_8 round-trip

**Date** : 2026-04-28
**Statut** : NOK runtime — investigation requise

## Référentiel

| Champ | Valeur |
|---|---|
| Phase de référence | Phase 1a.3 |
| Version de référence | V5.4.1 |
| Étape | E5.a — round-trip SAI capture 8ch → deinterleave_8 → 8 mono → interleave_8 → host PCM 8ch |
| Commit SOF | `a3dee1137` (branch `feature/audio-platform-v2`) |
| Commit yocto-nxp-debix | `ccd53966` (.gitignore safety net) |
| Topologie test | `sof-imx8mp-tac5212-V5.4.1-E5a.m4` (compile OK, deploy KO runtime) |
| Topology .tplg test md5 | `3614cbc0221d0f11676906ec4596673c` (10968 B) |
| Firmware sof-imx8m.ri md5 board | `997978174217aa0b505e54f601afa5c0` (E4 inchangé) |

## Architecture testée

```
SAI7 RX 8ch → B0 → deinterleave_8 → B1..B8 (8 mono) → interleave_8 → B9 → host PCM 8ch
```

P_GRAPH dapm avec 8 sinks parallèles depuis deinterleave_8 vers 8 buffers mono, puis 8 sources parallèles depuis ces buffers vers interleave_8.

## Tests réalisés

| # | Test | Résultat |
|---|---|---|
| E5.a.0 | Compile m4 + alsatplg | ✅ tplg 10968 B md5 `3614cbc0...` |
| E5.a.1 | Deploy + reboot | ✅ board boot |
| E5.a.2 | Firmware version | "version 2:10:0-6f166" (cache hash) |
| E5.a.3 | Card 2 sof-tac5212-tdm | ✅ présent |
| E5.a.4 | arecord 8ch 2s | **❌ Input/output error** |
| E5.a.5 | Bytes capturés | 44 (WAV header seul, 0 samples) |
| E5.a.6 | Trigger error | `TAC5212: ASoC: trigger FE cmd: 0 failed: -110` (-ETIMEDOUT) |
| E5.a.7 | Restore V3.2.2 | ✅ tplg `7366ff30...` restauré |

## Diagnostic

**Hypothèses sur la cause** :

1. **dapm multi-sinks pattern non-standard** : SOF P_GRAPH avec 8 dapm() depuis le même comp source vers 8 buffers parallèles n'est probablement pas reconnu standard par alsatplg/kernel comme un comp 1×8. Le widget bind module_adapter pourrait n'allouer qu'1 sink, refuser les 7 autres.

2. **Format mismatch buffers mono dans pipeline 8ch** : `PIPELINE_CHANNELS=8` global, mais B1..B8 alloués à 1ch via 4ème argument `COMP_BUFFER_SIZE`. Le module_adapter peut imposer même format partout.

3. **deinterleave_8 process pas appelé** : si le framework refuse 8 sinks au prepare, `mod->max_sinks` reste à 1, le 2e sink connecté lève `-EINVAL` à `module_adapter.c:806`. Process jamais appelé → trigger timeout.

4. **interleave_8 process pas appelé** : symétrique, framework refuse 8 sources connectées.

## Test utilisateur (réel, in-the-loop)

| Champ | Valeur |
|---|---|
| Test utilisateur effectué | **NON** |
| Type de test | — |
| Résultat | — |
| Commentaires | E5.a NOK runtime — pas de test utilisateur possible. Topology restaurée V3.2.2. |

## Logs significatifs

```
[10.422118] sof-audio-of-imx8m 3b6e8000.dsp: Firmware info: version 2:10:0-6f166
arecord: pcm_read:2272: read error: Input/output error
TAC5212: ASoC: trigger FE cmd: 0 failed: -110

44 bytes captured (WAV header only, 0 sample bytes)
```

## Conclusion

**NOK E5.a.** La pipeline `pipe-deinterleave-interleave-capture` ne s'instancie pas correctement runtime malgré compile OK. Trigger -ETIMEDOUT indique que le pipeline ne démarre pas.

**Investigation à faire avant E5.b** :
1. Lire le code module_adapter.c pour voir comment 1 source × N sinks est géré au prepare
2. Lire le code framework de bind buffers pour voir si 8 dapm sinks parallèles depuis 1 comp sont supportés
3. Vérifier si interleave_8/deinterleave_8 attendent des paramètres de config absents (blob, channel_map)
4. Tester pattern simplifié (1 src × 1 sink, sans le 8×) pour isoler

**Alternatives à explorer** :
- Multi-pipelines avec cross-pipeline buffer sharing (pattern plus standard SOF mais non-validé H2)
- Pattern smart_amp avec multi-sink déjà documenté upstream
- Faire le test en single-sink d'abord (deinterleave_8 avec 1 sink mono connecté, 7 inutilisés)

## Référence

- Spec : `PHASE_1A_3_DSP_V5.4.1.md`
- Pattern m4 référence : `pipe-multiband-drc-playback.m4` (single src × single sink fonctionne)
- Fiche précédente : `TESTS_V5.4.1_E4.deinterleave.md`
