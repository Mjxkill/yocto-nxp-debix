# Phase 1a.2 — NPU Tap V3.2.2 specification

**Status** : V3.2.1 validé GO 5/6 par investigation 6 workers (job `6f4cfb00-56e8-4390-b25b-931ace1277f1`) avec 1 RÉSERVE convergente sur multi-DAI. V3.2.2 = V3.2.1 + 2 amendements (R6 ring align, R7 sentinelle mono-DAI).
**Date** : 2026-04-26
**Branche** : `feature/audio-platform-v2`
**Précédents** : V1, V2, V3, V3.1, V3.2, V3.2.1 obsolètes — V3.2.2 vise GO unanime avant J0/J1.

---

## Objectif (inchangé)

Capturer les blocs audio post-effets OUT (post-MBDRC/PGA/DRC, juste avant le DMA SAI7 TX, **format 8 ch S32_LE @ 48 kHz, 3072 B / période 2 ms = 1.5 MB/s**) depuis la pipeline V4.2 du firmware SOF, et les exposer à un programme userspace A53 qui les pousse au NPU pour analyse ML temps réel.

## Évolution V3.2.1 → V3.2.2 (2 amendements + 6 micro-amendements J1)

V3.2.2 issu de l'investigation 6f4cfb00 (V3.2.1 GO 5/6). V3.2.2 confirmé GO 6/6 par investigation `f6e0efae-67c1-4302-9057-936e058040a4`. Les 6 micro-amendements (M1-M6) sont **intégrés inline dans cette spec** (pas de V3.3).

### Amendements R6/R7 (V3.2.1 → V3.2.2)

| # | Amendement | Source | Bénéfice |
|---|---|---|---|
| **R6** | **Ring align runtime** : `header->ring_size` calculé au `dai_common_params()` comme `(NPU_TAP_DATA_SIZE_MAX / period_bytes) * period_bytes`. Pour V4.2 (period_bytes=3072) : 262016 / 3072 × 3072 = 261120 B (= 85 périodes). A53 utilise `header->ring_size`, PAS la macro `NPU_TAP_DATA_SIZE_MAX`. | claude-code, deepseek-v4-pro | Pas de split mid-période sur le wrap, latence uniforme. |
| **R7** | **Sentinelle mono-DAI** : variable globale `struct dai_data *npu_tap_owner = NULL`. Dans `dai_common_params()` (playback) : si `npu_tap_owner == NULL`, prend ownership ; sinon log warn et `dd->tap_buffer = NULL`. Dans `dai_common_reset()` : si `dd == npu_tap_owner`, libère l'ownership. Single-core SOF Zephyr (CONFIG_CORE_COUNT=1) → pas de spinlock requis. | claude-code, kimi, minimax | Prévient collision si Phase 2 ajoute USB UAC2 playback simultané. V4.2 nominal transparent. |

### Micro-amendements M1-M6 (intégrés inline V3.2.2 final)

Issus de l'investigation V3.2.2 `f6e0efae` (GO 6/6 unanime, claude-code dit explicitement « pas de V3.3 »). Découverte clé : `XCHAL_DCACHE_LINESIZE = 128` sur HiFi4 i.MX8MP (pas 64).

| # | Amendement | Source |
|---|---|---|
| **M1** | `struct npu_tap_hdr __attribute__((aligned(128)))` — alignement cache line HiFi4 | claude-code, glm-5.1 |
| **M2** | `NPU_TAP_HDR_SIZE = 128` (pad header to full cache line, isolation contre false sharing) ; `NPU_TAP_DATA_SIZE_MAX = 262016` | glm-5.1 |
| **M3** | `static_assert(sizeof(npu_tap_hdr) == 128)` + `static_assert(... <= XCHAL_DCACHE_LINESIZE)` | kimi |
| **M4** | `NPU_TAP_DATA_SIZE_MAX` côté UAPI marquée `/* DEPRECATED — use hdr->ring_size at runtime */` | glm-5.1 |
| **M5** | Magic word handshake boot : `hdr->magic = 0` AVANT init, `hdr->magic = NPU_TAP_MAGIC` en DERNIER. A53 vérifie magic avant lecture, retry si invalide. | claude-code |
| **M6** | Documenter limitation Phase 2 dans header UAPI (mono-DAI, refactor per-DAI requis si UAC2 + SAI7 simultanés) | qwen, glm-5.1, minimax |

Toutes les améliorations V3 → V3.2.1 (A1-A7, R1, R2, R3, R4, R5) sont conservées intactes.

