# Phase 1a.2 — NPU Tap V3.2 specification

**Status** : V3.1 validé GO 6/6 par investigation 6 workers (job `115112b0-41b5-431b-8f3e-03fece0c3ff9`). V3.2 = V3.1 + 2 améliorations consolidées (R1 epoch counter, R2 macro partagé).
**Date** : 2026-04-26
**Branche** : `feature/audio-platform-v2`
**Précédents** : V1, V2, V3, V3.1 obsolètes — V3.2 est la spec finale avant J0/J1.

---

## Objectif (inchangé)

Capturer les blocs audio post-effets OUT (post-MBDRC/PGA/DRC, juste avant le DMA SAI7 TX, **format 8 ch S32_LE @ 48 kHz, 3072 B / période 2 ms = 1.5 MB/s**) depuis la pipeline V4.2 du firmware SOF, et les exposer à un programme userspace A53 qui les pousse au NPU pour analyse ML temps réel.

## Évolution V3.1 → V3.2 (2 améliorations consolidées de l'investigation 115112b0)

| # | Amélioration V3.2 | Source | Bénéfice |
|---|---|---|---|
| **R1** | Ajouter `epoch` counter monotone dans `npu_tap_hdr`. DSP l'incrémente à chaque `dai_common_params()`. A53 le lit en premier ; si changé depuis dernière lecture → discard ring buffer + reset `read_idx = write_idx`. | deepseek-v4-pro, glm-5.1, kimi, qwen | Robustesse rate change : A53 ne lit pas data périmées si DSP reset write_idx mid-flight |
| **R2** | Centraliser `NPU_TAP_PHYS_ADDR = 0x942B0000` dans un header partagé `meta-local/recipes-kernel/imx-audio-tap/imx-audio-tap-uapi.h`. SOF firmware garde sa copie dans `sof/src/include/sof/audio/npu_tap.h` avec `static_assert` au build (fail si divergence). DT runtime check (A7) = filet final. | deepseek-v4-pro, glm-5.1, kimi | Évite divergence silencieuse fw/kernel si l'adresse change un jour |

Les 7 améliorations V3 → V3.1 (A1-A7) sont conservées intactes.

## Solution V3.2

### Architecture (inchangée V3/V3.1)

- Adresse `0x942B0000` (256 KB carve dans `dsp_reserved_heap`, hors SOF `MEMORY{}`)
- Hook `dai_dma_cb()` après `dma_buffer_copy_to` succès branch (A3)
- Reserved-memory DT no-map + miscdevice mmap pgprot_writecombine
- DSP cacheattr region 4 = write-through (digit 4 = 1 dans `0x22212222`)
- A53 : `atomic_load_acquire` (équivalent `READ_ONCE` + `smp_rmb`)

### Modifications par fichier

#### 1. Firmware SOF — `sof/src/include/sof/audio/npu_tap.h` (V3.2)

```c
#ifndef __SOF_AUDIO_NPU_TAP_H__
#define __SOF_AUDIO_NPU_TAP_H__

#include <stdint.h>

#define NPU_TAP_MAGIC      0x5441504Eu   /* "NPAT" little-endian */
#define NPU_TAP_RING_SIZE  0x40000u      /* 256 KB total (header + data) */
#define NPU_TAP_HDR_SIZE   64u
#define NPU_TAP_DATA_SIZE  (NPU_TAP_RING_SIZE - NPU_TAP_HDR_SIZE)

/*
 * R2 : Cette valeur DOIT correspondre à NPU_TAP_PHYS_ADDR dans
 * meta-local/recipes-kernel/imx-audio-tap/imx-audio-tap-uapi.h
 * Le DT runtime check (A7) côté module kernel valide à l'init.
 * Modifier l'un sans l'autre = corruption mémoire silencieuse.
 */
#define NPU_TAP_PHYS_ADDR  0x942B0000u

struct npu_tap_hdr {
    uint32_t magic;          /* NPU_TAP_MAGIC */
    uint32_t version;        /* 2 (V3.2: epoch added) */
    uint32_t ring_size;      /* NPU_TAP_DATA_SIZE */
    uint32_t hdr_size;       /* NPU_TAP_HDR_SIZE */
    uint32_t epoch;          /* R1: DSP incr at each dai_common_params, A53 detects reset */
    uint32_t write_idx;      /* DSP increments (bytes), wrap mod ring_size */
    uint32_t read_idx;       /* A53 increments (bytes), wrap mod ring_size */
    uint32_t period_bytes;   /* 3072 nominal */
    uint32_t sample_rate;    /* 48000 */
    uint32_t channels;       /* 8 */
    uint32_t frame_fmt;      /* SOF_IPC_FRAME_S32_LE */
    uint32_t reserved[5];
} __attribute__((packed));

/* Compile-time sanity */
_Static_assert(sizeof(struct npu_tap_hdr) <= NPU_TAP_HDR_SIZE,
               "npu_tap_hdr exceeds NPU_TAP_HDR_SIZE");

#endif
```

