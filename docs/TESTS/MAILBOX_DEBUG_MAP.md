# Carte mailbox debug — V5.4.1 E6.b — REVUE EXHAUSTIVE

**SRAM_DEBUG_SIZE = 0x800 (2048 B)**, lecture côté Linux : `od -An -tx4 -N4 -j<offset> /sys/kernel/debug/sof/debug`

Cette carte recense **tous** les `mailbox_sw_reg_write` du code SOF, plus les rings dynamiques (`base+N`, `0xXXX+chan*4`).

---

## Écritures statiques (offset constant)

| Offset | Fichier | Fonction | Sens |
|---|---|---|---|
| 0x100 | matrix_2x8.c:229 | matrix_2x8_process | sentinelle DEADBEEF |
| 0x104 | matrix_2x8.c:230 | matrix_2x8_process | dbg_count tick (throttle 1/16) |
| 0x130-0x140 | matrix_2x8.c:248-253 | matrix_2x8_process | sink stream info (frame_bytes, free, channels, fmt) |
| 0x150-0x164 | matrix_2x8.c:201-206 | matrix_2x8_process | dbg_total + identity (throttle 1/32) |
| 0x1A0 | dai-legacy.c:1085 | dai_comp_trigger_internal STOP | dct_stop count |
| 0x200-0x208 | sai.c:561-563 | sai_trigger | dbg_trig + last cmd/dir |
| 0x230-0x23C | module_adapter_ipc3.c:164-196 | module_adapter_set_state | branch entries + PATH_STOP variants |
| 0x250-0x25C | zephyr_dma_domain.c:205,219,237,243 | dma_irq_handler | entries raw + PIPE 1/2 fires raw |
| 0x280-0x28C | pipeline-stream.c:115-183 | pipeline_comp_copy | total/P2/post-filter/err counters |
| 0x300-0x304 | sdma.c:118-120 | sdma_disable_channel | total + last chan |
| 0x320-0x32C | sdma.c:585-595 | sdma_stop | total + chan + status + skips |
| 0x340-0x34C | sai.c:223-235 | sai_stop | total + dir + TX/RX |
| 0x360-0x36C | pipeline-stream.c:632-635 | pipeline_trigger_run | total + cmd + ppl_id + host_id |
| 0x3C0-0x3C4 | ipc3/handler.c:520-521 | IPC pipeline_trigger (timer) | count + cmd |
| 0x3C8 | pipeline-xrun.c:80 | pipeline_xrun_recover | count |
| 0x3CC | pipeline-xrun.c:160 | pipeline_xrun (XRUN trig) | count |
| 0x3D0-0x3D4 | ipc3/handler.c:533-534 | IPC pipeline_trigger_run (DMA) | count + cmd |
| 0x400-0x40C | host-legacy.c:355-380 | host_dma_cb | total + PLAY/CAP/last bytes |
| 0x410-0x42C | host-legacy.c:228-256 | host_common_update | total + dir + bytes + local_pos + report_pos + period_bytes + host_size + no_pos |
| 0x570-0x574 | pipeline-schedule.c:182,186 | scheduler P1/P2 | dbg_p1/p2 |
| 0x580-0x58C | zephyr_dma_domain.c:204,239,245 | dma_irq_handler | entries (1/16) + PIPE 1/2 (1/16) |
| 0x5A0-0x5B4 | sdma.c:525-561 | sdma_start | dbg_entry + idx + status + HSTART count |
| 0x600-0x60C | sdma.c:634-640 | sdma_copy | total + last chan + bytes + chan6 count |
| 0x610-0x61C | dai-legacy.c:116-122 | dai_dma_cb | PLAY/CAP entries + last state/xrun |
| 0x620-0x62C | dai-legacy.c:1228-1291 | dai_zephyr (?) | dbg_play/cap/zp/zc |
| 0x630-0x654 | sdma.c:1100-1111 | sdma_set_config | sentinelle + cyclic + irq_disabled + count + chan_type + dir + src_w + dst_w + desc[0].config + buf_addr |
| 0x644-0x670 ⚠ | dai-legacy.c:142-170 | dai_dma_cb STOP branch (sticky) | dbg_stop_taken + state/xrun/dev_addr/dd_addr/etc. |
| 0x6A0 | sdma.c:648 | sdma_copy | chan3 type |
| 0x6A4-0x6A8 | dai-legacy.c:1310,1314 | idem | dbg_dp/dc |
| 0x6D0-0x6D4 | sdma.c:533-534 | sdma_start | sdma_chan_type + hw_event |
| 0x700-0x710 | sai.c:533-538 | sai_set_config | sentinelle + count + TCSR + RCSR + MCTL |
| 0x720-0x734 | sai.c:49-164 | sai_start | dbg_start_tx + tx_cfg + TCSR + dbg_start_rx + RCSR |
| 0x738-0x748 | pipeline-params.c:49-55 | pipeline_comp_params_neg | dbg_neg_count + cur_pid + start_pid + state + xpipe |
| 0x760-0x778 | dai-legacy.c:923-929 | dai_common_config_prepare | set_config trace |
| 0x7C0 | sdma.c:1126 | sdma_set_config | last (chan_idx<<8) | dir |
| 0x7C8-0x7CC | ipc3/dai.c:224,254 | ipc_dai_config | count + match total |
| 0x7E0-0x7E8 | sdma.c:442-464 | sdma_channel_get | count + idx + status |
| 0x7F0-0x7F4 | sdma.c:498-499 | sdma_channel_put | count + chan->index |