## Contexte technique enrichi (découverte deepseek V3.2.1)

`XCHAL_DCACHE_LINESIZE = 128` sur HiFi4 i.MX8MP, **pas 64**. Donc :
- Struct `npu_tap_hdr` à 64 B = **demi cache line** (pas une cache line entière)
- L'atomicité du writeback **ne suffit PAS** à publier atomiquement la struct
- → Le pattern R3 (memw + double writeback explicite) est **OBLIGATOIRE**, pas redondant
- → V3.2.2 préserve R3 sans modification

## Solution V3.2.2

### Architecture (inchangée V3.2.1)

- Adresse `0x942B0000` (256 KB carve dans `dsp_reserved_heap`, hors SOF `MEMORY{}`)
- Hook `dai_dma_cb()` après `dma_buffer_copy_to` succès branch (A3)
- DSP cacheattr region 4 = write-through (digit 4 = 1)
- A53 atomic_load_acquire + seqcount-style read (R4)

### Modifications par fichier

#### 1. Firmware SOF — `sof/src/include/sof/audio/npu_tap.h` (V3.2.2 + M1-M6)

```c
#ifndef __SOF_AUDIO_NPU_TAP_H__
#define __SOF_AUDIO_NPU_TAP_H__

#include <stdint.h>
#include <stddef.h>
#include <xtensa/config/core-isa.h>   /* XCHAL_DCACHE_LINESIZE = 128 sur HiFi4 */

#define NPU_TAP_MAGIC          0x5441504Eu   /* "NPAT" */
#define NPU_TAP_RING_SIZE      0x40000u      /* 256 KB total */
#define NPU_TAP_HDR_SIZE       128u          /* M2: full cache line (HiFi4 = 128B) */
#define NPU_TAP_DATA_SIZE_MAX  (NPU_TAP_RING_SIZE - NPU_TAP_HDR_SIZE)  /* = 262016 */
#define NPU_TAP_PHYS_ADDR      0x942B0000u

/*
 * R6 + M2: hdr_size = 128B (full HiFi4 cache line for isolation) ; data zone
 * commence à NPU_TAP_PHYS_ADDR + 128 et fait 262016 B max.
 * L'effective ring size est calculée runtime selon period_bytes et stockée
 * dans hdr->ring_size. A53 utilise hdr->ring_size, JAMAIS la macro.
 * Pour V4.2 (period_bytes=3072) : ring_size = (262016/3072)*3072 = 261120 B
 * (85 périodes). Reste 896 B inutilisés.
 *
 * M6 — LIMITATION Phase 2 :
 * Cette implémentation V3.2.2 utilise une sentinelle mono-DAI (R7). UN SEUL
 * DAI playback peut être tapé à la fois (le 1er à appeler dai_common_params
 * prend l'ownership ; les suivants ont leur tap_buffer = NULL). Si Phase 2
 * ajoute USB UAC2 playback simultané à SAI7 TX, il faudra :
 *   Option A — accepter qu'UN SEUL des 2 soit tapé (sentinelle actuelle) ;
 *   Option B — refactor avec per-DAI buffer (pool DT 4 zones, alloc dynamique).
 */

struct npu_tap_hdr {
    uint32_t magic;          /* @0  : NPU_TAP_MAGIC, écrit en DERNIER (M5) */
    uint32_t version;        /* @4  : 4 (V3.2.2) */
    uint32_t ring_size;      /* @8  : R6 — runtime-aligned (multiple of period_bytes) */
    uint32_t hdr_size;       /* @12 : NPU_TAP_HDR_SIZE = 128 */
    uint32_t epoch;          /* @16 : R1 — published LAST par DSP avant magic, monotonic */
    uint32_t write_idx;      /* @20 : DSP wrap mod hdr->ring_size */
    uint32_t read_idx;       /* @24 : A53 wrap mod hdr->ring_size */
    uint32_t period_bytes;   /* @28 : 3072 nominal */
    uint32_t sample_rate;    /* @32 : 48000 */
    uint32_t channels;       /* @36 : 8 */
    uint32_t frame_fmt;      /* @40 : SOF_IPC_FRAME_S32_LE */
    uint32_t reserved[19];   /* @44..@124 : zero-init, padding to 128B (M2) */
} __attribute__((packed, aligned(128)));    /* M1: HiFi4 cache line alignment */

/* M3 : compile-time assertions */
_Static_assert(sizeof(struct npu_tap_hdr) == 128,
               "npu_tap_hdr must be exactly 128B (HiFi4 cache line)");
_Static_assert(sizeof(struct npu_tap_hdr) <= XCHAL_DCACHE_LINESIZE,
               "npu_tap_hdr must fit in one HiFi4 cache line");
_Static_assert(NPU_TAP_PHYS_ADDR == 0x942B0000u, "PHYS_ADDR fixed");
_Static_assert(offsetof(struct npu_tap_hdr, epoch) == 16, "epoch @16");
_Static_assert(offsetof(struct npu_tap_hdr, write_idx) == 20, "write_idx @20");

/*
 * R7 sentinel ownership: mono-DAI owner.
 * NULL = available; non-NULL = owned by that dai_data instance.
 * Single-core (CONFIG_CORE_COUNT=1) → no atomic/spinlock required for SOF
 * (4/6 workers V3.2.2 ont confirmé : IPC thread non-préempté par lui-même
 * + IRQ callbacks séparés sur même core Xtensa).
 */
extern struct dai_data *npu_tap_owner;

#endif
```