#### 2. Header kernel partagé — `meta-local/recipes-kernel/imx-audio-tap/imx-audio-tap-uapi.h` (NOUVEAU)

```c
/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * UAPI shared header for imx-audio-tap kernel module + userspace.
 * R2 : NPU_TAP_PHYS_ADDR DOIT correspondre à sof/src/include/sof/audio/npu_tap.h.
 * Le DT runtime check (A7) valide à l'init du module kernel.
 */
#ifndef __IMX_AUDIO_TAP_UAPI_H__
#define __IMX_AUDIO_TAP_UAPI_H__

#define NPU_TAP_MAGIC          0x5441504EU   /* "NPAT" */
#define NPU_TAP_RING_SIZE      0x40000U      /* 256 KB */
#define NPU_TAP_HDR_SIZE       64U
#define NPU_TAP_DATA_SIZE      (NPU_TAP_RING_SIZE - NPU_TAP_HDR_SIZE)
#define NPU_TAP_PHYS_ADDR      0x942B0000U

struct npu_tap_hdr {
    uint32_t magic;
    uint32_t version;
    uint32_t ring_size;
    uint32_t hdr_size;
    uint32_t epoch;
    uint32_t write_idx;
    uint32_t read_idx;
    uint32_t period_bytes;
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t frame_fmt;
    uint32_t reserved[5];
} __attribute__((packed));

#endif
```

#### 3. Firmware SOF — `sof/src/include/sof/lib/dai-legacy.h` (champs `dai_data` inchangés V3.1)

```c
/* V3.x NPU tap (i.MX8MP) */
void *tap_buffer;
uint32_t tap_buffer_size;
uint32_t tap_period_bytes;
uint32_t tap_epoch;          /* R1: NEW V3.2 — local epoch increment counter */
```

#### 4. Firmware SOF — `sof/src/audio/dai-legacy.c`

**Init au `dai_common_params()`** (insertion entre l.591 et l.593) :

```c
/* V3.2 NPU tap: init / reset header (idempotent for rate change) */
if (dev->direction == SOF_IPC_STREAM_PLAYBACK) {
    dd->tap_buffer = (void *)(uintptr_t)NPU_TAP_PHYS_ADDR;
    dd->tap_buffer_size = NPU_TAP_RING_SIZE;
    dd->tap_period_bytes = period_bytes;
    dd->tap_epoch++;                          /* R1: monotonic on each params */

    struct npu_tap_hdr *hdr = (struct npu_tap_hdr *)dd->tap_buffer;
    hdr->magic = NPU_TAP_MAGIC;
    hdr->version = 2;
    hdr->ring_size = NPU_TAP_DATA_SIZE;
    hdr->hdr_size = NPU_TAP_HDR_SIZE;
    hdr->epoch = dd->tap_epoch;               /* R1: A53 detects reset */
    hdr->write_idx = 0;                       /* A2: reset on each params */
    hdr->read_idx = 0;                        /* A2: reset on each params */
    hdr->period_bytes = period_bytes;
    hdr->sample_rate = 48000;
    hdr->channels = 8;
    hdr->frame_fmt = SOF_IPC_FRAME_S32_LE;
    dcache_writeback_region(hdr, sizeof(*hdr));
    __asm__ volatile ("memw" ::: "memory");
}
```

**Hook au `dai_dma_cb()`** (inchangé V3.1, après `dma_buffer_copy_to` succès) :