---

## Écritures dynamiques (rings et per-chan)

| Pattern | Fichier:Ligne | Plage couverte | Fonction |
|---|---|---|---|
| `0x210 + (n-1)*8` (n=1..4) | sai.c:583/584 | 0x210-0x22F | sai_trigger ring (cmd, dir) |
| `0x240 + (n-1)*16` (n=1..4) | module_adapter_ipc3.c:167-172 | 0x240-0x27F | set_state ring (id, ppl, cmd, state) |
| `0x260 + chan*4` (chan 0..7) | zephyr_dma_domain.c:222 | 0x260-0x27C | dma_irq_handler per-chan fires |
| `0x290 + (n-1)*8` (n=1..12) | pipeline-stream.c:126/127/179 | 0x290-0x2EF | pipeline_comp_copy P2 ring (info, err) |
| `0x310 + (n-1)*4` (n=1..4) | sdma.c:123 | 0x310-0x31C | sdma_disable_channel ring |
| `0x330 + (n-1)*4` (n=1..4) | sdma.c:602 | 0x330-0x33C | sdma_stop ring |
| `0x350 + (n-1)*4` (n=1..4) | sai.c:235 | 0x350-0x35C | sai_stop ring |
| `0x370 + (n-1)*16` (n=1..4) | pipeline-stream.c | 0x370-0x3AF | pipeline_trigger_run ring |
| `0x430 + (n-1)*8` (n=1..8) | host-legacy.c:395-397 | 0x430-0x46F | host_dma_cb ring (id, ppl) |
| `0x680 + chan*4` (chan 0..7) | sdma.c:684 | 0x680-0x69C | sdma_copy per-chan |
| `0x6B0 + chan*4` (chan 0..7) | sdma.c:546 | 0x6B0-0x6CC | sdma_start per-chan |
| `0x780 + chan*4` (chan 0..7) | sdma.c:1121 | 0x780-0x79C | sdma_set_config per-chan counter |
| `0x7A0 + chan*4` (chan 0..7) | sdma.c:1123 | 0x7A0-0x7BC | sdma_set_config per-chan direction |
| `0x1B0 + (n-1)*16` (n=1..4) | dai-legacy.c:1088-1094 | 0x1B0-0x1EF | dai_comp_trigger STOP ring (id, ppl, xrun, dir) |

---

## ⚠ COLLISIONS DÉTECTÉES (revue exhaustive)

### Collision A — `0x250-0x27F` : module_adapter ring ↔ dma_irq_handler

