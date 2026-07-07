# Phase 1a.3 — DSP V5.4 (8 strips IN + tee_1to2 + matrix 16×8 + 8 strips OUT + ASIO 8 in/out)

**Status** : V5.4 = V5.3 + corrections C20bis/C22bis/C24/C25 issues de l'investigation `de3e0184-ffcf-4315-9b19-3c48fa53d2e8` (6 workers, divergence sur C20). Architecture inchangée. À valider par investigation V5.4.
**Date** : 2026-04-28
**Branche** : `feature/audio-platform-v2`
**Précédent** : V5.3 (`PHASE_1A_3_DSP_V5.3.md`, commit 329fe804) — REVIEW à cause de C20 (fan-out via volume incorrect).

---

## 1. Synthèse corrections V5.3 → V5.4

| # | Correction | Origine investigation V5.3 |
|---|---|---|
| **C20bis** | **Comp `tee_1to2` custom OBLIGATOIRE** (pas fallback). C20 V5.3 affirmait que le `volume` supportait nativement le fan-out bsink_list 2-way ; lecture code (`volume.c:601` hardcode `output_buffers[0]`, `:687` fait `list_first_item(bsink_list)`, `mod->max_sinks=1` par défaut, check `module_adapter.c:806` rejette 2 sinks avec -EINVAL) prouve le contraire. Composant minimal STREAM mode, ~40 LOC. | claude-code, glm-5.1, deepseek (3/6 lecture profonde) |
| **C22bis** | **MIPS multiband_drc à mesurer impérativement E6** : large divergence inter-workers (3.5 à 18 MIPS/instance mono, pas de path HiFi4 SIMD dans `multiband_drc_generic.c`). Estimation prudent V5.4 = 100-150 MIPS pour 8× mb_drc. Total DSP : 180-280 MIPS. Mesure HW obligatoire à E6. | claude-code (5-10), glm-5.1 (15), deepseek (15-17.5), kimi (17-18), minimax (10-15), qwen (15-18) |
| **C24** | **DT carve réduit 128 MB → 8 MB** @ 0xA0000000. Utilisation réelle estimée 250-500 KB. 8 MB = marge 16-32× pour Phase 1a.3 et phases suivantes. Économie 120 MB RAM Linux (vs 128 MB initial). | claude-code (16 MB), glm-5.1 (8 MB), deepseek (4-8 MB), minimax (128 MB OK marge future) |
| **C25** | **Plan E0.6 paliers POC pipelines TIMER** : 5 → 10 → 27. Aucun précédent connu IPC3 SOF avec >5 pipelines TIMER co-scheduled (claude-code R-K). Risque scheduler non-mesuré. | claude-code, deepseek (Q4) |

**Pré-requis E0.5 V5.1 toujours non-appliqués** :
- `sof/src/platform/imx8m/imx8m.x.in:166` : cacheattr toujours `0x22212222`
- `sof/src/platform/imx8m/imx8m.x.in` : `sof_sdram2` MEMORY{} non déclaré
- `sof/src/platform/imx8m/lib/memory.c` : `buffer[3]` absent
- `sof/app/boards/imx8mp_evk_mimx8ml8_adsp.conf` : `CONFIG_COMP_IIR=y` absent

---

## 2. Architecture V5.4 figée (avec tee_1to2 explicite)

```
                                          ┌──► buf_post_in_cap[n]
SAI7 RX 8ch → ×8 strips IN → tee_1to2[n] ─┤    → interleave_8_cap → PCM capture 8ch
(post TAC ADC)               (NEW comp,    │    ("ASIO IN" — Linux)
                             1 src × 2 sinks)
                                           ▼
                                       buf_post_in_mix[n]
                                           │
                                           ▼
                              ┌────────────────────────────┐
                              │ mixer16 (matrix 16×8)      │ ◄── 8 PCM playback hosts
                              │ PRE effets de sortie       │     ("ASIO OUT" mono)
                              │ gains Q1.31 (128 cellules) │
                              └────────────┬───────────────┘
                                           ▼ 8 sinks mono homogènes
                              ×8 strips OUT (multiband_drc + pga + drc per channel)
                                           ▼
                              interleave_8 (8 mono → 8ch S32_LE interleaved)
                                           ▼
                              SAI7 TX 8ch → NPU tap V3.2.2 → 4× TAC5212
```

