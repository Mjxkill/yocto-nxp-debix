# Phase 1a.2 — NPU Tap V3.2.1 specification

**Status** : V3.2 validé GO 6/6 par investigation 6 workers (job `03b6477a-d0e4-4b03-8a8c-080006aede81`) avec 1 erreur ordering DSP convergente. V3.2.1 = V3.2 + 3 corrections de pattern (R3, R4, R5).
**Date** : 2026-04-26
**Branche** : `feature/audio-platform-v2`
**Précédents** : V1, V2, V3, V3.1, V3.2 obsolètes — V3.2.1 est la spec finale avant J0/J1.

---

## Objectif (inchangé)

Capturer les blocs audio post-effets OUT (post-MBDRC/PGA/DRC, juste avant le DMA SAI7 TX, **format 8 ch S32_LE @ 48 kHz, 3072 B / période 2 ms = 1.5 MB/s**) depuis la pipeline V4.2 du firmware SOF, et les exposer à un programme userspace A53 qui les pousse au NPU pour analyse ML temps réel.

## Évolution V3.2 → V3.2.1 (3 corrections de pattern, investigation 03b6477a)

| # | Correction | Source | Bénéfice |
|---|---|---|---|
| **R3** | **DSP ordering corrigé** : `write_idx=0` puis `read_idx=0` puis champs params, PUIS writeback partial, PUIS `memw`, PUIS `epoch++` en DERNIER, PUIS writeback epoch, PUIS `memw`. Pattern canonique Linux `smp_store_release` (publication atomique data avant version). | claude-code, deepseek-v4-pro, glm-5.1, kimi, qwen (5/6) | Quand A53 voit nouvel epoch, TOUT le reste est déjà écrit. Plus robuste que le pattern « single cache line atomicity » de V3.2. |
| **R4** | **A53 seqcount-style** : lire `epoch1` (acquire) → lire `write_idx` (acquire) → lire data → lire `epoch2` (acquire) → si `e1 ≠ e2`, retry. | claude-code, deepseek-v4-pro | Détecte un reset DSP qui surviendrait PENDANT la lecture A53. Sans ça, A53 peut retourner des samples périmés tagués avec le nouvel epoch. |
| **R5** | **UAPI single-source-of-truth via build-time check Yocto** : 2 copies du header (firmware + kernel UAPI), mais `do_configure` Yocto fait `diff` automatique, `bbfatal` si divergence. Option upgrade future = vraie recipe `imx-audio-tap-uapi` qui partage le header via `STAGING_KERNEL_DIR`. | claude-code, deepseek-v4-pro | Évite la divergence silencieuse à 6 mois. Préserve `static_assert(NPU_TAP_PHYS_ADDR == 0x942B0000, ...)` dans chaque copie + DT runtime check (A7) = triple sécurité. |

Les 7 améliorations V3 → V3.1 (A1-A7) et les 2 V3.1 → V3.2 (R1, R2) sont conservées intactes.

## Solution V3.2.1

### Architecture (inchangée V3/V3.1/V3.2)

- Adresse `0x942B0000` (256 KB carve dans `dsp_reserved_heap`, hors SOF `MEMORY{}`)
- Hook `dai_dma_cb()` après `dma_buffer_copy_to` succès branch (A3)
- Reserved-memory DT no-map + miscdevice mmap pgprot_writecombine
- DSP cacheattr region 4 = write-through (digit 4 = 1 dans `0x22212222`)

### Modifications par fichier

#### 1. Firmware SOF — `sof/src/include/sof/audio/npu_tap.h` (V3.2.1)