#### 2. Header kernel UAPI — `meta-local/recipes-kernel/imx-audio-tap/imx-audio-tap-uapi.h` (V3.2.2 + M1-M6)

```c
/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef __IMX_AUDIO_TAP_UAPI_H__
#define __IMX_AUDIO_TAP_UAPI_H__

#include <stdint.h>
#include <stddef.h>

#define NPU_TAP_MAGIC          0x5441504EU
#define NPU_TAP_RING_SIZE      0x40000U
#define NPU_TAP_HDR_SIZE       128U                /* M2 : HiFi4 cache line full */
#define NPU_TAP_DATA_SIZE_MAX  (NPU_TAP_RING_SIZE - NPU_TAP_HDR_SIZE)  /* M4 : DEPRECATED */
#define NPU_TAP_PHYS_ADDR      0x942B0000U

/*
 * M4 : NPU_TAP_DATA_SIZE_MAX est DEPRECATED — A53 doit utiliser hdr->ring_size
 * lue runtime (R6 alignement period_bytes). Ne JAMAIS utiliser cette macro pour
 * le ring wrap. Conservée uniquement pour validation taille mmap initiale.
 *
 * M6 — LIMITATION Phase 2 (mono-DAI) :
 * V3.2.2 firmware = sentinelle mono-DAI. Si Phase 2 ajoute USB UAC2 playback
 * en parallèle à SAI7 TX, soit :
 *   Option A — accepter UN SEUL tap actif (le premier DAI playback, généralement
 *              SAI7 TX) ; les autres DAI auront un tap_buffer NULL ;
 *   Option B — refactor firmware/UAPI avec per-DAI buffer (4 zones DT séparées).
 * À documenter et planifier en Phase 2.
 */

struct npu_tap_hdr {
    uint32_t magic;            /* @0   : DSP écrit en DERNIER au boot (M5) */
    uint32_t version;          /* @4   : 4 (V3.2.2) */
    uint32_t ring_size;        /* @8   : runtime-aligned, A53 uses THIS */
    uint32_t hdr_size;         /* @12  : 128 */
    uint32_t epoch;            /* @16  : DSP increments at each params, monotonic */
    uint32_t write_idx;        /* @20  : DSP wrap mod ring_size */
    uint32_t read_idx;         /* @24  : A53 wrap mod ring_size */
    uint32_t period_bytes;     /* @28 */
    uint32_t sample_rate;      /* @32 */
    uint32_t channels;         /* @36 */
    uint32_t frame_fmt;        /* @40 */
    uint32_t reserved[19];     /* @44..@124 : zero-init, padding to 128B (M2) */
} __attribute__((packed, aligned(128)));    /* M1 : full cache line */

_Static_assert(sizeof(struct npu_tap_hdr) == 128, "must be exactly 128B (HiFi4 cache line)");
_Static_assert(NPU_TAP_PHYS_ADDR == 0x942B0000U, "PHYS_ADDR fixed");
_Static_assert(offsetof(struct npu_tap_hdr, magic) == 0, "magic must be @0");
_Static_assert(offsetof(struct npu_tap_hdr, epoch) == 16, "epoch @16");

#endif
```

#### 3. Firmware SOF — `sof/src/audio/dai-legacy.c`

##### **Variable globale R7** (en haut du fichier)

