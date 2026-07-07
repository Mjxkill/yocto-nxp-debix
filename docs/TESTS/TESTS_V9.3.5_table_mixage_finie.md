# TESTS V9.3.5 — Table de mixage finie

Date : 2026-06-05
Branche : `feature/v7.0-multiband-drc-tap`
Commits : 275b439d (#1) + f72d0137 (#2) + 56c5a392 (#4)
Version mixer-pro : `v9.3.5-presets`

## Objectif

Finir tous les TODOs « table de mixage » avant de passer à étape 1
(architecture insert mastering pour V10 NPU).

## TODOs

### ✅ #1 `lilv_port_get_range` exposé dans get_fx — V9.3.3

Bug : NPU ne pouvait pas normaliser ses prédictions sans les ranges
exacts des params LV2.

Fix : extension de `struct lv2_state` + `fx_init_lv2` + `lv2_get_state`.

Test board :
```bash
curl -X POST http://192.168.0.9:8080/api/cmd \
     -d '{"op":"set_fx_engine","bus":0,"engine":"lv2",
          "uri":"http://lsp-plug.in/plugins/lv2/compressor_stereo"}'
curl -X POST http://192.168.0.9:8080/api/cmd \
     -d '{"op":"get_fx","bus":0}' | jq
```

Résultat (extrait) :
```json
{
  "params": {"enabled":1, "g_in":1, "threshold":0.125, ...},
  "ranges": {
    "enabled": {"min":0, "max":1, "def":1},
    "g_in":    {"min":0, "max":1000, "def":1},
    "threshold": {"min":0, "max":1, "def":0.125},
    ...
  }
}
```

35 params + 35 ranges pour LSP Compressor Stereo. NaN/Inf → null JSON.

### ✅ #2 Worker LV2 pinning cores 0+1 — V9.3.4

Quick win RT. Worker threads LV2 spawnés sur sched_other free →
peuvent être schedulés sur core 2 et créer jitter audio_thread RT.

Fix : `pthread_attr_setaffinity_np` sur cpu_set `{0, 1}` avant
`pthread_create`.

Test board :
```bash
PID=$(pidof mixer-pro)
# Trouver worker thread
for tid in $(ls /proc/$PID/task/); do
  taskset -p $tid 2>/dev/null
done
```

Résultat : worker tid affinity = `0x3` (binaire 0011 = cores 0+1) ✓
Audio_thread mixer-pro = `0xc` (cores 2+3) ✓

### ⏭ #3 lv2:state extension — SKIPPÉ FACTUALISÉ

Investigation : capture des `Required Features` de 6 plugins LSP
potentiellement state-dépendants.

Résultat : 6/6 ont `state:mapPath` + `state:interface` en **OPTIONAL**,
pas required. Test load réel : 5/6 chargent OK sans implementation
host. Le 6e (`sampler_x12_stereo`) était URI inexistant.

Conclusion : extension non nécessaire pour la chaîne mastering V10.

### ✅ #4 Persistence presets JSON — V9.3.5

Save debounced 1s vers `/var/lib/mixer-pro/presets.json`.

Mécanisme :
- `atomic_int g_presets_dirty` flag
- `set_fx_engine` / `set_fx_param` → set flag
- `persistence_thread` : sleep 1s + `if exchange dirty` → `save_presets`
- Write atomique : tmp + rename

Test board :
```bash
curl -X POST http://192.168.0.9:8080/api/cmd \
     -d '{"op":"set_fx_engine","bus":0,"engine":"lv2",
          "uri":"http://lsp-plug.in/plugins/lv2/compressor_stereo"}'
sleep 2
cat /var/lib/mixer-pro/presets.json | jq
```

Résultat (extrait) :
```json
{
  "version": 1,
  "buses": [
    {"bus":0, "type":"lv2", "uri":"http://.../compressor_stereo",
     "params":{...}, "ranges":{...}},
    {"bus":1, "type":"reverb", ...},
    {"bus":2, "type":"delay", ...},
    {"bus":3, "type":"eq", ...}
  ]
}
```

Restore : NON implémenté en C (parser JSON ad-hoc fragile). Fichier sert
pour debug + NPU training + manual recovery via script bash futur.

### ⏭ #5 smooth_gains intra-block — SKIPPÉ FACTUALISÉ

Investigation : analyse des use cases changement de gain.

| Source de change | Fréquence max prévue |
|---|---|
| NPU | < 10 Hz (1 toutes 50 periods) |
| GUI slider rapide | < 50 Hz |
| LFO modulation | N/A (intra-plugin LV2, hors gain matrix) |

`GAIN_RAMP_FRAMES = 64` (ramp 1.33 ms @ 48 kHz) :
- Converge en < 1 period
- Pas de risque clic/pop perceptible pour tous les cas prévus

Conclusion : refactor en sous-blocs ajouterait lock contention +
complexité sans bénéfice mesurable.

### ⏭ #6 prof_cap_us 2.3 ms — SKIPPÉ FACTUALISÉ

Investigation : `top -H` + `wchan` des threads.

| Thread | wchan | Nature |
|---|---|---|
| audio_thread (RT prio 99) | `__snd_pcm_lib_xfer` | **WAIT** sur period |
| play_thread | `__snd_pcm_lib_xfer` | WAIT |
| cap_uac2 | `__skb_wait_for_more_packets` | WAIT USB |
| persistence_thread | `hrtimer_nanosleep` | WAIT |

Décomposition prof_iter_us :
- `prof_cap_us = 1797 µs` = blocking wait `snd_pcm_readi` (period 2 ms @ 48 kHz)
- `prof_mix_us = 184 µs` = CPU réel calcul mix (vs 519 µs V9.2g = -65%)
- `prof_push_us = 9 µs` = CPU push ring
- `prof_iter_us = 1991 µs` = ≈ 1 period exact

Conclusion : `prof_cap_us` est WAIT structurel, pas CPU. Mode NONBLOCK +
poll attendrait sur fd ALSA même durée. Pas d'optim possible sans
changer sample rate ou period.

## Verdict global

✅ **Table de mixage console Debix complète et opérationnelle V10-ready** :

- 345 plugins LV2 chargeables (4 patterns I/O : 2/2, 1/1, 1/2, sidechain)
- Block-based + NEON intrinsics matrices
- Baseline 25.5% CPU mixer-pro
- Worker LV2 isolation cores 0+1 (RT clean)
- Port ranges JSON exposés pour normalisation NPU
- Presets persistés `/var/lib/mixer-pro/presets.json`
- Race conditions résolues (lock étendu mix_block, static bufs)

Reste pour V10 mastering NPU :
1. Architecture insert mastering post-master (out_0+out_1)
2. Type fx_chain pour cascade 5 plugins LV2
3. Modélisation TAC5212 Python (training PC)
4. NPU tap DMA + TFLite VX delegate
5. Training ML sur dataset paires (utilisateur)
6. Deploy + essais
