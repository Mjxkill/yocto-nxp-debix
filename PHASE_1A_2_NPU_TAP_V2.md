# Phase 1a.2 — NPU Tap V2 specification

**Status** : V1 rejeté avec 6 erreurs code-level par investigation 6 workers (job `01688a32-341a-4979-85fe-f5ab27d09d7f`). V2 = V1 + corrections.
**Date** : 2026-04-26
**Branche** : `feature/audio-platform-v2`
**Précédent** : V1 obsolète (`PHASE_1A_2_NPU_TAP_V1.md`).

---

## Objectif (inchangé V1)

Capturer les blocs audio post-effets OUT (post-MBDRC/PGA/DRC, juste avant le DMA SAI7 TX, **format 8ch S32_LE @ 48 kHz, 3072 B / période 2 ms = 1.5 MB/s**) depuis la pipeline V4.2 du firmware SOF, et les exposer à un programme userspace A53 qui les pousse au NPU pour analyse ML temps réel.

**Contrainte projet** : tap NPU non-négociable (cf. `CLAUDE.md`, mémoire `project_npu_non_negotiable.md`).

## Différences V2 vs V1 (les 6 corrections)

| # | Erreur V1 | Correction V2 |
|---|---|---|
| 1 | Adresse `0x93500000` HORS fenêtre DSP | Reserved-memory `npu_tap_buffer@0x93380000` (256 KB, dans SDRAM1 0x92C00000-0x933FFFFF) |
| 2 | `buffer_alloc(is_shared=true)` ne fait PAS ce que V1 croyait sur i.MX8MP single-core (CONFIG_CORE_COUNT=1) | **Pas de `buffer_alloc(is_shared)` du tout**. Reserved-memory DT à adresse fixe + `rmalloc(SOF_MEM_ZONE_RUNTIME, 0, SOF_MEM_CAPS_RAM \| SOF_MEM_CAPS_DMA, ...)` OU pointeur direct hardcoded vers la zone DT. |
| 3 | `dd->dma_buffer` est en OCRAM DSP-only (`0x3B6F0000`), invisible A53 | Memcpy explicite OBLIGATOIRE depuis `dd->dma_buffer` (OCRAM) → tap_buffer (SDRAM A53-visible). |
| 4 | Math fausse `period_bytes = 384 B` | Réel : **`period_bytes = 8 ch × 4 B × 96 frames @ 2 ms = 3072 B`**. Charge DSP <1% inchangé. |
| 5 | Ring buffer wrap non géré | `audio_stream_bytes_without_wrap()` + head/tail split en cas de wrap. |
| 6 | `dcache_writeback_region()` annoncé requis | Découverte : SDRAM cacheattr = WT (write-through, `cacheattr 0x22212222` digit 4 = 1). `dcache_writeback_region()` est no-op effectif. **Conservé pour portabilité** mais cohérence DSP→A53 gratuite. |

## Historique des voies tentées (à NE PAS reproduire)

Cf. `phase_1a2_history.md` (mémoire) et `PROJECT_STATE.md` §6 pour le détail complet :

| Voie | Statut |
|------|--------|
| V1.0 → V1.5 (ALSA capture PCM via BE DAI dummy / MUXDEMUX cross-pipeline) | NE FONCTIONNAIT PAS techniquement (DPCM walk + IPC3 trigger asymétrie + dummy BE binding). PAS un refus utilisateur. |
| S1 SOF Probes upstream | Bloqué silent : `sdma.c:588` guard drop AP2AP avec `buf_xaddr=0` (gateway HDA assumed) |
| Alt-A IPC vendor `SOF_IPC_PROBE_HOST_BUFFER_SET` | Bloqué silent -22 invisible (filtre ABI amont rejette cmd inconnu) |
| S6 mem_sink/tap_sink | Tenté précédemment, abandonné |
| SDMA scatter-gather multi-dest | Impossible HW |
| Dual-mapping OCRAM/DRAM coherent | Impossible HW (DSP Xtensa pas de MMU runtime) |
| SAI hardware loopback | Impossible HW (pas de mux interne TX→RX) |
| DMA Trace SOF détourné | Bande passante insuffisante |