| Offset | Writer 1 | Writer 2 |
|---|---|---|
| 0x250-0x25C | dma_irq_handler raw counters (zephyr_dma_domain.c) | module_adapter_set_state ring entry n=2 (base=0x250) |
| 0x260-0x27C | dma_irq_handler per-chan fires (chan 0..7) | module_adapter_set_state ring entry n=3,4 (base=0x260, 0x270) |

**Statut** : module_adapter_set_state n'entre PAS dans la branche num_of_sources>1 (matrix_2x8 a son propre `.trigger` qui bypass). Donc le ring n'écrit jamais → **collision dormante**. À corriger pour fiabilité future.

### Collision B — `0x644-0x654` : sdma_set_config ↔ dai_dma_cb STOP branch

| Offset | Writer 1 (ACTIF) | Writer 2 (DORMANT) |
|---|---|---|
| 0x644 | sdma.c:1105 (config->direction) | dai-legacy.c:142 (dbg_stop_taken) |
| 0x648 | sdma.c:1106 (config->src_width) | dai-legacy.c:144 (dev->state au STOP) |
| 0x64C | sdma.c:1107 (config->dest_width) | dai-legacy.c:145 (dd->xrun au STOP) |
| 0x650 | sdma.c:1110 (desc[0].config) | dai-legacy.c:147 (dev addr au STOP) |
| 0x654 | sdma.c:1111 (buf_addr) | dai-legacy.c:148 (dd addr au STOP) |

**Statut** : dai_dma_cb STOP branch n'a jamais été prise au cold-boot (vérifié 0x644 = valeur sdma_set_config). **C'est cette collision qui m'a fait halluciner state=4 xrun=4 dev=0x000D0C00 dd=0x000F0C00** au début de l'investigation. Toutes ces "valeurs corrompues" venaient de sdma_set_config :
- `0x000D0C00` = probablement une adresse de `desc[0].config` ou `buf_addr`
- `4` à 0x648 = `config->src_width` (4 bytes/sample)
- `4` à 0x64C = `config->dest_width` (4 bytes/sample)

### Collision C — `0x658-0x670` : étendue dai_dma_cb STOP ↔ probables zones suivantes sdma

| Offset | Writer 1 | Writer 2 |
|---|---|---|
| 0x658-0x670 | dai-legacy.c:152-170 (next ptr, elem.size, chan idx, ipc_config) | (zone non utilisée par sdma.c selon grep, mais à proximité de 0x630-0x654 sdma) |

**Statut** : grep ne montre pas de writer sdma.c au-delà de 0x654. Mais on ne peut pas être certain sans vérification supplémentaire. L'écriture dai-legacy.c à 0x658-0x670 ne fire pas (STOP branch pas prise). Zone propre tant que la branche dort.

### Collision D — `0x740/0x744` (RÉSOLUE)

| Offset | Avant | Après edit récent |
|---|---|---|
| 0x740 | sai.c sai_stop (dbg_stop_tx) **+** pipeline-params.c:51 (start_pid) | sai.c retiré, seul pipeline-params.c écrit ✅ |
| 0x744 | sai.c sai_stop (dbg_stop_rx) **+** pipeline-params.c:52 (current->state) | sai.c retiré ✅ |

**Statut** : résolu. Le `sai_stop TX = 1` que je voyais initialement à 0x740 était en réalité `start_pid = 1` de pipeline-params. Les nouveaux compteurs sai_stop sont à 0x340-0x35F (clean).

---

## Zones libres confirmées (à utiliser pour nouvelle instrumentation)