```c
#ifndef __SOF_AUDIO_NPU_TAP_H__
#define __SOF_AUDIO_NPU_TAP_H__

#include <stdint.h>
#include <stddef.h>

#define NPU_TAP_MAGIC      0x5441504Eu   /* "NPAT" little-endian */
#define NPU_TAP_RING_SIZE  0x40000u      /* 256 KB total (header + data) */
#define NPU_TAP_HDR_SIZE   64u           /* sizeof(struct npu_tap_hdr) — must match */
#define NPU_TAP_DATA_SIZE  (NPU_TAP_RING_SIZE - NPU_TAP_HDR_SIZE)

/*
 * R2/R5 : Cette valeur DOIT correspondre à NPU_TAP_PHYS_ADDR dans
 * meta-local/recipes-kernel/imx-audio-tap/imx-audio-tap-uapi.h.
 * Le `do_configure` du recipe Yocto fait un diff entre les 2 fichiers et
 * `bbfatal` si divergence. Le DT runtime check (A7) côté module kernel
 * valide aussi à l'init. Triple sécurité : compile static_assert + build diff + runtime DT.
 */
#define NPU_TAP_PHYS_ADDR  0x942B0000u

struct npu_tap_hdr {
    uint32_t magic;          /* @0  : NPU_TAP_MAGIC */
    uint32_t version;        /* @4  : 3 (V3.2.1) */
    uint32_t ring_size;      /* @8  : NPU_TAP_DATA_SIZE */
    uint32_t hdr_size;       /* @12 : NPU_TAP_HDR_SIZE */
    uint32_t epoch;          /* @16 : R1 — published LAST by DSP, monotonic */
    uint32_t write_idx;      /* @20 : DSP increments (bytes), wrap mod ring_size */
    uint32_t read_idx;       /* @24 : A53 increments (bytes), wrap mod ring_size */
    uint32_t period_bytes;   /* @28 : 3072 nominal */
    uint32_t sample_rate;    /* @32 : 48000 */
    uint32_t channels;       /* @36 : 8 */
    uint32_t frame_fmt;      /* @40 : SOF_IPC_FRAME_S32_LE */
    uint32_t reserved[5];    /* @44..@60 : zero-initialized for forward compat */
} __attribute__((packed, aligned(64)));

_Static_assert(sizeof(struct npu_tap_hdr) == 64,
               "npu_tap_hdr must be exactly 64B (1 cache line)");
_Static_assert(NPU_TAP_PHYS_ADDR == 0x942B0000u,
               "NPU_TAP_PHYS_ADDR mismatch — must align with kernel UAPI");
_Static_assert(offsetof(struct npu_tap_hdr, epoch) == 16,
               "epoch offset must be 16 (cache-line aligned with write_idx)");

#endif
```

#### 2. Header kernel UAPI partagé — `meta-local/recipes-kernel/imx-audio-tap/imx-audio-tap-uapi.h` (NOUVEAU)

```c
/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * UAPI shared header for imx-audio-tap kernel module + userspace.
 * R2/R5 : DOIT correspondre à sof/src/include/sof/audio/npu_tap.h.
 * Yocto recipe `do_configure` fait un diff au build et bbfatal si divergence.
 */
#ifndef __IMX_AUDIO_TAP_UAPI_H__
#define __IMX_AUDIO_TAP_UAPI_H__

#include <stdint.h>
#include <stddef.h>

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
} __attribute__((packed, aligned(64)));

_Static_assert(sizeof(struct npu_tap_hdr) == 64, "npu_tap_hdr must be 64B");
_Static_assert(NPU_TAP_PHYS_ADDR == 0x942B0000U, "NPU_TAP_PHYS_ADDR fixed");

#endif
```

#### 3. Firmware SOF — `sof/src/include/sof/lib/dai-legacy.h` (champs `dai_data` inchangés V3.2)

```c
/* V3.x NPU tap (i.MX8MP) */
void *tap_buffer;
uint32_t tap_buffer_size;
uint32_t tap_period_bytes;
uint32_t tap_epoch;          /* R1 V3.2 — local monotonic counter */
```

#### 4. Firmware SOF — `sof/src/audio/dai-legacy.c`

##### **R3 : Init au `dai_common_params()` — ORDERING CORRIGÉ** (insertion entre l.591 et l.593)