## Solution V2 — Hook `dai_dma_cb()` + Reserved-memory DT corrigée

### Principe

Au moment où le firmware DSP a fini de copier le buffer audio post-DRC vers `dd->dma_buffer` (en OCRAM, destiné au DAI SAI7 TX), faire un **memcpy supplémentaire** vers une zone reserved-memory DT en SDRAM1 (DSP↔A53 visible). Le programme userspace A53 lit cette zone via mmap d'un module kernel out-of-tree miscdevice.

### Pourquoi V2 marche là où V1.x/S1/Alt-A ont échoué

- **Pas d'IPC custom** → pas de filtre ABI silencieux (≠ Alt-A)
- **Pas de SDMA AP2AP avec dest=0** → pas de guard `sdma.c:588` (≠ S1)
- **Pas de modèle gateway HDA assumed** → on contrôle physiquement où copier
- **Pas de patch ALSA core** → pas de DPCM walk à contourner (≠ V1.x ALSA)
- **Latence : ~3-5 µs / 2 ms = 0.15-0.25 %** (HiFi4 @ 800 MHz, memcpy 3072 B `~6000 cycles` worst case)
- **Cohérence cache gratuite** : SDRAM en write-through côté DSP, A53 en write-combine (mmap)
- **Adresse DSP-accessible** : 0x93380000 dans SDRAM1, dans la fenêtre 0x92400000-0x933FFFFF

### Architecture cible

```
Pipeline V4.2 PIPE 6 playback (inchangé) :

   PCM_host
      │
      ▼
   MBDRC ─► PGA ─► DRC ─► dma_buffer_copy_to() ─► dd->dma_buffer (OCRAM)
                                                       │           │
                                                       │           ▼
                                                       │      SAI7 TX SDMA ─► HP physiques
                                                       │
                                                       ▼ memcpy_s + dcache_writeback
                                                  npu_tap_buffer@0x93380000
                                                  (reserved-memory DT, 256 KB SDRAM1, no-map)
                                                       │
                                                       ▼ /dev/imx-audio-tap (miscdevice mmap WC)
                                                  Userspace A53 npu_tap_reader
                                                       │
                                                       ▼
                                                  NPU
```

### Modifications par fichier

#### Firmware SOF (`sof/`, modifs additives)

| Fichier | Modif | Lignes |
|---------|-------|--------|
| `sof/src/audio/dai-legacy.h` | +3 champs (`tap_buffer`, `tap_size`, `tap_write_offset`) dans `struct dai_data` | 3 |
| `sof/src/audio/dai-legacy.c` | Init `dd->tap_buffer = (void *)NPU_TAP_PHYS_ADDR` au `dai_common_params()` (playback only). Hook `memcpy_s` + ring wrap + `dcache_writeback_region` au `dai_dma_cb()` après `dma_buffer_copy_to()` existant. Init header magic au premier appel. Pas de free (zone DT, statique). | ~80 |
| `sof/src/include/sof/audio/npu_tap.h` (NOUVEAU) | Header ring-buffer struct `npu_tap_hdr` + macro `NPU_TAP_MAGIC = 0x5441504E ("NPAT")`, `NPU_TAP_RING_SIZE = 0x40000` (256 KB), `NPU_TAP_PHYS_ADDR = 0x93380000` | ~40 |
| `sof/app/boards/imx8mp_evk_mimx8ml8_adsp.conf` | Aucun changement V2 (V1 disait « retirer CONFIG_PROBE » mais l'option n'est pas dans ce fichier). Modifs Alt-A locales `sof/src/probe/probe.c`, `ipc/ipc3/handler.c`, `include/ipc/header.h`, `include/ipc3/probe.h`, `include/sof/probe/probe.h` à **REVERT** avant J1 V2. | -- |

##### Header struct (npu_tap.h)