### 2.1 Composants NEW (3 au lieu de 2 vs V5.3)

| Composant | Rôle | LOC | Validation |
|---|---|---|---|
| **tee_1to2** | 1 src mono → 2 sinks mono identiques (memcpy) | ~40 | E0.6 POC obligatoire |
| **mixer16** | matrix 16×8 mono, gains Q1.31 (cf. V5.2 §2.1) | ~150 | E2 |
| **interleave_8** | 8 sinks mono → 1 buffer 8ch S32_LE (cf. V5.2 §2.2) | ~50 | E3 |

### 2.2 tee_1to2 spec

**Fichiers** :
- `sof/src/audio/tee_1to2/tee_1to2.c` (~40 LOC)
- `sof/src/audio/tee_1to2/Kconfig`
- `sof/src/audio/tee_1to2/CMakeLists.txt`
- patch `sof/src/audio/CMakeLists.txt`
- patch `sof/app/boards/imx8mp_evk_mimx8ml8_adsp.conf` (`+CONFIG_COMP_TEE_1TO2=y`)

**proc_type / API** : `MODULE_PROCESS_TYPE_AUDIO_STREAM` (= STREAM). Le check `module_adapter.c:258-261` (`max_sources>1 && max_sinks>1`) ne s'applique PAS à 1×2 (1>1 = false). Confirmé par 4/6 workers (claude-code, glm-5.1, deepseek, kimi).

**Skeleton** :
```c
static int tee_1to2_prepare(struct processing_module *mod, ...) {
    mod->max_sources = 1;
    mod->max_sinks = 2;
    return 0;
}

static int tee_1to2_process(struct processing_module *mod,
                             struct input_stream_buffer *in_buf, int n_in,
                             struct output_stream_buffer *out_buf, int n_out) {
    struct audio_stream *src = in_buf[0].data;
    int frames = in_buf[0].size;
    int nch = audio_stream_get_channels(src);

    for (int i = 0; i < n_out; i++) {
        struct audio_stream *dst = out_buf[i].data;
        audio_stream_copy(src, 0, dst, 0, frames * nch);
        out_buf[i].size = frames;
    }

    module_update_buffer_position(&in_buf[0], &out_buf[0], frames);
    return 0;
}

static const struct module_interface tee_1to2_interface = {
    .init = tee_1to2_init,
    .prepare = tee_1to2_prepare,
    .process_audio_stream = tee_1to2_process,
    .reset = tee_1to2_reset,
    .free = tee_1to2_free,
};

DECLARE_MODULE_ADAPTER(tee_1to2_interface, tee_1to2_uuid, tee_1to2_tr);
```

**MIPS estimation** : ~1 MIPS/instance × 8 = 8 MIPS (memory-bound, 96 frames/period × 4 B × 2 sinks = 768 B copie).

### 2.3 Périmètre DSP final

| Bloc | Quantité | Composant | NEW ? |
|---|---|---|---|
| strips IN | 8 mono parallèles | eq_iir + drc + 2× volume L/R | non |
| **tee_1to2** | **8 instances** | **tee_1to2 NEW (~40 LOC)** | **OUI** |
| capture host | 1 × 8ch | host PCM | non |
| matrix | 1 | mixer16 NEW | OUI (V5.2) |
| playback hosts | 8 mono | host PCM | non |
| strips OUT | 8 mono parallèles | multiband_drc + pga + drc | non |
| interleave | 1 | interleave_8 NEW | OUI (V5.2) |
| dai out | 1 | SAI7 TX 8ch | non |

**Total comps** : 8 strips IN × 4 + 8 tee + 1 mixer + 8 strips OUT × 3 + 1 interleave + DAI ≈ 75 instances.
**Total pipelines** : 27 inchangé (les tee_1to2 sont dans les pipelines de strips IN).

---

## 3. DT carve SDRAM2 réduit (C24)

```diff
-sdram2_reserved@a0000000 {
-    no-map;
-    reg = <0 0xA0000000 0 0x8000000>;  /* 128 MB */
-};
+sdram2_reserved@a0000000 {
+    no-map;
+    reg = <0 0xA0000000 0 0x800000>;   /* 8 MB — V5.4 reduction */
+};
```