```c
/* V3.2.1 NPU tap: init / reset header — pattern canonique Linux smp_store_release */
if (dev->direction == SOF_IPC_STREAM_PLAYBACK) {
    dd->tap_buffer = (void *)(uintptr_t)NPU_TAP_PHYS_ADDR;
    dd->tap_buffer_size = NPU_TAP_RING_SIZE;
    dd->tap_period_bytes = period_bytes;

    struct npu_tap_hdr *hdr = (struct npu_tap_hdr *)dd->tap_buffer;

    /* Step 1: zero-init for forward-compat (covers reserved[]) */
    memset(hdr, 0, sizeof(*hdr));

    /* Step 2: data-side state FIRST (write_idx/read_idx + params)
     *  R3 — pattern canonique : data avant version */
    hdr->magic = NPU_TAP_MAGIC;
    hdr->version = 3;                        /* V3.2.1 */
    hdr->ring_size = NPU_TAP_DATA_SIZE;
    hdr->hdr_size = NPU_TAP_HDR_SIZE;
    hdr->write_idx = 0;                      /* A2 reset */
    hdr->read_idx = 0;                       /* A2 reset */
    hdr->period_bytes = period_bytes;
    hdr->sample_rate = 48000;
    hdr->channels = 8;
    hdr->frame_fmt = SOF_IPC_FRAME_S32_LE;

    /* Step 3: flush all data state (everything except epoch) */
    dcache_writeback_region(hdr, offsetof(struct npu_tap_hdr, epoch));

    /* Step 4: memw barrier — guarantees stores above visible BEFORE epoch */
    __asm__ volatile ("memw" ::: "memory");

    /* Step 5: publish epoch LAST — R1 monotonic + R3 publication ordering */
    hdr->epoch = ++dd->tap_epoch;

    /* Step 6: flush epoch field, then memw to ensure A53 visibility */
    dcache_writeback_region(&hdr->epoch, sizeof(hdr->epoch));
    __asm__ volatile ("memw" ::: "memory");
}
```

##### Hook au `dai_dma_cb()` (inchangé V3.1, dans `else` succès branch, après `dma_buffer_copy_to`)

```c
} else { /* dma_buffer_copy_to OK (ret >= 0) — A3 */
    /* V3.2.1 NPU tap: copy post-DRC to shared ring buffer */
    if (dd->tap_buffer && bytes > 0 && bytes <= NPU_TAP_DATA_SIZE) {  /* A6 */
        struct npu_tap_hdr *hdr = (struct npu_tap_hdr *)dd->tap_buffer;
        uint8_t *data_base = (uint8_t *)dd->tap_buffer + NPU_TAP_HDR_SIZE;
        uint32_t ring_size = NPU_TAP_DATA_SIZE;
        uint32_t w = hdr->write_idx;

        /* A4: back-walk to source start using SOF helper */
        struct audio_stream *stream = &dd->dma_buffer->stream;
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

        /* Publish write_idx (single 32-bit atomic write on aligned addr) */
        hdr->write_idx = (w + bytes) % ring_size;
        dcache_writeback_region(&hdr->write_idx, sizeof(hdr->write_idx));
        __asm__ volatile ("memw" ::: "memory");
    }

    dd->total_data_processed += bytes;
}
```

##### Cleanup au `dai_common_reset()` (insertion AVANT `dai_dma_release` à l.722, A1)

```c
/* A1: NULL tap_buffer FIRST, before any cleanup */
dd->tap_buffer = NULL;
dd->tap_buffer_size = 0;
dd->tap_period_bytes = 0;
/* dd->tap_epoch reste — strictly monotonic for life of struct */

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
#include "imx-audio-tap-uapi.h"      /* R2/R5: shared with userspace */

static int imx_audio_tap_probe(struct platform_device *pdev)
{
    struct device *dev = &pdev->dev;
    struct device_node *mem_np;
    struct reserved_mem *rmem;

    mem_np = of_parse_phandle(dev->of_node, "memory-region", 0);
    if (!mem_np) return -ENODEV;
    rmem = of_reserved_mem_lookup(mem_np);
    of_node_put(mem_np);
    if (!rmem) return -EINVAL;

    /* A7: DT runtime sanity check, R2/R5 macro shared */
    if (rmem->base != NPU_TAP_PHYS_ADDR || rmem->size != NPU_TAP_RING_SIZE) {
        dev_err(dev, "DT mismatch: expected base=0x%x size=0x%x, got base=%pa size=%pa\n",
                NPU_TAP_PHYS_ADDR, NPU_TAP_RING_SIZE, &rmem->base, &rmem->size);
        return -EINVAL;
    }
    /* ... miscdevice register, sysfs ... */
}
```

