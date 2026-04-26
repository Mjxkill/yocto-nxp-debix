# Phase 1a.2 — NPU Tap V3 specification

**Status** : V2 rejeté en NO-GO par investigation 6 workers (job `64a4b28e-f6ab-4b02-9cfa-552b0c145b04`). V3 = V2 + corrections issues investigation.
**Date** : 2026-04-26
**Branche** : `feature/audio-platform-v2`
**Précédents** : V1 obsolète (`PHASE_1A_2_NPU_TAP_V1.md`), V2 obsolète (`PHASE_1A_2_NPU_TAP_V2.md`).

---

## Objectif (inchangé V1/V2)

Capturer les blocs audio post-effets OUT (post-MBDRC/PGA/DRC, juste avant le DMA SAI7 TX, **format 8 ch S32_LE @ 48 kHz, 3072 B / période 2 ms = 1.5 MB/s**) depuis la pipeline V4.2 du firmware SOF, et les exposer à un programme userspace A53 qui les pousse au NPU pour analyse ML temps réel.

**Contrainte projet** : tap NPU non-négociable (cf. `CLAUDE.md`, mémoire `project_npu_non_negotiable.md`).

## Évolution V2 → V3 (les 6 corrections post-investigation 64a4b28e)

| # | Erreur V2 (constat investigation 64a4b28e) | Correction V3 |
|---|---|---|
| **1** 🔴 | Adresse `0x93380000` **DANS HEAP_BUFFER** SOF (0x92C2F100-0x933FF000). `buffer_alloc()` peut écrire à 0x93380000 → corruption silencieuse. Cause racine : confusion entre `dsp_reserved` côté Linux et `MEMORY{}` côté SOF firmware. | Déplacer à **`0x942B0000`** : carver 256 KB dans `dsp_reserved_heap` (Linux no-map), JUSTE AVANT `vdev0vring0@942F0000`. **HORS de SOF `MEMORY{}`** (au-delà de 0x93400000). DSP voit toujours via cacheattr digit 4 (region 0x80000000-0x9FFFFFFF). |
| **2** 🟠 | Hook utilisait `dd->period_bytes` (constant) | Utiliser `bytes` = `next->elem.size` (variable transitoire post-xrun, ≤ period_bytes) |
| **3** 🟠 | Pas de barrière mémoire entre data write et `write_idx` update | Ajouter `__asm__ volatile("memw" ::: "memory")` (Xtensa) entre les deux |
| **4** 🟠 | `dai_common_reset()` ne nullifie pas `dd->tap_buffer` → race possible avec callback en vol | Ajouter `dd->tap_buffer = NULL` dans `dai_common_reset()` |
| **5** 🟠 | Côté A53 kernel, lecture `write_idx` sans barrière | `READ_ONCE(hdr->write_idx)` + `smp_rmb()` avant lecture data |
| **6** 🟡 | Pendant XRUN, hook copie du silence dans tap_buffer (pas grave mais inutile) | Skip tap_buffer write si `dd->xrun` (early-return existant l.116-123) |

## Historique des voies tentées (à NE PAS reproduire)

Cf. `phase_1a2_history.md` (mémoire) et `PROJECT_STATE.md` §6 pour le détail. V1 ALSA / S1 Probes / Alt-A IPC vendor / S6 mem_sink toutes bloquées techniquement.

## Solution V3 — Hook `dai_dma_cb()` + Reserved-memory dans `dsp_reserved_heap`

### Découverte clé V3

Distinction CRITIQUE entre 2 « fenêtres DSP » :

1. **`dsp_reserved` côté Linux** = `[0x92400000, 0x933FFFFF]` (16 MB, no-map). Linux ne touche pas. C'est là que SOF firmware tourne.
2. **`MEMORY{}` côté SOF firmware** = SDRAM0 + SDRAM1 = `[0x92400000, 0x93400000)` (16 MB exactement). SOF y alloue activement (mailbox, heaps, stack).
3. **`dsp_reserved_heap` côté Linux** = `[0x93400000, 0x942F0000)` (~15 MB, no-map). **HORS de SOF `MEMORY{}`**, mais DANS la cacheattr Xtensa region 4 (0x80000000-0x9FFFFFFF, write-through). DSP peut y accéder via pointeur direct, SOF firmware ne l'utilise pas.