```c
#ifndef __SOF_AUDIO_NPU_TAP_H__
#define __SOF_AUDIO_NPU_TAP_H__

#include <stdint.h>

#define NPU_TAP_MAGIC      0x5441504Eu   /* "NPAT" little-endian */
#define NPU_TAP_RING_SIZE  0x40000u      /* 256 KB = ~170 ms @ 1.5 MB/s */
#define NPU_TAP_PHYS_ADDR  0x93380000u   /* Reserved-memory DT, SDRAM1, DSP-visible */
#define NPU_TAP_HDR_SIZE   64u           /* aligned, leaves 256 KB - 64 for data */

struct npu_tap_hdr {
    uint32_t magic;          /* NPU_TAP_MAGIC */
    uint32_t version;        /* 1 */
    uint32_t ring_size;      /* NPU_TAP_RING_SIZE - NPU_TAP_HDR_SIZE */
    uint32_t hdr_size;       /* NPU_TAP_HDR_SIZE */
    uint32_t write_idx;      /* DSP increments (bytes) */
    uint32_t read_idx;       /* A53 increments (bytes) */
    uint32_t period_bytes;   /* 3072 = 8ch × 4B × 96 frames */
    uint32_t sample_rate;    /* 48000 */
    uint32_t channels;       /* 8 */
    uint32_t frame_fmt;      /* SOF_IPC_FRAME_S32_LE */
    uint32_t reserved[6];
} __attribute__((packed));

#endif
```

##### Hook V2 dans dai_dma_cb()

```c
/* Dans dai_dma_cb(), après le if (dev->direction == SOF_IPC_STREAM_PLAYBACK) { ... } existant */
if (dev->direction == SOF_IPC_STREAM_PLAYBACK && ret >= 0 && dd->tap_buffer) {
    struct npu_tap_hdr *hdr = (struct npu_tap_hdr *)dd->tap_buffer;
    uint8_t *data = (uint8_t *)dd->tap_buffer + NPU_TAP_HDR_SIZE;
    uint32_t ring_size = NPU_TAP_RING_SIZE - NPU_TAP_HDR_SIZE;
    uint32_t w = hdr->write_idx;

    /* Source = dd->dma_buffer (post format conversion S32_LE 8ch) */
    void *src = audio_stream_get_rptr(&dd->dma_buffer->stream);
    uint32_t to_end = audio_stream_bytes_without_wrap(&dd->dma_buffer->stream, src);
    uint32_t head = MIN(bytes, to_end);
    uint32_t tail = bytes - head;

    /* Tap buffer wrap handling (head + optional tail) */
    uint32_t tap_to_end = ring_size - w;
    if (head <= tap_to_end) {
        memcpy_s(data + w, ring_size - w, src, head);
    } else {
        memcpy_s(data + w, ring_size - w, src, tap_to_end);
        memcpy_s(data, ring_size, (uint8_t *)src + tap_to_end, head - tap_to_end);
    }

    if (tail) {
        /* Source wrapped, copy tail from dma_buffer base */
        void *src2 = audio_stream_get_addr(&dd->dma_buffer->stream);
        uint32_t new_w = (w + head) % ring_size;
        uint32_t tap_to_end2 = ring_size - new_w;
        if (tail <= tap_to_end2) {
            memcpy_s(data + new_w, ring_size - new_w, src2, tail);
        } else {
            memcpy_s(data + new_w, ring_size - new_w, src2, tap_to_end2);
            memcpy_s(data, ring_size, (uint8_t *)src2 + tap_to_end2, tail - tap_to_end2);
        }
    }

    /* DSP cacheattr SDRAM = WT, dcache_writeback est no-op effectif mais conservé */
    dcache_writeback_region(data + w, bytes);
    dcache_writeback_region(hdr, sizeof(*hdr));

    /* Atomic update of write_idx */
    hdr->write_idx = (w + bytes) % ring_size;
    dcache_writeback_region(&hdr->write_idx, sizeof(hdr->write_idx));
}
```

