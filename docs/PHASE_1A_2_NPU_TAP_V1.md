# Phase 1a.2 — NPU Tap V1 specification

**Status** : proposition validée par investigation 6 workers (job `54dc95c6-256e-42d1-beb9-1a73d7d6a0c5`).
**Date** : 2026-04-26
**Branche** : `feature/audio-platform-v2`

## Objectif

Capturer les blocs audio post-effets OUT (post-MBDRC/PGA/DRC, juste avant le DMA SAI7 TX, format 8ch S32_LE @ 48 kHz, débit 1.5 MB/s) depuis la pipeline V4.2 du firmware SOF, et les exposer à un programme userspace A53 qui les pousse au NPU pour analyse ML temps réel.

**Contrainte projet** : tap NPU non-négociable. Cf. `CLAUDE.md` et la mémoire projet `project_npu_non_negotiable.md`.

## Historique des voies tentées (à NE PAS reproduire)

| Voie | Statut | Raison de l'échec |
|------|--------|-------------------|
| V1.0 → V1.5 (ALSA capture PCM via BE DAI dummy / MUXDEMUX cross-pipeline) | **Rejeté** | Patch kernel ALSA core requis (refusé), IPC3 trigger -22 EINVAL, DPCM `dpcm_end_walk_at_be` rejette `out_drv` |
| **S1 SOF Probes upstream** (kernel additif `imx-probes.c` + activation `CONFIG_PROBE`) | **Bloqué silent** | Modèle gateway HDA assumed by upstream, SDMA i.MX `sdma_copy()` ligne 588 drop silencieusement les transferts AP2AP avec `buf_xaddr=0` |
| **Alt-A** (S1 + IPC vendor `SOF_IPC_PROBE_HOST_BUFFER_SET`) | **Bloqué silent** | Filtre ABI amont rejette le nouveau cmd (-22 invisible), 4 itérations rebuild firmware avec codes erreur différenciés ne sortent jamais → couche de validation IPC pre-dispatcher non-traçable. etrace mailbox ferme sur ce build (Invalid argument) |
| S6 mem_sink/tap_sink composant SOF custom | Tenté précédemment, abandonné | Détails à reconfirmer dans git log |
| SDMA scatter-gather multi-destination | Impossible HW | Aucun script ROM SDMA i.MX8MP avec multi-dest, BD mono-source/mono-dest |
| Dual-mapping OCRAM/DRAM coherent | Impossible HW | DSP Xtensa pas de MMU configurable runtime |
| SAI hardware loopback | Impossible HW | RM AUDIOMIX confirme aucun mux interne TX→RX data path |
| DMA Trace SOF détourné | Bande passante insuffisante | Ring trace ~16-32 KB << 1.5 MB/s soutenu |

**Documenté en détail** dans la mémoire projet `phase_1a2_history.md`.

## Solution V1 — Hook `dai_dma_cb()` dans `dai-legacy.c`

### Principe

Au moment où le firmware DSP copie le buffer audio post-DRC vers le buffer DMA destiné au DAI SAI7 TX (callback `dai_dma_cb()` de `dai-legacy.c` après chaque période DMA), faire un **memcpy supplémentaire** vers une zone DRAM partagée DSP↔A53 (reserved-memory dédiée dans le DT). Le programme userspace A53 lit cette zone via mmap d'un module kernel out-of-tree.

### Pourquoi V1 marche là où S1/Alt-A ont échoué

- **Pas d'IPC custom** → pas de filtre ABI silencieux qui rejette
- **Pas de SDMA AP2AP avec dest=0** → pas de guard `sdma.c:588`
- **Pas de modèle gateway HDA assumed** → on contrôle physiquement où copier
- **Latence : 5 µs sur budget 2 ms = 0.25 %** (HiFi4 @ 600 MHz, memcpy ~768 cycles pour 384 B 8ch s32le)
- **Standard SOF** : utilise `buffer_alloc(SOF_MEM_ZONE_RUNTIME_SHARED)` natif, pas de fork ABI

### Architecture cible

```
Pipeline V4.2 PIPE 6 playback (inchangé) :
   PCM_host
      │
      ▼
   MBDRC ─► PGA ─► DRC ─► [tap V1] ─► SAI7 TX ─► HP physiques
                              │
                              └─► memcpy_s vers npu_tap_buffer (reserved-memory)
                                     │
                                     ▼
                                Userspace A53 mmap /dev/imx-audio-tap
                                     │
                                     ▼
                                NPU
```

### Modifications par fichier

**Firmware SOF (`sof/`, modifs additives)** :

| Fichier | Modif | Lignes |
|---------|-------|--------|
| `sof/src/audio/dai-legacy.h` | +2 champs (`tap_buffer`, `tap_buffer_size`) dans `struct dai_data` | 2 |
| `sof/src/audio/dai-legacy.c` | Allocation `buffer_alloc(is_shared=true)` au `dai_common_params()` (playback only). Hook `memcpy_s` au `dai_dma_cb()` après le `dma_buffer_copy_to()` existant. Free au `dai_common_reset()` | ~50 |
| `sof/src/include/sof/audio/npu_tap.h` (NOUVEAU) | Header ring-buffer struct `npu_tap_hdr` + macro `NPU_TAP_MAGIC = 0x5441504E ("NPAT")`, `NPU_TAP_RING_SIZE = 256 KB` | ~30 |
| `sof/app/boards/imx8mp_evk_mimx8ml8_adsp.conf` | **Retirer** `CONFIG_PROBE=y` (Alt-A obsolète). Aucune option à ajouter pour V1 (utilise primitives standard) | -3 |

