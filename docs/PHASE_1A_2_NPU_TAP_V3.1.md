# Phase 1a.2 — NPU Tap V3.1 specification

**Status** : V3 validé GO 5/6 par investigation 6 workers (job `23dde533-0dba-4bdc-9b33-f5ecf8a7b3f3`). V3.1 = V3 + 7 améliorations mineures consolidées (A1-A7).
**Date** : 2026-04-26
**Branche** : `feature/audio-platform-v2`
**Précédents** : V1 obsolète, V2 obsolète (NO-GO), V3 obsolète (GO mais incomplet — V3.1 ajoute A1-A7).

---

## Objectif (inchangé V1/V2/V3)

Capturer les blocs audio post-effets OUT (post-MBDRC/PGA/DRC, juste avant le DMA SAI7 TX, **format 8 ch S32_LE @ 48 kHz, 3072 B / période 2 ms = 1.5 MB/s**) depuis la pipeline V4.2 du firmware SOF, et les exposer à un programme userspace A53 qui les pousse au NPU pour analyse ML temps réel.

**Contrainte projet** : tap NPU non-négociable.

## Évolution V3 → V3.1 (7 améliorations consolidées de l'investigation 23dde533)

| # | Amélioration | Source | Impact |
|---|---|---|---|
| **A1** | `dai_common_reset()` → `dd->tap_buffer = NULL` **AVANT** `dai_dma_release()` (l.722), pas avant `buffer_free` | glm-5.1 + qwen | Évite race callback DMA en vol |
| **A2** | `dai_common_params()` → reset `hdr->write_idx = 0` ET `hdr->read_idx = 0` à chaque appel | glm-5.1 | Idempotence rate change / re-prepare |
| **A3** | Hook tap dans le `else` branch (succès `ret >= 0`), pas inconditionnel | claude-code | Pas de tap si `dma_buffer_copy_to` échoue |
| **A4** | Utiliser `audio_stream_rewind_wptr_by_bytes()` (audio_stream.h:778) | claude-code | Helper SOF officiel pour back-walk après `produce` |
| **A5** | **`dcache_writeback_region` sur DATA AVANT le memw, ET sur write_idx APRÈS** | deepseek-v4-pro | Pattern SOF établi (sdma.c:1254-1255), sécurité WT |
| **A6** | Guard overflow runtime : check `bytes <= NPU_TAP_DATA_SIZE` avant memcpy | kimi (point #2 valide) | Protège vdev0vring0@942F0000 contre débordement |
| **A7** | Module kernel : runtime check `rmem->base == 0x942B0000 && rmem->size == 0x40000` après `of_reserved_mem_lookup` | claude-code | Sanity check DT, échoue gracieusement si DT mismatch |

## Solution V3.1

### Architecture cible (inchangée V3)

```
Pipeline V4.2 PIPE 6 playback (inchangé) :

   PCM_host → MBDRC → PGA → DRC → dma_buffer_copy_to() → dd->dma_buffer (OCRAM)
                                                              │
                                                              ▼ SAI7 TX SDMA → HP

   Hook V3.1 dans dai_dma_cb() (post-copy success branch) :
     1. Read dd->dma_buffer via audio_stream_rewind_wptr_by_bytes(stream, bytes)
     2. memcpy(tap_data + w, src, bytes) avec ring wrap × 2 (src + dst)
     3. dcache_writeback_region(tap_data + w, bytes)        ← A5 (DATA flush)
     4. memw                                                 ← ordering
     5. hdr->write_idx = (w + bytes) % ring_size
     6. dcache_writeback_region(&hdr->write_idx, 4)         ← A5 (idx flush)

   A53 userspace via /dev/imx-audio-tap (mmap WC) :
     - mmap PROT_READ
     - widx = READ_ONCE(hdr->write_idx); smp_rmb(); read data
     - push NPU
```

### Adresse confirmée : `0x942B0000`

| Critère | Vérification |
|---|---|
| Cacheattr DSP write-through | `_addr_attr(0x942B0000) = (0x22212222 >> 16) & 0xF = 0x1` (WT). `is_cached() = TRUE` |
| Hors SOF `MEMORY{}` | `0x942B0000 > 0x93400000` (fin SDRAM1). Aucun allocateur SOF n'y touche |
| Carve dans `dsp_reserved_heap` (Linux no-map) | Réduit de `0xef0000` à `0xeb0000` (-256 KB), aucun phandle ne référence cette zone |
| Pas de conflit M7 RPMsg | `0x942B0000 + 0x40000 = 0x942F0000` = pile début `vdev0vring0@942F0000` |
| Aligné | `0x942B0000` aligné 64 KB |

### Modifications par fichier

#### 1. Firmware SOF (`sof/`, ~120 lignes total)

##### `sof/src/include/sof/lib/dai-legacy.h` — ajout 3 champs (V3.1 = V3)

Insertion dans `struct dai_data` (l.169-204) :

```c
/* V3.1 NPU tap (i.MX8MP) */
void *tap_buffer;            /* Ptr direct vers reserved-mem @0x942B0000, NULL = disabled */
uint32_t tap_buffer_size;    /* NPU_TAP_RING_SIZE */
uint32_t tap_period_bytes;   /* sanity check, normalement = period_bytes */
```

##### `sof/src/include/sof/audio/npu_tap.h` (NOUVEAU, ~50 lignes)

```c
#ifndef __SOF_AUDIO_NPU_TAP_H__
#define __SOF_AUDIO_NPU_TAP_H__

#include <stdint.h>

#define NPU_TAP_MAGIC      0x5441504Eu   /* "NPAT" little-endian */
#define NPU_TAP_RING_SIZE  0x40000u      /* 256 KB total (header + data) */
#define NPU_TAP_HDR_SIZE   64u           /* aligned 64 B */
#define NPU_TAP_DATA_SIZE  (NPU_TAP_RING_SIZE - NPU_TAP_HDR_SIZE)
#define NPU_TAP_PHYS_ADDR  0x942B0000u   /* Carve dans dsp_reserved_heap, hors SOF MEMORY{} */

struct npu_tap_hdr {
    uint32_t magic;          /* NPU_TAP_MAGIC */
    uint32_t version;        /* 1 */
    uint32_t ring_size;      /* NPU_TAP_DATA_SIZE */
    uint32_t hdr_size;       /* NPU_TAP_HDR_SIZE */
    uint32_t write_idx;      /* DSP increments (bytes), wrap mod ring_size */
    uint32_t read_idx;       /* A53 increments (bytes), wrap mod ring_size */
    uint32_t period_bytes;   /* 3072 nominal */
    uint32_t sample_rate;    /* 48000 */
    uint32_t channels;       /* 8 */
    uint32_t frame_fmt;      /* SOF_IPC_FRAME_S32_LE */
    uint32_t reserved[6];
} __attribute__((packed));

#endif
```

##### `sof/src/audio/dai-legacy.c` — modifs

**Init au `dai_common_params()`** (insertion entre l.591 et l.593, après `buffer_set_params`) :

```c
/* V3.1 NPU tap : init or reset header (idempotent for rate change) — A2 */
if (dev->direction == SOF_IPC_STREAM_PLAYBACK) {
    dd->tap_buffer = (void *)(uintptr_t)NPU_TAP_PHYS_ADDR;
    dd->tap_buffer_size = NPU_TAP_RING_SIZE;
    dd->tap_period_bytes = period_bytes;

    struct npu_tap_hdr *hdr = (struct npu_tap_hdr *)dd->tap_buffer;
    hdr->magic = NPU_TAP_MAGIC;
    hdr->version = 1;
    hdr->ring_size = NPU_TAP_DATA_SIZE;
    hdr->hdr_size = NPU_TAP_HDR_SIZE;
    hdr->write_idx = 0;                      /* A2 : reset on each params */
    hdr->read_idx = 0;                       /* A2 : reset on each params */
    hdr->period_bytes = period_bytes;
    hdr->sample_rate = 48000;
    hdr->channels = 8;
    hdr->frame_fmt = SOF_IPC_FRAME_S32_LE;
    dcache_writeback_region(hdr, sizeof(*hdr));
    __asm__ volatile ("memw" ::: "memory");
}
```

**Hook au `dai_dma_cb()`** (insertion dans le `else` branch après `dma_buffer_copy_to` réussi, après l.131, avant l.150 `dd->total_data_processed += bytes;`) :

```c
} else { /* dma_buffer_copy_to OK (ret >= 0) — A3 */
    /* V3.1 NPU tap : copy post-DRC to shared ring buffer */
    if (dd->tap_buffer && bytes > 0 && bytes <= NPU_TAP_DATA_SIZE) {  /* A6 guard */
        struct npu_tap_hdr *hdr = (struct npu_tap_hdr *)dd->tap_buffer;
        uint8_t *data_base = (uint8_t *)dd->tap_buffer + NPU_TAP_HDR_SIZE;
        uint32_t ring_size = NPU_TAP_DATA_SIZE;
        uint32_t w = hdr->write_idx;

        /* A4 : back-walk to source start using SOF helper */
        struct audio_stream *stream = &dd->dma_buffer->stream;
        void *src = audio_stream_rewind_wptr_by_bytes(stream, bytes);
        uint32_t src_to_end = audio_stream_bytes_without_wrap(stream, src);
        uint32_t src_head = MIN(bytes, src_to_end);
        uint32_t src_tail = bytes - src_head;

        /* Tap ring write at offset w, with wrap on src AND on tap ring */
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

        /* A5 : flush DATA to DDR (pattern sdma.c:1254-1255) */
        uint32_t total_in_tap = MIN(bytes, ring_size - w);
        dcache_writeback_region(data_base + w, total_in_tap);
        if (bytes > ring_size - w) {
            dcache_writeback_region(data_base, bytes - (ring_size - w));
        }

        /* Memory barrier: data writes visible before write_idx update */
        __asm__ volatile ("memw" ::: "memory");

        /* Atomic 32-bit aligned write */
        hdr->write_idx = (w + bytes) % ring_size;

        /* A5 : flush write_idx */
        dcache_writeback_region(&hdr->write_idx, sizeof(hdr->write_idx));
    }

    dd->total_data_processed += bytes;       /* original l.150 */
}
```

**Cleanup au `dai_common_reset()`** (insertion **AVANT** `dai_dma_release()` à l.722, **A1**) :

```c
/* A1 : NULL tap_buffer FIRST to block any in-flight callback */
dd->tap_buffer = NULL;
dd->tap_buffer_size = 0;
dd->tap_period_bytes = 0;

/* Existing: */
if (!dd->delayed_dma_stop)
    dai_dma_release(dd, dev);
dma_sg_free(&config->elem_array);

if (dd->dma_buffer) {
    buffer_free(dd->dma_buffer);
    dd->dma_buffer = NULL;
}
/* ... */
```

#### 2. Device Tree (DTS, additif)

```dts
&{/reserved-memory/dsp_reserved_heap@93400000} {
    reg = <0x0 0x93400000 0x0 0xeb0000>;   /* 0xef0000 - 0x40000 = -256 KB */
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

#### 3. Kernel module (`meta-local/recipes-kernel/imx-audio-tap/imx-audio-tap.c`, ~140 LOC)

Pattern miscdevice + mmap + sysfs. **A7** : runtime check après `of_reserved_mem_lookup` :

```c
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

    /* A7 : sanity check DT vs spec */
    if (rmem->base != 0x942B0000ULL || rmem->size != 0x40000ULL) {
        dev_err(dev, "DT mismatch: expected base=0x942B0000 size=0x40000, got base=%pa size=%pa\n",
                &rmem->base, &rmem->size);
        return -EINVAL;
    }

    /* ... miscdevice register, sysfs, ... */
}

