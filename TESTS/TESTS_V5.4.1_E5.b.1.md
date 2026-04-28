# Test Fiche : V5.4.1 — E5.b.1 quick-fix buffers 8ch

**Date** : 2026-04-28
**Statut** : NOK — bug A seul ne suffit pas, autres causes en jeu

## Référentiel

| Champ | Valeur |
|---|---|
| Phase de référence | Phase 1a.3 |
| Version de référence | V5.4.1 |
| Étape | E5.b.1 — quick-fix deepseek (buffers B1..B8 en 8ch au lieu de mono) |
| Commit SOF | `a3dee1137` (E5.a topology) + modif locale buffers 8ch |
| Topology test md5 | `47fbce730d5a6b55535e94c807a234e6` (10968 B) |
| Firmware sof-imx8m.ri md5 board | `997978174217aa0b505e54f601afa5c0` (E4 inchangé) |

## Hypothèse testée

**Bug A (deepseek B1)** : `comp_verify_params()` force PIPELINE_CHANNELS=8 sur tous les buffers via `BUFFER_UPDATE_FORCE`, écrasant la définition mono des B1..B8. Quick-fix : déclarer B1..B8 directement en 8ch dans le m4 (mes comps lisent slot 0 quand mono).

## Modification appliquée

Dans `pipe-deinterleave-interleave-capture.m4`, B1..B8 :
```diff
W_BUFFER(N, COMP_BUFFER_SIZE(2,
-    COMP_SAMPLE_SIZE(PIPELINE_FORMAT), 1,
+    COMP_SAMPLE_SIZE(PIPELINE_FORMAT), PIPELINE_CHANNELS,
    COMP_PERIOD_FRAMES(PCM_MAX_RATE, SCHEDULE_PERIOD)),
    PLATFORM_HOST_MEM_CAP)
```

## Tests réalisés

| # | Test | Résultat |
|---|---|---|
| E5.b.1.0 | Compile m4 + alsatplg | ✅ tplg 10968 B md5 `47fbce73...` |
| E5.b.1.1 | Deploy + reboot | ✅ board boot |
| E5.b.1.2 | Firmware version | "version 2:10:0-6f166" |
| E5.b.1.3 | Card 2 sof-tac5212-tdm | ✅ |
| E5.b.1.4 | arecord 8ch 2s | **❌ Input/output error** |
| E5.b.1.5 | Bytes capturés | 44 (WAV header seul) |
| E5.b.1.6 | Trigger error | `TAC5212: ASoC: trigger FE cmd: 0 failed: -110` |
| E5.b.1.7 | Restore V3.2.2 | ✅ |

## Test utilisateur

| Champ | Valeur |
|---|---|
| Test utilisateur effectué | **NON** |
| Type de test | — |
| Résultat | — |
| Commentaires | E5.b.1 NOK — pas testable utilisateur. V3.2.2 restauré. |

## Conclusion

**NOK E5.b.1.** Le quick-fix Bug A (buffers 8ch partout) ne résout pas l'échec runtime. Le trigger -110 persiste malgré l'élimination du mismatch buffer mono/8ch. Donc :
- **Bug A confirmé non-bloquant seul** : le format des buffers n'était pas la seule cause
- **Bug B (architecture P_GRAPH multi-sink dans 1 pipeline)** ou **Bug C (PPL_STATUS_PATH_STOP collision)** restent les causes probables
- Architecture **multi-pipelines avec cross-buffer sharing** (claude-code/glm-5.1/qwen) reste la solution recommandée

## Prochaine étape : E5.c

Refonte topology selon pattern crossover/demux SOF standard :
- Pipeline 1 (master, 8ch DMA) : `SAI7 RX → deinterleave_8 → 1 sink local + 7 sinks via PIPELINE_DEINTERLEAVE_1`
- Pipelines 2..8 (piggyback, sched_comp partagé) : 1 buffer mono chacun, exposé via `PIPELINE_BUFFER_<id>`
- `SectionGraph.dapm()` cross-pipeline pour binding sinks 2..8

Effort : ~200-300 lignes m4 (multi-pipelines + section graph). Session dédiée recommandée.

## Référence

- Investigation E5.a NOK : job `b245b22c-e3a8-415c-81fd-f6e896f399b0` (6 workers, 28 min)
- Pattern multi-pipelines : `sof-imx8mp-tac5212-masterV42-hpmon.m4:84-92` (référence projet)
- Fiche précédente : `TESTS_V5.4.1_E5.a.md`