```c
#include <sof/audio/npu_tap.h>

/* R7: mono-DAI ownership sentinel (single-core, no atomic needed) */
struct dai_data *npu_tap_owner = NULL;
```

##### **Init au `dai_common_params()`** (R3 ordering + R6 ring align + R7 sentinel)

Insertion entre l.591 et l.593 :

```c
/* V3.2.2 NPU tap: init / reset header */
if (dev->direction == SOF_IPC_STREAM_PLAYBACK) {
    /* R7: mono-DAI sentinel — first PLAYBACK DAI gets ownership */
    if (npu_tap_owner == NULL) {
        npu_tap_owner = dd;
    } else if (npu_tap_owner != dd) {
        /* Another DAI already owns the tap — disable for THIS DAI */
        comp_warn(dev, "NPU tap already owned, this DAI disabled");
        dd->tap_buffer = NULL;
        dd->tap_buffer_size = 0;
        dd->tap_period_bytes = 0;
        goto skip_tap_init;
    }
    /* npu_tap_owner == dd : (re-)init OK */

    dd->tap_buffer = (void *)(uintptr_t)NPU_TAP_PHYS_ADDR;
    dd->tap_buffer_size = NPU_TAP_RING_SIZE;
    dd->tap_period_bytes = period_bytes;

    struct npu_tap_hdr *hdr = (struct npu_tap_hdr *)dd->tap_buffer;

    /* R6: align ring_size to a multiple of period_bytes */
    uint32_t aligned_ring_size = (NPU_TAP_DATA_SIZE_MAX / period_bytes) * period_bytes;

    /* M5 step 1 : invalidate magic FIRST so A53 sees stale-state during init */
    hdr->magic = 0u;
    dcache_writeback_region(&hdr->magic, sizeof(hdr->magic));
    __asm__ volatile ("memw" ::: "memory");

    /* Step 2: zero-init the rest of the header */
    memset(((uint8_t *)hdr) + sizeof(uint32_t), 0,
           sizeof(*hdr) - sizeof(uint32_t));

    /* Step 3: data state (R3 canonical: data fields BEFORE epoch BEFORE magic) */
    hdr->version = 4;                        /* V3.2.2 */
    hdr->ring_size = aligned_ring_size;      /* R6: runtime-aligned */
    hdr->hdr_size = NPU_TAP_HDR_SIZE;        /* 128 (M2) */
    hdr->write_idx = 0;
    hdr->read_idx = 0;
    hdr->period_bytes = period_bytes;
    hdr->sample_rate = 48000;
    hdr->channels = 8;
    hdr->frame_fmt = SOF_IPC_FRAME_S32_LE;

    /* Step 4: flush data state (everything BEFORE epoch) */
    dcache_writeback_region(hdr, offsetof(struct npu_tap_hdr, epoch));

    /* Step 5: memw barrier */
    __asm__ volatile ("memw" ::: "memory");

    /* Step 6: publish epoch (R1 monotonic + R3 ordering) */
    hdr->epoch = ++dd->tap_epoch;
    dcache_writeback_region(&hdr->epoch, sizeof(hdr->epoch));
    __asm__ volatile ("memw" ::: "memory");

    /* M5 step 7 : publish magic LAST — A53 only trusts header when magic == NPAT */
    hdr->magic = NPU_TAP_MAGIC;
    dcache_writeback_region(&hdr->magic, sizeof(hdr->magic));
    __asm__ volatile ("memw" ::: "memory");

skip_tap_init:
    ;  /* fallthrough to return */
}
```

##### **Hook au `dai_dma_cb()`** (utilise `hdr->ring_size` runtime, R6)

```c
} else { /* dma_buffer_copy_to OK — A3 */
    if (dd->tap_buffer && bytes > 0) {  /* tap_buffer NULL = R7 disabled or reset */
        struct npu_tap_hdr *hdr = (struct npu_tap_hdr *)dd->tap_buffer;
        uint32_t ring_size = hdr->ring_size;            /* R6: runtime value */

        if (bytes <= ring_size) {                       /* A6 guard, now uses runtime ring_size */
            uint8_t *data_base = (uint8_t *)dd->tap_buffer + NPU_TAP_HDR_SIZE;
            uint32_t w = hdr->write_idx;

            /* A4: back-walk to source start */
            struct audio_stream *stream = &dd->dma_buffer->stream;
            void *src = audio_stream_rewind_wptr_by_bytes(stream, bytes);
            uint32_t src_to_end = audio_stream_bytes_without_wrap(stream, src);
            uint32_t src_head = MIN(bytes, src_to_end);
            uint32_t src_tail = bytes - src_head;

            /* Tap ring write with wrap × 2 */
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

            /* A5: flush DATA before write_idx */
            uint32_t total_in_tap = MIN(bytes, ring_size - w);
            dcache_writeback_region(data_base + w, total_in_tap);
            if (bytes > ring_size - w) {
                dcache_writeback_region(data_base, bytes - (ring_size - w));
            }

            __asm__ volatile ("memw" ::: "memory");

            hdr->write_idx = (w + bytes) % ring_size;
            dcache_writeback_region(&hdr->write_idx, sizeof(hdr->write_idx));
            __asm__ volatile ("memw" ::: "memory");
        }
    }

    dd->total_data_processed += bytes;
}
```