**Device Tree (DTS, additif)** :

| Fichier | Modif |
|---------|-------|
| Patch DT additif (pattern existant `vdev0buffer` Cortex-M7) : nouveau node `reserved-memory/npu_tap_buffer@0x9XXXXXXX` (256 KB no-map dans la fenêtre SDRAM DSP 0x92400000-0x933FFFFF) + node `imx_audio_tap` avec `compatible = "electrosens,imx-audio-tap"` et `memory-region = <&npu_tap_buffer>` |

**Kernel module out-of-tree (`meta-local/recipes-kernel/imx-audio-tap/`)** :

| Fichier | Modif |
|---------|-------|
| `imx-audio-tap.c` (NOUVEAU, ~80 LOC) | `miscdevice` exposant `/dev/imx-audio-tap`. Open + mmap (write-combine via `pgprot_writecombine`) sur la zone reserved-memory. sysfs `magic`, `period_bytes`, `sample_rate`, `channels` |
| `imx-audio-tap_0.1.bb` (NOUVEAU) | Recipe Yocto kernel module out-of-tree |
| `linux-imx_%.bbappend` | Aucun changement nécessaire (le module est livré séparément, chargé par `systemd-modules-load.d`) |

**App userspace (`meta-local/audio-tools/`)** :

| Fichier | Modif |
|---------|-------|
| `npu_tap_reader.c` (NOUVEAU, ~100 LOC) | Open `/dev/imx-audio-tap` → mmap → valide magic → poll `hdr->write_idx` → lit blocs → push NPU (stub printf initial) |

### Chemins de risque + mitigation

| Risque | Mitigation |
|--------|------------|
| Cache coherency DSP↔A53 (DSP write pas vu par A53) | `dcache_writeback_region()` côté DSP après chaque memcpy + mapping write-combine côté A53 (`pgprot_writecombine`) |
| Le DSP n'a pas accès à l'adresse choisie (hors fenêtre DSP MMU/MPU SDMA) | Plan B : utiliser `buffer_alloc(SOF_MEM_CAPS_RAM, ..., is_shared=true)` natif SOF. Le buffer est alloué automatiquement dans SDRAM1 (0x92C00000+) que le DSP voit déjà. Adresse exposée via sysfs depuis le module kernel après lecture du IPC d'init du DAI |
| Drop sous-charge si A53 trop lent | Ring 256 KB = ~170 ms tampon. NPU latency target ~50 ms → marge confortable |
| Régression V4.2 audio (memcpy 5 µs / 2 ms = 0.25 %) | Test V4.2 T1-T6 obligatoire après chaque deploy, Gate explicite |

### Plan en 5 jours

| Jour | Travail | Gate |
|------|---------|------|
| J1 | Firmware : hook `dai_dma_cb()` + `buffer_alloc` + build + sign + deploy | Build OK + V4.2 regression PASS |
| J2 | Kernel module `imx-audio-tap.ko` + DTS reserved-memory + bbappend | `modprobe` OK + `/dev/imx-audio-tap` présent + sysfs lisible |
| J3 | App userspace `npu_tap_reader` + dump wav | Wav non-vide + RMS > -100 dB pendant `aplay siren.wav` |
| J4 | Test latence + 0-packet-loss 10 min | <50 ms latence + 0 underrun ring + 4/4 V4.2 PASS |
| J5 | Intégration NPU pipeline réelle | Stream NPU stable, mesure performance ML |

### Ce que V1 NE FAIT PAS (sécurité)

- Aucune modif `sof/src/drivers/imx/sai.c` (intouchable, 7 jours validation utilisateur)
- Aucune modif kernel ALSA core
- Aucune modif topology m4 V4.2
- Aucune modif IPC SOF (pas de vendor cmd risquant filtre amont silencieux)
- Aucune dépendance à un mécanisme upstream non-supporté sur SDMA (pas de probes gateway model)

### Plan B (si V1 échoue inopinément)

**Cortex-M7 bridge via RPMsg** : pattern déjà utilisé sur ce projet. Le M7 (inutilisé) snoope la zone shared mem et transmet via RPMsg vers A53. Découple totalement le tap du chemin audio critique. Effort 2-3 semaines mais robuste.

## Validation à venir

Avant code, lancer une **investigation finale critique** sur ce plan détaillé pour validation collective. Si GO, passer à l'implémentation J1.

## Référence

- Investigation 6 workers : job `54dc95c6-256e-42d1-beb9-1a73d7d6a0c5` (verdict unanime V1)
- Mémoire projet : `phase_1a2_history.md`, `npu_tap_v1_proposal.md`
- Pattern reserved-memory existant : `vdev0buffer@94300000` (M7 RPMsg) dans `imx8mp-evk.dts`