##### Init au dai_common_params() (playback only)

```c
if (dev->direction == SOF_IPC_STREAM_PLAYBACK && !dd->tap_buffer) {
    dd->tap_buffer = (void *)(uintptr_t)NPU_TAP_PHYS_ADDR;

    struct npu_tap_hdr *hdr = (struct npu_tap_hdr *)dd->tap_buffer;
    hdr->magic = NPU_TAP_MAGIC;
    hdr->version = 1;
    hdr->ring_size = NPU_TAP_RING_SIZE - NPU_TAP_HDR_SIZE;
    hdr->hdr_size = NPU_TAP_HDR_SIZE;
    hdr->write_idx = 0;
    hdr->read_idx = 0;
    hdr->period_bytes = bytes_per_period;     /* = 3072 attendu */
    hdr->sample_rate = 48000;
    hdr->channels = 8;
    hdr->frame_fmt = SOF_IPC_FRAME_S32_LE;
    dcache_writeback_region(hdr, sizeof(*hdr));
}
```

#### Device Tree (DTS, additif)

| Fichier | Modif |
|---------|-------|
| Patch DT additif (pattern existant `vdev0buffer` Cortex-M7) : nouveau node `reserved-memory/npu_tap_buffer@0x93380000` (256 KB, no-map, dans SDRAM1) + node `imx_audio_tap` avec `compatible = "electrosens,imx-audio-tap"` et `memory-region = <&npu_tap_buffer>` |

```dts
/ {
    reserved-memory {
        npu_tap_buffer: npu_tap_buffer@93380000 {
            compatible = "shared-dma-pool";
            reg = <0x0 0x93380000 0x0 0x40000>;   /* 256 KB */
            no-map;
        };
    };

    imx_audio_tap {
        compatible = "electrosens,imx-audio-tap";
        memory-region = <&npu_tap_buffer>;
    };
};
```

⚠ **Vérification obligatoire** : confirmer que `0x93380000` n'overlap pas avec `dsp_reserved_heap@93400000` (qui commence à 0x93400000). 0x93380000 + 0x40000 = 0x933C0000 < 0x93400000 ✅. Confirmer aussi que SOF firmware n'utilise pas ces 256 KB pour `HEAP_BUFFER_SIZE` runtime — à vérifier dans `memory.h:154-165` et runtime via le mapfile post-build.

#### Kernel module out-of-tree (`meta-local/recipes-kernel/imx-audio-tap/`)

| Fichier | Modif |
|---------|-------|
| `imx-audio-tap.c` (NOUVEAU, ~100 LOC) | `miscdevice` `/dev/imx-audio-tap`. Probe : `of_reserved_mem_lookup()` pour récupérer phys+size. mmap : `remap_pfn_range()` + `pgprot_writecombine`. sysfs : `magic`, `period_bytes`, `sample_rate`, `channels`, `phys_addr`, `size`. |
| `imx-audio-tap_0.1.bb` (NOUVEAU) | Recipe Yocto kernel module out-of-tree |
| `linux-imx_%.bbappend` | Aucun changement (module séparé, chargé par `systemd-modules-load.d`) |

#### App userspace (`meta-local/audio-tools/`)

| Fichier | Modif |
|---------|-------|
| `npu_tap_reader.c` (NOUVEAU, ~120 LOC) | `open /dev/imx-audio-tap` → `mmap` → valide magic → poll `hdr->write_idx` (busy-wait ou epoll si module ajoute irq) → lit blocs → push NPU (stub printf initial). Ring read avec head/tail split. |

### Chemins de risque + mitigation