→ La **vraie zone idéale** pour le tap est dans `dsp_reserved_heap`, pas dans `dsp_reserved`. C'est ce que V3 fait.

### Architecture cible

```
Pipeline V4.2 PIPE 6 playback (inchangé) :

   PCM_host
      │
      ▼
   MBDRC ─► PGA ─► DRC ─► dma_buffer_copy_to() ─► dd->dma_buffer (OCRAM 0x3B6F0000)
                                                       │           │
                                                       │           ▼
                                                       │      SAI7 TX SDMA ─► HP physiques
                                                       │
                                                       ▼ memcpy + memw barrier + writeback
                                                  npu_tap_buffer@0x942B0000
                                                  (carve dans dsp_reserved_heap, 256 KB no-map)
                                                       │
                                                       ▼ /dev/imx-audio-tap (miscdevice mmap WC)
                                                  Userspace A53 npu_tap_reader
                                                       │ (READ_ONCE + smp_rmb)
                                                       ▼
                                                  NPU
```

### Vérifications préalables sur 0x942B0000

| Critère | Vérification |
|---|---|
| Cacheattr DSP cacheable | `_addr_range(0x942B0000) = (0x942B0000 >> 29) & 0x7 = 4`. `_addr_attr = (0x22212222 >> 16) & 0xF = 1` = **write-through**. ✅ |
| Hors SOF `MEMORY{}` | `0x942B0000 > 0x93400000` (fin SDRAM1). SOF firmware n'y alloue pas via `buffer_alloc`. ✅ |
| Dans Linux `no-map` | Carve 256 KB de `dsp_reserved_heap` qui est `no-map` Linux. ✅ |
| Pas de conflit M7 RPMsg | `vdev0vring0@942F0000` commence à 0x942F0000. `0x942B0000 + 0x40000 = 0x942F0000` = exactement avant. ✅ |
| Aligné | `0x942B0000` = aligné sur 64 KB (0x10000) — adéquat pour `pgprot_writecombine`. ✅ |

### Modifications par fichier

#### 1. Firmware SOF (`sof/`, modifs additives, ~100 lignes total)

##### `sof/src/audio/dai-legacy.h` (+3 champs dans `struct dai_data`)

```c
/* V3 NPU tap (i.MX8MP) */
void *tap_buffer;            /* Direct ptr to reserved-memory @0x942B0000, NULL = disabled */
uint32_t tap_buffer_size;    /* NPU_TAP_RING_SIZE */
uint32_t tap_period_bytes;   /* For sanity check, normally = period_bytes */
```

##### `sof/src/include/sof/audio/npu_tap.h` (NOUVEAU, ~50 lignes)

```c
#ifndef __SOF_AUDIO_NPU_TAP_H__
#define __SOF_AUDIO_NPU_TAP_H__

#include <stdint.h>

#define NPU_TAP_MAGIC      0x5441504Eu   /* "NPAT" little-endian */
#define NPU_TAP_RING_SIZE  0x40000u      /* 256 KB total (header + data) */
#define NPU_TAP_HDR_SIZE   64u           /* aligned, leaves 256 KB - 64 for ring data */
#define NPU_TAP_DATA_SIZE  (NPU_TAP_RING_SIZE - NPU_TAP_HDR_SIZE)
#define NPU_TAP_PHYS_ADDR  0x942B0000u   /* Carve dans dsp_reserved_heap, hors SOF MEMORY{} */

struct npu_tap_hdr {
    uint32_t magic;          /* NPU_TAP_MAGIC */
    uint32_t version;        /* 1 */
    uint32_t ring_size;      /* NPU_TAP_DATA_SIZE */
    uint32_t hdr_size;       /* NPU_TAP_HDR_SIZE */
    uint32_t write_idx;      /* DSP increments (bytes), wrap-around mod ring_size */
    uint32_t read_idx;       /* A53 increments (bytes), wrap-around mod ring_size */
    uint32_t period_bytes;   /* 3072 nominal */
    uint32_t sample_rate;    /* 48000 */
    uint32_t channels;       /* 8 */
    uint32_t frame_fmt;      /* SOF_IPC_FRAME_S32_LE */
    uint32_t reserved[6];
} __attribute__((packed));

#endif
```