```diff
-#define SDRAM2_SIZE 0x8000000   /* 128 MB */
+#define SDRAM2_SIZE 0x800000    /* 8 MB — V5.4 reduction */
```

Cacheattr `0x22112222` inchangé (region 5 = 0xA0000000-0xBFFFFFFF reste WT, mais seuls 8 MB sont carvés — le reste reste accessible Linux).

**Note** : Si E6 montre saturation buffer heap, agrandir à 16 ou 32 MB sans rebuild firmware (juste DT).

---

## 4. Pré-requis E0.5 + plan d'implémentation V5.4

| Étape | Action | Test gate |
|---|---|---|
| **E0.0** | Baseline board V4.2 + V3.2.2 NPU tap | test-sof-dsp.sh PASS, NPU tap PASS |
| **E0.5** | SDRAM2 8 MB (DT + cacheattr 0x22112222 + memory.h SDRAM2_BASE/SIZE + linker sof_sdram2 + heap[3] + COMP_IIR) | alloc test PASS dans buffer[3] (0xA0000000-0xA07FFFFF) |
| **E0.6** | **POC paliers (C25)** : <br>(a) tee_1to2 skeleton + topology test 1 voie : 1 host → tee → 2 hosts capture, vérifier sha256 identique <br>(b) cycle count multiband_drc mono S32_LE 3-bandes sur HW (target 5-18 MIPS, mesure réelle) <br>(c) 5 pipelines TIMER co-scheduled, mesurer cycles_max/period <br>(d) 10 pipelines TIMER, idem <br>(e) 27 pipelines TIMER (target full V5.4) | (a) bit-perfect 2 captures ; (b) MIPS confirmé ; (c-e) 0 timeout |
| **E0.7** | Patches isolés C14 + C21 + V4.2 régression | sha256sum capture 8ch identique avant/après |
| **E1** | Skeleton tee_1to2 + mixer16 + interleave_8 + Kconfig + CMakeLists | Build OK CONFIG_COMP_TEE_1TO2/MIXER16/INTERLEAVE_8=y |
| **E2** | mixer16 mix loop 16×8 gains identité + bytes blob 512B + topology test : 8 mics → tee → mixer16(I_8) → 8 strips OUT placeholder → interleave_8 → SAI TX | Audio passthrough, NPU tap PASS, V4.2 régression PASS |
| **E3** | PCM capture "ASIO IN" 8ch (depuis buf_post_in_cap[0..7] via interleave_8_cap) | arecord 8ch OK, fan-out bit-perfect entre matrix-input et capture-output |
| **E4** | 8 PCM playback "ASIO OUT" mono → mixer16 in[8..15] | aplay 8 mono streams OK, mix audible côté SAI TX |
| **E5** | Strips IN complets (eq_iir + drc + 2× volume L/R par voie) | 8 jeux EQ/DRC/Vol amixer fonctionnels |
| **E6** | Strips OUT complets (multiband_drc + pga + drc par voie). **Mesure cycle count multiband_drc ×8 réelle** (target ≤ 150 MIPS pour 8× mb_drc) | 8 jeux mb_drc/pga/drc OK, charge DSP < 50% |
| **E7** | NPU tap V3.2.2 régression complète + mesure latence end-to-end (SAI RX → SAI TX) | period_bytes=3072 stable, latence < 12 ms |
| **E8** | Stress 10 min (aplay 8ch + arecord 8ch + amixer Matrix + Strips parallèles) | 0 underrun, charge DSP < 60% |

---

## 5. Risques résiduels V5.4

