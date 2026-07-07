# Test Fiche : V5.4.1 — E5.c.1 multi-pipelines cross-buffer (1+1)

**Date** : 2026-04-28
**Statut** : NOK — route cross-pipeline non acceptée par firmware

## Référentiel

| Champ | Valeur |
|---|---|
| Phase de référence | Phase 1a.3 |
| Version de référence | V5.4.1 |
| Étape | E5.c.1 — multi-pipelines cross-buffer pattern (master deinterleave + 1 piggyback) |
| Topology test md5 | `c546b3cfdbf67cd0d03841ab7dd2c047` (9812 B) |
| Firmware sof-imx8m.ri md5 board | `997978174217aa0b505e54f601afa5c0` (E4 inchangé) |

## Architecture testée

```
Pipeline 1 (master, capture SAI 8ch) :
  SAI7 RX → B0(8ch) → deinterleave_8 → B1(1ch local) → host PCM 0 (mono)
                              │
                              └── (sink #2 exposé via PIPELINE_DEINTERLEAVE_1)

Pipeline 2 (piggyback, sched_comp partagé) :
  B0_pipe2 (mono) → host PCM 1 (mono)

Pipeline 3 (playback V3.2.2-like) :
  host → volume → SAI7 TX 8ch

SectionGraph cross-pipeline :
  dapm(PIPELINE_BUFFER_2, PIPELINE_DEINTERLEAVE_1)  # bind sink #2 → buffer pipeline 2
```

## Erreur runtime

```
sof_ipc3_route_setup: route BUF2.0 -> PCM1C failed
ipc tx error for 0x30030000 (msg/reply size: 16/0): -22
error: tplg component load failed -22
asoc-simple-card sof-sound-tac5212: ASoC: failed to instantiate card -22
```

## Cause probable

La route locale `dapm(N_PCMC(PCM_ID), N_BUFFER(0))` dans le P_GRAPH de pipeline 2 échoue parce que le buffer `B0_pipe2` n'a pas de producer DANS pipeline 2 (le producer = `deinterleave_8.sink#2` est dans pipeline 1, via cross-pipeline). Le firmware SOF IPC3 refuse cette route car la source du buffer n'est pas locale ni encore résolue au moment de l'établissement de la route.

## Tests réalisés

| # | Test | Résultat |
|---|---|---|
| E5.c.1.0 | Compile m4 + alsatplg | ✅ tplg 9812 B md5 `c546b3cf...` |
| E5.c.1.1 | Deploy + reboot | ❌ tplg load failed at boot |
| E5.c.1.2 | Card 2 sof-tac5212-tdm | ❌ pas instantiée (tplg load err -22) |
| E5.c.1.3 | arecord | N/A (no card) |
| E5.c.1.4 | Restore V3.2.2 | ✅ |

## Test utilisateur

| Champ | Valeur |
|---|---|
| Test utilisateur effectué | **NON** |
| Type de test | — |
| Résultat | — |
| Commentaires | E5.c.1 NOK — tplg ne loade pas. V3.2.2 restauré. |

## Conclusion

**NOK E5.c.1.** Le pattern multi-pipelines avec cross-buffer naïf ne marche pas tel quel. Le firmware refuse la route `BUF (cross-pipeline) → PCM` dans une pipeline secondaire si le buffer n'a pas de producer local.

**Pistes pour E5.c.2** :
1. Mettre la route `dapm(PCM, BUFFER)` dans le SectionGraph top-level (au lieu du P_GRAPH local) pour que firmware traite tout le binding ensemble
2. Étudier en détail un fichier exemple SOF qui marche avec cross-pipeline DAPM (ex: `sof-imx8mp-tac5212-masterV42-hpmon.m4` mentionné par claude-code)
3. Créer une investigation critic ciblée sur le pattern exact de SectionGraph avec PCM-host cross-pipeline (pas juste demux→DAI)

## Référence

- Investigation E5.a NOK : job `b245b22c`
- Pattern référence : `pipe-volume-demux-playback.m4` (mais le sink supplémentaire de demux va vers une autre pipeline + DAI, pas vers un PCM host)
- Fiche précédente : `TESTS_V5.4.1_E5.b.1.md`