##### `sof/src/audio/dai-legacy.c` — modifs

**Init au `dai_common_params()`** (après l. 590, avant `return` l. 593) :

```c
if (dev->direction == SOF_IPC_STREAM_PLAYBACK) {
    dd->tap_buffer = (void *)(uintptr_t)NPU_TAP_PHYS_ADDR;
    dd->tap_buffer_size = NPU_TAP_RING_SIZE;
    dd->tap_period_bytes = period_bytes;

    struct npu_tap_hdr *hdr = (struct npu_tap_hdr *)dd->tap_buffer;
    hdr->magic = NPU_TAP_MAGIC;
    hdr->version = 1;
    hdr->ring_size = NPU_TAP_DATA_SIZE;
    hdr->hdr_size = NPU_TAP_HDR_SIZE;
    hdr->write_idx = 0;
    hdr->read_idx = 0;
    hdr->period_bytes = period_bytes;
    hdr->sample_rate = 48000;
    hdr->channels = 8;
    hdr->frame_fmt = SOF_IPC_FRAME_S32_LE;
    /* memw for ordering, even on WT — defensive */
    __asm__ volatile ("memw" ::: "memory");
    dcache_writeback_region(hdr, sizeof(*hdr));
}
```

**Hook au `dai_dma_cb()`** (après `dma_buffer_copy_to` l. 127, dans la branche playback) :

```c
/* V3 NPU tap : copy post-DRC samples to shared ring buffer */
if (dev->direction == SOF_IPC_STREAM_PLAYBACK
    && ret >= 0
    && dd->tap_buffer
    && !dd->xrun                                 /* skip during xrun */
    && bytes > 0) {

    struct npu_tap_hdr *hdr = (struct npu_tap_hdr *)dd->tap_buffer;
    uint8_t *data_base = (uint8_t *)dd->tap_buffer + NPU_TAP_HDR_SIZE;
    uint32_t ring_size = NPU_TAP_DATA_SIZE;
    uint32_t w = hdr->write_idx;

    /* Source = dd->dma_buffer (post format conversion) — ring buffer */
    void *src = audio_stream_get_rptr(&dd->dma_buffer->stream);
    uint32_t src_to_end = audio_stream_bytes_without_wrap(&dd->dma_buffer->stream, src);
    uint32_t src_head = MIN(bytes, src_to_end);
    uint32_t src_tail = bytes - src_head;

    /* Tap ring write at offset w, with wrap handling */
    uint32_t tap_to_end = ring_size - w;
    uint32_t head_in_tap = MIN(src_head, tap_to_end);
    memcpy_s(data_base + w, ring_size - w, src, head_in_tap);
    if (src_head > tap_to_end) {
        memcpy_s(data_base, ring_size,
                 (uint8_t *)src + tap_to_end, src_head - tap_to_end);
    }

    if (src_tail) {
        void *src2 = audio_stream_get_addr(&dd->dma_buffer->stream);
        uint32_t new_w = (w + src_head) % ring_size;
        uint32_t tap_to_end2 = ring_size - new_w;
        uint32_t tail_in_tap = MIN(src_tail, tap_to_end2);
        memcpy_s(data_base + new_w, ring_size - new_w, src2, tail_in_tap);
        if (src_tail > tap_to_end2) {
            memcpy_s(data_base, ring_size,
                     (uint8_t *)src2 + tap_to_end2, src_tail - tap_to_end2);
        }
    }

    /* WT cacheattr drain (mostly redundant but defensive) */
    dcache_writeback_region(data_base + w, MIN(bytes, ring_size - w));
    if (bytes > ring_size - w)
        dcache_writeback_region(data_base, bytes - (ring_size - w));

    /* Memory barrier: ensure data writes are visible BEFORE write_idx update.
     * Critical because A53 mmap is write-combine (relaxed ordering). */
    __asm__ volatile ("memw" ::: "memory");

    /* Atomic update of write_idx (32-bit aligned, atomic on Xtensa+ARMv8) */
    hdr->write_idx = (w + bytes) % ring_size;
    dcache_writeback_region(&hdr->write_idx, sizeof(hdr->write_idx));
}
```