| Plage | Taille | Statut |
|---|---|---|
| 0x108-0x12F | 40 B | libre |
| 0x140 ⚠ | utilisée (matrix sink fmt) | NON-libre |
| 0x144-0x14F | 12 B | libre |
| 0x168-0x19F | 56 B | libre |
| 0x1A4-0x1AF | 12 B | libre |
| 0x1F0-0x1FF | 16 B | libre |
| 0x22C-0x22F | 4 B | libre |
| 0x2F0-0x2FF | 16 B | libre |
| 0x308-0x30C | 8 B | libre |
| 0x314-0x31C | 12 B | libre (ring sdma_disable n=2..4) |
| 0x334-0x33C | 12 B | libre (ring sdma_stop n=2..4) |
| 0x354-0x35C | 12 B | libre (ring sai_stop n=2..4) |
| 0x3B0-0x3BF | 16 B | libre |
| 0x3D8-0x3FF | 40 B | libre |
| 0x470-0x4FF | **144 B largement libre** ✅ |
| 0x540-0x56F | 48 B | libre |
| 0x590-0x59C | 16 B | libre |
| 0x5B8-0x5FF | 72 B | libre |
| 0x6E0-0x6FF | 32 B | libre |
| 0x714-0x71F | 12 B | libre |
| 0x74C-0x75F | 20 B | libre |
| 0x7D0-0x7DF | 16 B | libre |

**Pour nouveau debug : utiliser 0x470-0x4FF (144 B propres garantis).**

---

## Méthode de vérification d'un offset avant utilisation

```bash
# Vérifier si un offset a un writer existant
grep -rn "mailbox_sw_reg_write.*0xXXX\b" /home/michael/yocto-nxp-debix/sof/src/

# Lister tous les offsets utilisés
grep -rn "mailbox_sw_reg_write" /home/michael/yocto-nxp-debix/sof/src/ | grep -oE "0x[0-9A-Fa-f]+" | sort -u
```

Pour les rings dynamiques (`base + N`, `0xXXX + chan*4`), expanser manuellement la plage couverte.

---

## Erreurs commises pendant cette session (auto-bilan)

1. **Collision B (0x644-0x670)** : interprété `state=4 xrun=4 dev=0x000D0C00 dd=0x000F0C00` comme une corruption de struct comp_dev. **C'était sdma_set_config qui écrivait aux mêmes offsets**.
2. **Collision D (0x740/0x744)** : interprété `sai_stop_tx=1` comme un appel à sai_stop. **C'était pipeline_comp_params_neg start_pid=1**. Diagnostic "STOP propagé via dai_comp_trigger" basé sur cette fausse mesure.
3. **Throttling 1/16 et 1/32** sur compteurs critiques : `0x150 matrix tick=1` et `0x58C PIPE 2 fires=1` sous-estimaient les vraies valeurs. Ajout des compteurs raw à 0x250-0x27C a corrigé.
4. **Hallucination** : "les 14 host_dma_cb viennent de HDMI/ES8316" — faux, ces cards ne passent pas par le DSP.

---

## État actuel des compteurs cold-boot pur (référence empirique)

| Compteur | Offset | Valeur | Fiabilité |
|---|---|---|---|
| matrix_2x8 ticks | 0x150 (1/32) | 1 (donc 1-32) | throttled |
| dma_irq_handler entries raw | 0x250 | 2 | ✅ fiable |
| status_get true | 0x254 | 2 | ✅ fiable |
| PIPE 2 fires raw | 0x25C | 2 | ✅ fiable |
| chan 4 fires (SAI TX) | 0x270 | 2 | ✅ fiable |
| pipeline_comp_copy P2 visites | 0x284 | 6 | ✅ fiable (zone clean) |
| pipeline_trigger_run total | 0x360 | 4 | ✅ fiable |
| ipc3 trigger DMA-driven count | 0x3D0 | 4 | ✅ fiable |
| pipeline_xrun (XRUN détecté DSP) | 0x3CC | jamais | ✅ fiable |
| host_dma_cb total | 0x400 | 14 | ✅ fiable |
| host_common_update | 0x410 | 14 | ✅ fiable |
| hd->report_pos accumulé | 0x420 | ~10752 | ✅ fiable |
| hd->host_period_bytes (seuil) | 0x424 | 16384 | ✅ fiable |
| sdma_disable_channel | 0x300 | 1 (chan 4) | ✅ fiable |
| sdma_stop | 0x320 | 1 (chan 4, status ACTIVE) | ✅ fiable |
| sai_stop total | 0x340 | 1 (PLAYBACK) | ✅ fiable |
| dai_comp_trigger_internal STOP | 0x1A0 | 1 (id=17 ppl=2 xrun=0) | ✅ fiable |