static int imx_audio_tap_mmap(struct file *file, struct vm_area_struct *vma)
{
    struct imx_audio_tap *priv = file->private_data;

    if (vma->vm_end - vma->vm_start > priv->size)
        return -EINVAL;

    vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
    return remap_pfn_range(vma, vma->vm_start,
                           priv->phys_addr >> PAGE_SHIFT,
                           vma->vm_end - vma->vm_start,
                           vma->vm_page_prot);
}
```

#### 4. App userspace (`meta-local/audio-tools/npu_tap_reader.c`, ~140 LOC)

```c
#include <stdatomic.h>
/* ... open /dev/imx-audio-tap, mmap PROT_READ, validate magic ... */

while (running) {
    uint32_t widx = atomic_load_explicit(&hdr->write_idx, memory_order_acquire);
    /* A53 atomic_load with acquire = READ_ONCE + smp_rmb */

    if (widx != ridx) {
        /* Read data with ring wrap */
        uint32_t bytes = (widx - ridx + ring_size) % ring_size;
        /* memcpy or push to NPU */
        ridx = widx;
    } else {
        usleep(2000);   /* poll @ 500 Hz */
    }
}
```

### Plan en 5 jours (V3.1)

| Jour | Travail | Gate |
|---|---|---|
| **J0** | Revert modifs Alt-A locales `sof/`. Régression V4.2 PASS sur firmware vanilla. Commit V3.1 doc + PROJECT_STATE.md. | `git diff sof/` propre + 4/4 V4.2 PASS |
| **J1** | Firmware : `npu_tap.h` + struct field + init + hook (A1+A2+A3+A4+A5+A6) + cleanup + build + sign + deploy | Build OK + V4.2 régression PASS + magic visible via `devmem 0x942B0000` |
| **J2** | Kernel module + DTS + recipe Yocto (A7) | `modprobe` OK + `/dev/imx-audio-tap` + sysfs + DT check passe |
| **J3** | App userspace `npu_tap_reader` + dump wav | RMS > -100 dB pendant `aplay siren.wav` |
| **J4** | Test latence + 0-packet-loss 10 min + stress 60 min | <50 ms latence + 0 underrun + 4/4 V4.2 PASS |
| **J5** | Intégration NPU pipeline réelle | Stream NPU stable |

### Plan B (si V3.1 échoue inopinément)

**Cortex-M7 bridge via RPMsg** — 2-3 semaines, totalement découplé.

## Référence

- Investigation V1 : job `54dc95c6-256e-42d1-beb9-1a73d7d6a0c5` (verdict GO conditionnel V1)
- Investigation V2 : job `01688a32-341a-4979-85fe-f5ab27d09d7f` (V1 → V2 corrections)
- Investigation V2 (re) : job `64a4b28e-f6ab-4b02-9cfa-552b0c145b04` (V2 NO-GO, overlap HEAP_BUFFER)
- Investigation V3 : job `23dde533-0dba-4bdc-9b33-f5ecf8a7b3f3` (V3 GO 5/6, 7 améliorations identifiées → V3.1)
- Décisions critic_decision : `3565abbf` (V1→V2), `809eae7d` (V2→V3), V3→V3.1 à archiver
- Doc maître projet : `PROJECT_STATE.md`