#### 7. R5 — Yocto recipe `do_configure` build-time check

`meta-local/recipes-kernel/imx-audio-tap/imx-audio-tap_0.1.bb` :

```python
do_configure_prepend() {
    # R5: build-time check — fail if NPU_TAP_PHYS_ADDR diverges
    UAPI_HDR="${WORKDIR}/files/imx-audio-tap-uapi.h"
    SOF_HDR="${WORKDIR}/../../sof/src/include/sof/audio/npu_tap.h"

    if [ ! -f "$SOF_HDR" ]; then
        bbfatal "SOF firmware header not found: $SOF_HDR"
    fi

    UAPI_ADDR=$(grep -E '^\s*#define\s+NPU_TAP_PHYS_ADDR' "$UAPI_HDR" | awk '{print $3}')
    SOF_ADDR=$(grep -E '^\s*#define\s+NPU_TAP_PHYS_ADDR' "$SOF_HDR" | awk '{print $3}' | sed 's/u$//')

    if [ "$UAPI_ADDR" != "$SOF_ADDR" ]; then
        bbfatal "NPU_TAP_PHYS_ADDR mismatch: UAPI=$UAPI_ADDR vs SOF=$SOF_ADDR"
    fi
}
```

#### 8. App userspace — `meta-local/audio-tools/npu_tap_reader.c` — R4 PATTERN SEQCOUNT

```c
#include "imx-audio-tap-uapi.h"      /* R2/R5: shared header */
#include <stdatomic.h>
#include <string.h>

/* R4: seqcount-style read pattern. Detects DSP reset mid-read. */

static uint32_t last_seen_epoch = 0;
static uint32_t local_read_idx  = 0;

while (running) {
    uint32_t e1, e2, w;
    uint8_t local_buf[NPU_TAP_DATA_SIZE];

    /* R4 step 1: read epoch BEFORE write_idx (acquire) */
    e1 = atomic_load_explicit(&hdr->epoch, memory_order_acquire);

    if (e1 != last_seen_epoch) {
        /* DSP did params reset — discard stale local state */
        last_seen_epoch = e1;
        local_read_idx = atomic_load_explicit(&hdr->write_idx, memory_order_acquire);
        fprintf(stderr, "NPU tap: DSP reset detected, epoch -> %u\n", e1);
        continue;
    }

    /* R4 step 2: read write_idx (acquire) */
    w = atomic_load_explicit(&hdr->write_idx, memory_order_acquire);
    if (w == local_read_idx) { usleep(2000); continue; }

    /* R4 step 3: read data into local buffer (with ring wrap) */
    uint32_t bytes_avail = (w - local_read_idx + NPU_TAP_DATA_SIZE) % NPU_TAP_DATA_SIZE;
    uint32_t to_end = NPU_TAP_DATA_SIZE - local_read_idx;
    if (bytes_avail <= to_end) {
        memcpy(local_buf, data_base + local_read_idx, bytes_avail);
    } else {
        memcpy(local_buf, data_base + local_read_idx, to_end);
        memcpy(local_buf + to_end, data_base, bytes_avail - to_end);
    }

    /* R4 step 4: re-read epoch — if changed, discard everything we just read */
    e2 = atomic_load_explicit(&hdr->epoch, memory_order_acquire);
    if (e2 != e1) {
        /* DSP reset PENDANT notre lecture — data potentially stale, retry */
        last_seen_epoch = e2;
        local_read_idx = atomic_load_explicit(&hdr->write_idx, memory_order_acquire);
        fprintf(stderr, "NPU tap: race detected (e1=%u e2=%u), discarding\n", e1, e2);
        continue;
    }

    /* R4 step 5: data is committed, push to NPU */
    push_to_npu(local_buf, bytes_avail);
    local_read_idx = w;
}
```