```c
} else { /* dma_buffer_copy_to OK (ret >= 0) — A3 */
    /* V3.2 NPU tap: copy post-DRC to shared ring buffer */
    if (dd->tap_buffer && bytes > 0 && bytes <= NPU_TAP_DATA_SIZE) {  /* A6 */
        struct npu_tap_hdr *hdr = (struct npu_tap_hdr *)dd->tap_buffer;
        uint8_t *data_base = (uint8_t *)dd->tap_buffer + NPU_TAP_HDR_SIZE;
        uint32_t ring_size = NPU_TAP_DATA_SIZE;
        uint32_t w = hdr->write_idx;

        struct audio_stream *stream = &dd->dma_buffer->stream;       /* A4 */
        void *src = audio_stream_rewind_wptr_by_bytes(stream, bytes);
        uint32_t src_to_end = audio_stream_bytes_without_wrap(stream, src);
        uint32_t src_head = MIN(bytes, src_to_end);
        uint32_t src_tail = bytes - src_head;

        /* Tap ring write at offset w with wrap × 2 (src + tap) */
        uint32_t tap_to_end = ring_size - w;
        uint32_t head_in_tap = MIN(src_head, tap_to_end);
        memcpy_s(data_base + w, ring_size - w, src, head_in_tap);
        if (src_head > tap_to_end) {
            memcpy_s(data_base, ring_size,
                     (uint8_t *)src + tap_to_end, src_head - tap_to_end);
        }

        if (src_tail) {
            void *src2 = audio_stream_get_addr(stream);
            uint32_t new_w = (w + src_head) % ring_size;
            uint32_t tap_to_end2 = ring_size - new_w;
            uint32_t tail_in_tap = MIN(src_tail, tap_to_end2);
            memcpy_s(data_base + new_w, ring_size - new_w, src2, tail_in_tap);
            if (src_tail > tap_to_end2) {
                memcpy_s(data_base, ring_size,
                         (uint8_t *)src2 + tap_to_end2, src_tail - tap_to_end2);
            }
        }

        /* A5: flush DATA before write_idx (pattern sdma.c:1254-1255) */
        uint32_t total_in_tap = MIN(bytes, ring_size - w);
        dcache_writeback_region(data_base + w, total_in_tap);
        if (bytes > ring_size - w) {
            dcache_writeback_region(data_base, bytes - (ring_size - w));
        }

        __asm__ volatile ("memw" ::: "memory");

        hdr->write_idx = (w + bytes) % ring_size;
        dcache_writeback_region(&hdr->write_idx, sizeof(hdr->write_idx));
    }

    dd->total_data_processed += bytes;
}
```

**Cleanup au `dai_common_reset()`** (insertion AVANT `dai_dma_release` à l.722, A1) :

```c
/* A1: NULL tap_buffer FIRST, before any cleanup */
dd->tap_buffer = NULL;
dd->tap_buffer_size = 0;
dd->tap_period_bytes = 0;
/* dd->tap_epoch reste : compteur monotone à vie */

if (!dd->delayed_dma_stop)
    dai_dma_release(dd, dev);
/* ... reste inchangé ... */
```

#### 5. Device Tree (DTS, additif — inchangé V3.1)

```dts
&{/reserved-memory/dsp_reserved_heap@93400000} {
    reg = <0x0 0x93400000 0x0 0xeb0000>;   /* -256 KB */
};

&{/reserved-memory} {
    npu_tap_buffer: npu_tap_buffer@942b0000 {
        compatible = "shared-dma-pool";
        reg = <0x0 0x942b0000 0x0 0x40000>;
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

#### 6. Kernel module — `meta-local/recipes-kernel/imx-audio-tap/imx-audio-tap.c` (~150 LOC)

```c
#include "imx-audio-tap-uapi.h"      /* R2: shared with userspace */

static int imx_audio_tap_probe(struct platform_device *pdev)
{
    struct device *dev = &pdev->dev;
    struct device_node *mem_np;
    struct reserved_mem *rmem;

    mem_np = of_parse_phandle(dev->of_node, "memory-region", 0);
    if (!mem_np)
        return -ENODEV;
    rmem = of_reserved_mem_lookup(mem_np);
    of_node_put(mem_np);
    if (!rmem)
        return -EINVAL;

    /* A7: DT sanity check, R2: use shared macro */
    if (rmem->base != NPU_TAP_PHYS_ADDR || rmem->size != NPU_TAP_RING_SIZE) {
        dev_err(dev, "DT mismatch: expected base=0x%x size=0x%x, got base=%pa size=%pa\n",
                NPU_TAP_PHYS_ADDR, NPU_TAP_RING_SIZE, &rmem->base, &rmem->size);
        return -EINVAL;
    }

    /* ... miscdevice register, sysfs ... */
}