| Risque | Mitigation V2 |
|--------|---------------|
| 0x93380000 hors fenêtre DSP MMU/MPU | Vérifié : 0x93380000 < 0x933FFFFF ✅ (fin SDRAM1). Cacheattr digit 4 = 1 (write-through cacheable). Test runtime J1. |
| 0x93380000 utilisé déjà par SOF heap_buffer runtime | Reduce `HEAP_BUFFER_SIZE` de 256 KB dans `memory.c` si nécessaire, OU placer le tap au tout début de SDRAM1 et décaler `HEAP_SYSTEM_BASE`. À vérifier au mapfile post-build. |
| Cache coherency (bug rare) | DSP : `dcache_writeback_region` après chaque memcpy + write_idx (no-op WT mais portable). A53 : `pgprot_writecombine` mmap. |
| Drop sous-charge si A53 trop lent | Ring 256 KB - 64 = 262080 B = ~85 périodes = ~170 ms tampon. NPU latency target ~50 ms → marge confortable. |
| Régression V4.2 audio (memcpy ~3-5 µs / 2 ms = 0.25 %) | Tests T1-T6 V4.2 obligatoires après chaque deploy (Gate explicite). |
| Ring wrap dma_buffer ET tap_buffer simultané | Code V2 gère les 2 wraps explicitement (head/tail × 2). |
| `audio_stream_bytes_without_wrap` indispo | Si oui, fallback : copier en deux étapes manuelles via `audio_stream_get_addr` + `audio_stream_get_size`. |

### Plan en 5 jours

| Jour | Travail | Gate |
|------|---------|------|
| **J0** | Revert modifs Alt-A locales `sof/`. Régression V4.2 PASS sur firmware vanilla. | `git diff sof/` propre + 4/4 V4.2 PASS |
| **J1** | Firmware : `npu_tap.h` + hook `dai_dma_cb()` + init `dai_common_params()` + build + sign + deploy | Build OK + V4.2 régression PASS + magic visible via `devmem 0x93380000` côté A53 |
| **J2** | Kernel module `imx-audio-tap.ko` + DTS reserved-memory + recipe Yocto | `modprobe` OK + `/dev/imx-audio-tap` présent + sysfs `magic == 0x5441504E` |
| **J3** | App userspace `npu_tap_reader` + dump wav | Wav non-vide + RMS > -100 dB pendant `aplay siren.wav` |
| **J4** | Test latence + 0-packet-loss 10 min | <50 ms latence + 0 underrun ring + 4/4 V4.2 PASS |
| **J5** | Intégration NPU pipeline réelle | Stream NPU stable, mesure performance ML |

### Ce que V2 NE FAIT PAS (sécurité)

- Aucune modif `sof/src/drivers/imx/sai.c` (intouchable, 7 jours validation utilisateur)
- Aucune modif kernel ALSA core
- Aucune modif topology m4 V4.2
- Aucune modif IPC SOF (pas de vendor cmd risquant filtre amont silencieux)
- Aucune dépendance à un mécanisme upstream non-supporté sur SDMA (pas de probes gateway model)
- **Aucun usage de `is_shared=true`** (no-op sur i.MX8MP single-core, source d'erreur V1)

### Plan B (si V2 échoue inopinément)

**Cortex-M7 bridge via RPMsg** : pattern déjà éprouvé sur ce projet. Le M7 (inutilisé) snoope la zone shared mem et transmet via RPMsg vers A53. Découple totalement le tap du chemin audio critique. Effort 2-3 semaines mais robuste.

## Validation

Avant J0, lancer une **investigation V2 finale** sur ce plan détaillé pour validation collective. Si GO unanime, passer à l'implémentation J0.

## Référence

- Investigation V1 → V2 corrections : job `01688a32-341a-4979-85fe-f5ab27d09d7f` (verdict GO conditionnel + 6 erreurs identifiées)
- Décision archivée : critic_decision `3565abbf-19e4-4091-9bf6-19cfaf4c7708`
- Mémoire projet : `phase_1a2_history.md`, `npu_tap_v1_proposal.md` (à mettre à jour V2)
- Doc maître projet : `PROJECT_STATE.md`
- Pattern reserved-memory existant : `vdev0buffer@94300000` (M7 RPMsg, hors fenêtre DSP) — V2 utilise même pattern dans la fenêtre DSP