**Cleanup au `dai_common_reset()`** (avant l. 726) :

```c
dd->tap_buffer = NULL;
dd->tap_buffer_size = 0;
dd->tap_period_bytes = 0;
```

##### Modifs Alt-A à revert avant J1 V3

```bash
git checkout sof/src/probe/probe.c
git checkout sof/src/ipc/ipc3/handler.c
git checkout sof/src/include/ipc/header.h
git checkout sof/src/include/ipc3/probe.h
git checkout sof/src/include/sof/probe/probe.h
git checkout sof/app/boards/imx8mp_evk_mimx8ml8_adsp.conf
```

#### 2. Device Tree (DTS, additif)

Patch DT additif (intégré au `linux-imx_%.bbappend` via `apply-tac5212-dt.py` ou patch dédié) :

```dts
&{/reserved-memory/dsp_reserved_heap@93400000} {
    /* Reduce dsp_reserved_heap by 256 KB to carve npu_tap_buffer */
    reg = <0x0 0x93400000 0x0 0xeb0000>;   /* 0xef0000 - 0x40000 */
};

&{/reserved-memory} {
    npu_tap_buffer: npu_tap_buffer@942b0000 {
        compatible = "shared-dma-pool";
        reg = <0x0 0x942b0000 0x0 0x40000>;   /* 256 KB */
        no-map;
    };
};

/ {
    imx_audio_tap: imx_audio_tap {
        compatible = "electrosens,imx-audio-tap";
        memory-region = <&npu_tap_buffer>;
    };
};
```

⚠ **Vérification obligatoire** : confirmer `0x942B0000 + 0x40000 = 0x942F0000` = pile début `vdev0vring0@942F0000`. Pas d'overlap.

#### 3. Kernel module out-of-tree (`meta-local/recipes-kernel/imx-audio-tap/`)

| Fichier | Modif |
|---|---|
| `imx-audio-tap.c` (NOUVEAU, ~120 LOC) | `miscdevice` `/dev/imx-audio-tap`. Probe : `of_reserved_mem_lookup()` → phys + size. mmap : `remap_pfn_range()` + `pgprot_writecombine`. sysfs : `magic`, `period_bytes`, `sample_rate`, `channels`, `phys_addr`, `size`. Le reader doit utiliser `READ_ONCE(hdr->write_idx)` + `smp_rmb()` avant data read. |
| `imx-audio-tap_0.1.bb` (NOUVEAU) | Recipe Yocto kernel module out-of-tree |
| Chargé par `systemd-modules-load.d` au boot | Aucun changement `linux-imx_%.bbappend` |

#### 4. App userspace (`meta-local/audio-tools/`)

| Fichier | Modif |
|---|---|
| `npu_tap_reader.c` (NOUVEAU, ~120 LOC) | `open` → `mmap` PROT_READ → valide `magic`. Boucle : `READ_ONCE(hdr->write_idx)` + `smp_rmb()` (via `__atomic_load_n` ou `READ_ONCE` macro userspace) → lecture des blocs (avec ring wrap) → push NPU (stub printf au début). Poll busy-wait `usleep(2000)` à 500 Hz acceptable (~0.02% CPU A53). |

### Risques + mitigation V3