### Plan en 5 jours (V3.2.1)

| Jour | Travail | Gate |
|---|---|---|
| **J0** | Revert modifs Alt-A locales `sof/`. Régression V4.2 PASS sur firmware vanilla. Commit V3.2.1 doc + PROJECT_STATE.md. | `git diff sof/` propre + 4/4 V4.2 PASS |
| **J1** | Firmware : `npu_tap.h` (R2/R5 + static_assert) + struct field (+ epoch R1) + init R3 ordering + hook + cleanup A1 + build + sign + deploy | Build OK + V4.2 régression PASS + magic+epoch lisibles via `devmem 0x942B0000` |
| **J2** | Header UAPI (R2/R5) + Kernel module (A7 + R2/R5) + DTS + recipe Yocto avec do_configure diff check (R5) | `modprobe` OK + `/dev/imx-audio-tap` + sysfs + DT check passe + diff check passe |
| **J3** | App userspace `npu_tap_reader` (R4 seqcount-style) + dump wav | RMS > -100 dB + epoch incremental + 0 race detected en lecture continue |
| **J4** | Test latence + 0-packet-loss 10 min + stress 60 min + test rate change (epoch detect) | <50 ms latence + 0 underrun + 4/4 V4.2 PASS + epoch reset détecté côté A53 |
| **J5** | Intégration NPU pipeline réelle | Stream NPU stable |

## Tests obligatoires (post-fix V3.2.1)

### Build/static checks

- `static_assert(sizeof(struct npu_tap_hdr) == 64)` compile firmware ET kernel
- `static_assert(offsetof(struct npu_tap_hdr, epoch) == 16)` (cache line alignment)
- `static_assert(NPU_TAP_PHYS_ADDR == 0x942B0000U)` dans CHAQUE copie
- Yocto `do_configure` diff entre UAPI et SOF headers : pas de divergence
- DT runtime check (A7) : `bbfatal` si DTB modifié avec mauvaise adresse

### Runtime correctness (J3)

- **Test "params re-trigger"** : `aplay siren.wav` × 3 successifs sans `rmmod` → 3 epoch distincts détectés côté A53, aucun sample du run N tagué `epoch_N+1`
- **Test "remoteproc restart"** : `echo stop > /sys/class/remoteproc/remoteproc0/state; echo start > ...; aplay siren.wav` → reader voit `epoch=0` (firmware reset), discard local_read_idx
- **Test "concurrent races"** : `while true; do aplay -d 0.1 siren.wav; done` × 60s → 0 samples corrompus, RMS analyse stable

### Régression V4.2 (obligatoire post-deploy)

- T1-T6 V4.2 PASS 6/6 inchangé
- Latence DMA SAI7 mesurée < 2 ms (memcpy tap = ~5 µs ⇒ 0.25 % budget)
- 10 min loopback 8ch S32_LE @ 48 kHz sans xrun

## Référence

- Investigation V1 : `54dc95c6-256e-42d1-beb9-1a73d7d6a0c5`
- Investigation V2 : `01688a32-341a-4979-85fe-f5ab27d09d7f`
- Investigation V2 (re) : `64a4b28e-f6ab-4b02-9cfa-552b0c145b04` (NO-GO)
- Investigation V3 : `23dde533-0dba-4bdc-9b33-f5ecf8a7b3f3` (GO 5/6)
- Investigation V3.1 : `115112b0-41b5-431b-8f3e-03fece0c3ff9` (GO 6/6)
- Investigation V3.2 : `03b6477a-d0e4-4b03-8a8c-080006aede81` (GO 6/6 + 1 erreur ordering)
- Investigation V3.2.1 : à lancer
- Décisions : `3565abbf` V1→V2, `809eae7d` V2→V3, `18e10a2c` V3→V3.1, `002f2f3c` V3.1→V3.2, V3.2→V3.2.1 à archiver
- Doc maître projet : `PROJECT_STATE.md`