##### **Cleanup au `dai_common_reset()`** (A1 + R7 release)

Insertion AVANT `dai_dma_release` à l.722 :

```c
/* A1: NULL tap_buffer FIRST */
dd->tap_buffer = NULL;
dd->tap_buffer_size = 0;
dd->tap_period_bytes = 0;
/* dd->tap_epoch reste — strictly monotonic for life of struct */

/* R7: release ownership if we're the owner */
if (npu_tap_owner == dd) {
    npu_tap_owner = NULL;
}

if (!dd->delayed_dma_stop)
    dai_dma_release(dd, dev);
/* ... reste inchangé ... */
```

#### 4. Device Tree (DTS, additif — inchangé V3.2.1)

```dts
&{/reserved-memory/dsp_reserved_heap@93400000} {
    reg = <0x0 0x93400000 0x0 0xeb0000>;
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

#### 5. Kernel module — `meta-local/recipes-kernel/imx-audio-tap/imx-audio-tap.c`

(inchangé V3.2.1, A7 DT runtime check préservé)

#### 6. Yocto recipe `do_configure` build-time check (R5, inchangé V3.2.1)

```python
do_configure_prepend() {
    UAPI_HDR="${WORKDIR}/files/imx-audio-tap-uapi.h"
    SOF_HDR="${WORKDIR}/../../sof/src/include/sof/audio/npu_tap.h"
    [ -f "$SOF_HDR" ] || bbfatal "SOF firmware header not found: $SOF_HDR"
    UAPI_ADDR=$(grep -E '^\s*#define\s+NPU_TAP_PHYS_ADDR' "$UAPI_HDR" | awk '{print $3}')
    SOF_ADDR=$(grep -E '^\s*#define\s+NPU_TAP_PHYS_ADDR' "$SOF_HDR" | awk '{print $3}' | sed 's/u$//')
    [ "$UAPI_ADDR" = "$SOF_ADDR" ] || bbfatal "NPU_TAP_PHYS_ADDR mismatch: UAPI=$UAPI_ADDR vs SOF=$SOF_ADDR"
}
```

#### 7. App userspace — `meta-local/audio-tools/npu_tap_reader.c` (R4 seqcount + R6 runtime ring_size)

```c
#include "imx-audio-tap-uapi.h"
#include <stdatomic.h>
#include <string.h>

/* R4: seqcount-style read pattern + R6: runtime ring_size */

static uint32_t last_seen_epoch = 0;
static uint32_t local_read_idx  = 0;