static int imx_audio_tap_mmap(struct file *file, struct vm_area_struct *vma)
{
    struct imx_audio_tap *priv = file->private_data;
    if (vma->vm_end - vma->vm_start > priv->size) return -EINVAL;
    vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
    return remap_pfn_range(vma, vma->vm_start, priv->phys_addr >> PAGE_SHIFT,
                           vma->vm_end - vma->vm_start, vma->vm_page_prot);
}
```

#### 7. App userspace — `meta-local/audio-tools/npu_tap_reader.c` (~150 LOC)

```c
#include "imx-audio-tap-uapi.h"      /* R2: shared header */
#include <stdatomic.h>

/* ... open /dev/imx-audio-tap, mmap PROT_READ, validate magic ... */

uint32_t last_epoch = 0;
uint32_t ridx = 0;

while (running) {
    uint32_t epoch = atomic_load_explicit(&hdr->epoch, memory_order_acquire);  /* R1 */

    if (epoch != last_epoch) {
        /* DSP reset detected: discard ring, sync to current write_idx */
        ridx = atomic_load_explicit(&hdr->write_idx, memory_order_acquire);
        last_epoch = epoch;
        fprintf(stderr, "NPU tap: DSP reset detected, epoch %u -> %u\n",
                last_epoch, epoch);
        continue;
    }

    uint32_t widx = atomic_load_explicit(&hdr->write_idx, memory_order_acquire);

    if (widx != ridx) {
        uint32_t bytes_avail = (widx - ridx + NPU_TAP_DATA_SIZE) % NPU_TAP_DATA_SIZE;
        /* ... read with ring wrap, push NPU ... */
        ridx = widx;
    } else {
        usleep(2000);   /* poll @ 500 Hz */
    }
}
```

### Plan en 5 jours (V3.2)

| Jour | Travail | Gate |
|---|---|---|
| **J0** | Revert modifs Alt-A locales `sof/`. Régression V4.2 PASS sur firmware vanilla. Commit V3.2 doc + PROJECT_STATE.md mis à jour. | `git diff sof/` propre + 4/4 V4.2 PASS |
| **J1** | Firmware : `npu_tap.h` (R2) + struct field (+ epoch R1) + init + hook + cleanup + build + sign + deploy | Build OK + V4.2 régression PASS + magic/epoch lisibles via `devmem 0x942B0000` |
| **J2** | Header UAPI partagé (R2) + Kernel module (A7 + R2) + DTS + recipe Yocto | `modprobe` OK + `/dev/imx-audio-tap` + sysfs + DT check passe |
| **J3** | App userspace `npu_tap_reader` (R1 epoch detect) + dump wav | RMS > -100 dB pendant `aplay siren.wav` + epoch incremental |
| **J4** | Test latence + 0-packet-loss 10 min + stress 60 min + test rate change | <50 ms latence + 0 underrun + 4/4 V4.2 PASS + epoch reset détecté côté A53 |
| **J5** | Intégration NPU pipeline réelle | Stream NPU stable |

## Référence

- Investigation V1 : `54dc95c6-256e-42d1-beb9-1a73d7d6a0c5`
- Investigation V2 : `01688a32-341a-4979-85fe-f5ab27d09d7f`
- Investigation V2 (re) : `64a4b28e-f6ab-4b02-9cfa-552b0c145b04` (NO-GO)
- Investigation V3 : `23dde533-0dba-4bdc-9b33-f5ecf8a7b3f3` (GO 5/6)
- Investigation V3.1 : `115112b0-41b5-431b-8f3e-03fece0c3ff9` (GO 6/6)
- Investigation V3.2 : à lancer
- Décisions : `3565abbf` V1→V2, `809eae7d` V2→V3, `18e10a2c` V3→V3.1, V3.1→V3.2 à archiver
- Doc maître projet : `PROJECT_STATE.md`