| Risque | Probabilité | Mitigation |
|---|---|---|
| R-A tee_1to2 STREAM mode rejeté par module_adapter | **Faible** | E0.6.a POC + check `max_sources=1, max_sinks=2` non bloqué (4/6 workers confirment) |
| R-B PLATFORM_MAX_STREAMS 8→16 régression V4.2 | Faible | E0.7 test isolé bit-perfect |
| R-C multiband_drc MIPS > 150 sur HW | Moyen | Bypass dynamique ALSA (`process_enabled` switch, prepare-time toggle) ; réduction à 2 bandes au lieu de 3 |
| R-D 27 pipelines TIMER : aucun précédent IPC3 connu | **Moyen** | E0.6.c-e POC paliers 5/10/27 ; rollback si timeout |
| R-E DT carve 8 MB insuffisant si Phase 2-4 alloue plus | Faible | Élargir DT (rebuild kernel uniquement, pas firmware) |
| R-F Buffer multi-consumer rptr partagé (2 buffers indépendants post-tee) | Très faible | tee_1to2 produit dans 2 `comp_buffer` distincts → r_ptr séparés |
| R-G 8 SDMA channels playback côté hosts ASIO OUT | Faible | Vérifier DT iomux + `cat /sys/class/dma/` |
| R-H 0xA0000000 utilisé par CMA Linux | Faible | E0.5.a pré-check + reserved-memory no-map |
| R-I 265 controls ALSA saturent IPC3 | Faible | Bytes blob 512B fragmenté (SOF_IPC_MSG_MAX_SIZE=384) |
| R-J Latence end-to-end > 12 ms (6 hops × 2 ms pire cas) | Moyen | E7 mesure ; si > 12 ms, réduire période à 1 ms (4 ms latence min) |

---

## 6. Hors scope V5.4

- USB UAC2 gadget côté kernel — 100% Linux
- FX send/return — 100% Linux (JACK + LV2)
- Phase 4 NPU mastering closed-loop — séparé
- Optimisation HiFi4 SIMD multiband_drc — différée à Phase 1a.4 si E6 montre dépassement (pas de path SIMD aujourd'hui dans `multiband_drc_generic.c`)

---

## 7. Investigation critic V5.4 (questions ciblées)

Q1. **tee_1to2 STREAM mode (C20bis)** : skeleton ~40 LOC validé ? Le check `module_adapter.c:806` (`num_output_buffers > max_sinks`) accepte bien 2 buffers si `max_sinks=2` ? Pas d'autre garde-fou bloquant ?
Q2. **DT carve 8 MB (C24)** : suffit pour Phase 1a.3 + marge 16× ? Ajustement futur en DT-only OK (sans rebuild firmware) ?
Q3. **C25 paliers POC pipelines TIMER** : protocole de test viable ? Mesurer `cycles_max/period` à chaque palier permet d'extrapoler le risque de timeout à 27 ?
Q4. **MIPS multiband_drc mono divergence inter-workers** (3.5 à 18 MIPS) : peut-on stabiliser une estimation théorique avant E6 ? Ou la mesure HW est-elle le seul juge ?
Q5. **Bypass mb_drc prepare-time vs runtime** : claude-code dit runtime, glm-5.1 dit prepare-time. Lequel correct ? Lecture précise de `multiband_drc.c:374-399` + `multiband_drc_ipc3.c:35-37`.
Q6. **tee_1to2 vs option modif volume.c (~10 LOC)** : tee_1to2 plus propre architecturalement ? Ou modif volume.c plus économe (1 comp en moins par voie = -8 instances) ?
Q7. **Latence 6 hops vs single-tick** : si toutes les 27 pipelines sont co-scheduled sur le même tick TIMER 2 ms ET partagent le même `sched_comp`, latence réelle = 1 tick (2 ms) au lieu de 6×2=12 ms ? Comment garantir le single-tick scheduling ?
Q8. **Confirmation tous les workers V5.3 d'accord sur architecture restante** : 8 strips IN + tee + matrix + 8 strips OUT + interleave + SAI TX — aucun bug fondamental restant ?
Q9. **Risques RÉSIDUELS V5.4** non identifiés ?
Q10. **VERDICT FINAL : GO V5.4 prête pour E0.5+E0.7+E0.6 paliers ?** Ou itération V5.5 nécessaire ?

---

## 8. Référence

- Investigation V5.3 : job `de3e0184-ffcf-4315-9b19-3c48fa53d2e8` (6 workers, 1142s)
- V5.3 : `PHASE_1A_3_DSP_V5.3.md` (commit 329fe804)
- V5.2 : `PHASE_1A_3_DSP_V5.2.md` (commit 2543c087)
- V5.1 : `PHASE_1A_3_DSP_V5.1.md` (commit 8f2765f5)
- Architecture figée : mémoire `project_v5_architecture.md`
- V4.2 production : `meta-local/recipes-kernel/linux/files/sof-imx8mp-tac5212-drc.m4`
- NPU tap V3.2.2 : `PHASE_1A_2_NPU_TAP_V3.2.2.md`