while (running) {
    uint32_t e1, e2, w, magic;
    uint32_t ring_size = atomic_load_explicit(&hdr->ring_size, memory_order_acquire);  /* R6 */
    uint8_t *local_buf = malloc(ring_size);
    if (!local_buf) break;

    /* M5 step 0 : verify magic before trusting header */
    magic = atomic_load_explicit(&hdr->magic, memory_order_acquire);
    if (magic != NPU_TAP_MAGIC) {
        fprintf(stderr, "NPU tap: invalid magic 0x%x (DSP not ready)\n", magic);
        free(local_buf);
        usleep(10000);   /* 10 ms backoff */
        continue;
    }

    /* R4 step 1: read epoch FIRST (acquire) */
    e1 = atomic_load_explicit(&hdr->epoch, memory_order_acquire);

    if (e1 != last_seen_epoch) {
        last_seen_epoch = e1;
        local_read_idx = atomic_load_explicit(&hdr->write_idx, memory_order_acquire);
        fprintf(stderr, "NPU tap: DSP reset detected, epoch -> %u\n", e1);
        free(local_buf); continue;
    }

    /* R4 step 2: read write_idx */
    w = atomic_load_explicit(&hdr->write_idx, memory_order_acquire);
    if (w == local_read_idx) { usleep(2000); free(local_buf); continue; }

    /* R4 step 3: read data with ring wrap (using runtime ring_size R6) */
    uint32_t bytes_avail = (w - local_read_idx + ring_size) % ring_size;
    uint32_t to_end = ring_size - local_read_idx;
    if (bytes_avail <= to_end) {
        memcpy(local_buf, data_base + local_read_idx, bytes_avail);
    } else {
        memcpy(local_buf, data_base + local_read_idx, to_end);
        memcpy(local_buf + to_end, data_base, bytes_avail - to_end);
    }

    /* R4 step 4: re-read epoch — if changed, discard */
    e2 = atomic_load_explicit(&hdr->epoch, memory_order_acquire);
    if (e2 != e1) {
        last_seen_epoch = e2;
        local_read_idx = atomic_load_explicit(&hdr->write_idx, memory_order_acquire);
        fprintf(stderr, "NPU tap: race detected (e1=%u e2=%u), discarding\n", e1, e2);
        free(local_buf); continue;
    }

    /* R4 step 5: data committed, push to NPU */
    push_to_npu(local_buf, bytes_avail);
    local_read_idx = w;
    free(local_buf);
}
```

### Plan en 5 jours (V3.2.2)

| Jour | Travail | Gate |
|---|---|---|
| **J0** | Revert modifs Alt-A locales `sof/`. Régression V4.2 PASS sur firmware vanilla. Commit V3.2.2 doc + PROJECT_STATE.md. | `git diff sof/` propre + 4/4 V4.2 PASS |
| **J1** | Firmware : `npu_tap.h` (R6 macro DATA_SIZE_MAX) + struct field + init R3+R6+R7 + hook + cleanup A1+R7 release + build + sign + deploy | Build OK + V4.2 régression PASS + magic+epoch+ring_size lisibles via `devmem 0x942B0000` |
| **J2** | Header UAPI + Kernel module (A7 + R6 docs) + DTS + recipe Yocto avec do_configure diff (R5) | `modprobe` OK + `/dev/imx-audio-tap` + sysfs + DT check + diff check passent |
| **J3** | App userspace `npu_tap_reader` (R4 seqcount + R6 runtime ring_size) + dump wav | RMS > -100 dB + epoch incremental + ring_size = 261120 (85×3072) |
| **J4** | Test latence + 0-packet-loss 10 min + stress 60 min + test rate change | <50 ms latence + 0 underrun + 4/4 V4.2 PASS + epoch reset détecté |
| **J5** | Intégration NPU pipeline réelle | Stream NPU stable |

### Tests V3.2.2 supplémentaires

- **R6 ring_size** : `devmem 0x942B0008` (offset 8 = ring_size) → doit lire 0x3FC00 (= 261120) en V4.2
- **R7 sentinel** : démarrer 1 DAI playback, vérifier `npu_tap_owner != NULL` ; tenter de démarrer 2e DAI playback (si dispo), vérifier que son `dd->tap_buffer == NULL` et warn dans logs
- **R7 release** : arrêter le DAI owner, vérifier `npu_tap_owner = NULL`, démarrer un autre DAI playback, vérifier qu'il prend l'ownership

## Référence

- Investigation V1 : `54dc95c6-256e-42d1-beb9-1a73d7d6a0c5`
- Investigation V2 : `01688a32-341a-4979-85fe-f5ab27d09d7f`
- Investigation V2 (re) : `64a4b28e-f6ab-4b02-9cfa-552b0c145b04` (NO-GO)
- Investigation V3 : `23dde533-0dba-4bdc-9b33-f5ecf8a7b3f3` (GO 5/6)
- Investigation V3.1 : `115112b0-41b5-431b-8f3e-03fece0c3ff9` (GO 6/6)
- Investigation V3.2 : `03b6477a-d0e4-4b03-8a8c-080006aede81` (GO 6/6 + ordering)
- Investigation V3.2.1 : `6f4cfb00-56e8-4390-b25b-931ace1277f1` (GO 5/6 + multi-DAI)
- Investigation V3.2.2 : à lancer
- Décisions : `3565abbf` V1→V2, `809eae7d` V2→V3, `18e10a2c` V3→V3.1, `002f2f3c` V3.1→V3.2, `d6064e52` V3.2→V3.2.1, V3.2.1→V3.2.2 à archiver
- Doc maître : `PROJECT_STATE.md`