| Risque | Mitigation V3 |
|---|---|
| 0x942B0000 hors fenêtre DSP MMU/MPU | Vérifié : cacheattr region 4 (0x80000000-0x9FFFFFFF) couvre. WT cacheable. Test runtime J1. |
| Conflit avec M7 RPMsg vdev0vring0 | 0x942B0000 + 0x40000 = 0x942F0000 = pile avant. Pas d'overlap. |
| Linux Linux-side mapping conflict | DT carve : `dsp_reserved_heap` réduit de 256 KB, `npu_tap_buffer` est un nœud séparé. `of_reserved_mem_lookup()` sait résoudre. |
| Cache coherency DSP→A53 | DSP : `memw` barrier + `dcache_writeback_region` (WT, mostly no-op effectif). A53 : `pgprot_writecombine` mmap + `READ_ONCE(write_idx)` + `smp_rmb()`. Cohérence garantie. |
| Race `dai_common_reset()` vs callback en vol | `dd->tap_buffer = NULL` dans reset + check `if (!dd->tap_buffer) return;` dans hook. |
| XRUN | Skip explicite `if (dd->xrun) goto skip_tap;` (mais l'early return l.116-123 le fait déjà). |
| Drop sous-charge | Ring 256 KB - 64 = ~85 périodes = ~170 ms. NPU latency target ~50 ms → marge confortable. |
| Régression V4.2 audio | Tests T1-T6 V4.2 obligatoires après chaque deploy (Gate explicite). |
| `audio_stream_bytes_without_wrap` indispo | Vérifié existe `audio_stream.h:749`, signature `(const struct audio_stream *, const void *) -> int`. |

### Plan en 5 jours (V3)

| Jour | Travail | Gate |
|---|---|---|
| **J0** | Revert modifs Alt-A locales `sof/`. Régression V4.2 PASS sur firmware vanilla. | `git diff sof/` propre + 4/4 V4.2 PASS |
| **J1** | Firmware : `npu_tap.h` + hook `dai_dma_cb()` + init `dai_common_params()` + cleanup `dai_common_reset()` + build + sign + deploy | Build OK + V4.2 régression PASS + `magic` lisible via `devmem 0x942B0000` |
| **J2** | Kernel module `imx-audio-tap.ko` + DTS patch (reduce dsp_reserved_heap + add npu_tap_buffer) + recipe Yocto | `modprobe` OK + `/dev/imx-audio-tap` présent + sysfs `magic == 0x5441504E` |
| **J3** | App userspace `npu_tap_reader` + dump wav | Wav non-vide + RMS > -100 dB pendant `aplay siren.wav` |
| **J4** | Test latence + 0-packet-loss 10 min + stress 60 min | <50 ms latence + 0 underrun ring + 4/4 V4.2 PASS |
| **J5** | Intégration NPU pipeline réelle | Stream NPU stable, mesure performance ML |

### Ce que V3 NE FAIT PAS (sécurité)

- Aucune modif `sof/src/drivers/imx/sai.c` (intouchable)
- Aucune modif kernel ALSA core
- Aucune modif topology m4 V4.2
- Aucune modif IPC SOF (pas de vendor cmd)
- Aucune dépendance probes upstream
- **Aucune modif `memory.h` SOF** (V3 utilise une zone HORS de `MEMORY{}`)
- Aucune modif `imx8m.x.in` linker script

### Plan B (si V3 échoue inopinément)

**Cortex-M7 bridge via RPMsg** — 2-3 semaines mais robuste, totalement découplé du chemin audio critique.

## Validation

**Investigation V3 finale** soumise après création de ce doc pour validation collective. Si GO unanime, J0 démarre.

## Référence

- Investigation V1 → V2 corrections : job `01688a32-341a-4979-85fe-f5ab27d09d7f`
- Investigation V2 → V3 corrections : job `64a4b28e-f6ab-4b02-9cfa-552b0c145b04`
- Décisions archivées : critic_decision V1→V2 `3565abbf-19e4-4091-9bf6-19cfaf4c7708`, V2→V3 (à archiver après création)
- Mémoire projet : `phase_1a2_history.md`, `npu_tap_v1_proposal.md` (à mettre à jour V3)
- Doc maître projet : `PROJECT_STATE.md`
- Pattern reserved-memory existant : `vdev0buffer@94300000` (M7 RPMsg) — V3 utilise même pattern dans `dsp_reserved_heap`
