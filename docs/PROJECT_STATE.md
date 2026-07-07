# PROJECT_STATE — Plateforme Mastering Live Debix

**Document maître unique. Source de vérité du projet.**
Dernière mise à jour : 2026-05-13
Branche active : **`feature/v7.0-multiband-drc-tap`** (basée sur `L6.12.3-debix_model_ab`)

Branche origine `feature/audio-platform-v2` archivée. La V7.0 a fork à
partir du commit `0580b5f14` (E6.a multi-codec matrix 16×8 son OK) et
poursuit en parallèle.

**HEAD courant** : `6511f273 meta-local: sof-firmware-custom recipe`
(yocto-nxp-debix) + `4bea8e59d` (sof). Voir `REPRODUCIBILITY.md` pour
la procédure complète de rebuild from scratch.

> **À LIRE EN PREMIER** par toute personne (humain ou IA) qui reprend le projet.
> Ce document remplace toute lecture ad hoc des docs `PHASE_*.pdf` éparpillés.

---

## 1. Vision du projet

Plateforme de **mastering audio live « lite » embarquée**, pilotée par IA, sur board **Debix Model AB** (NXP i.MX8MP).

**But fonctionnel** :
- Console de mixage 8 entrées / 8 sorties XLR + 8 mics PDM sélectionnables.
- Bus master stéréo avec multiband DRC, exciter, harmonizer, limiter.
- **Mastering automatique en boucle fermée par NPU** (le NPU lit l'audio post-effets, infère, écrit les paramètres en temps réel ~10 Hz).
- USB composite : 8×8 audio + MIDI + réseau (PC) + 2×2 audio + MIDI (téléphone).
- 4 effets send (reverb / chorus / flanger / delay) en LV2 sur Cortex-A53.
- UI web (Flutter) pour contrôle, presets, visualisation.

**Contrainte non-négociable** : le **tap audio post-effets vers NPU** est l'objectif central du projet. Toute architecture qui ne permet pas ce tap est rejetée par construction (cf. mémoire `project_npu_non_negotiable.md`).

## 2. Architecture matérielle

```
┌───────────────────────── i.MX 8M Plus SoC ───────────────────────────┐
│                                                                       │
│  4× Cortex-A53 (1.6 GHz)        HiFi4 DSP (800 MHz)     Cortex-M7    │
│  Linux 6.6.36 + Yocto 5.0       SOF Zephyr (IPC3)       INUTILISÉ    │
│                                                                       │
│  Vivante GPU                    NPU 2.3 TOPS (eIQ TFLite)            │
│                                                                       │
│  AudioMix (SAI7 TDM-8 / EASRC)   SDMA (mem→mem AP2AP)                │
└───────────────────────────────────────────────────────────────────────┘
       │                                          │
       │ I²C (0x50-0x53)                         │ SAI7 TDM 8ch×32b @48kHz ASYNC
       ▼                                          ▼
   ┌─────────────────────────────────────────────────────────────┐
   │  4× TAC5212 daisy-chain (TI codec)                          │
   │  - 2 ADC + 2 DAC + 2 PDM mics chacun                        │
   │  - ADC : MUX XLR/PDM, PGA, 3 biquads, HPF, gate, DRE        │
   │  - DAC : 3 biquads, DVC, HP/Line drive                      │
   │  - 8 XLR IN, 8 XLR OUT, 8 micros PDM (sélectionnables)      │
   └─────────────────────────────────────────────────────────────┘
```

Détails clé :
- **SAI7** : TDM 8 slots × 32-bit, dsp_a, **TX master / RX slave SYNC=0** (ASYNC).
  - Pourquoi ASYNC : le mode SYNC souffre d'un timing mismatch qui produit -55 dB de bruit. ASYNC + continuous BCLK = -121 dB de bruit (mémoire `sof_async_solution.md`).
- **REGCACHE_NONE** sur TAC5212 : écriture I²C directe, pas de cache regmap.
- **DSP Xtensa HiFi4** : pas de MMU runtime. Cacheattr fixe au boot, SDRAM 0x80000000-0x9FFFFFFF en write-through.

## 3. Architecture logicielle (mémoire DSP)

```
DSP HiFi4 vu de l'A53 :
  DRAM0  0x3B6E8000  (8 KB OCRAM, DSP-only HEAP_HP_RX)
  DRAM1  0x3B6F0000  (8 KB OCRAM, DSP-only HEAP_HP_TX)
  SDRAM0 0x92400000  (8 MB, DSP↔A53 partagée — code firmware + heaps)
  SDRAM1 0x92C00000  (8 MB, DSP↔A53 partagée — heap buffer + mailbox)
  Fenêtre DSP totale : 0x92400000 - 0x933FFFFF (16 MB)

Au-dessus de la fenêtre DSP, mais accessibles A53 :
  dsp_reserved_heap@93400000  (~15 MB carve-out Linux, no-map)
  vdev0buffer@94300000        (1 MB Cortex-M7 RPMsg) ❌ HORS fenêtre DSP
  vdev0vring0/1@942F0000      (M7 RPMsg control)
```

Découverte critique (mémoire `phase_1a2_history.md` discovery #1) :
> `vdev0buffer@94300000` n'est PAS accessible par le DSP. Toute zone shared DSP↔A53 doit être DANS 0x92400000-0x933FFFFF (ou créer une nouvelle reserved-memory dédiée dans cette fenêtre).

## 4. Roadmap macro (état réel)

| Phase | Description | Statut |
|---|---|---|
| **0** | TAC5212 driver + SAI7 TDM + ASYNC + clocks continues | ✅ FAIT |
| **1** | Full-duplex 8ch ALSA + REGCACHE_NONE + tac-reset | ✅ FAIT |
| **1a** | Topology SOF V4.2 console 8ch end-to-end (MBDRC + PGA + DRC) | ✅ FAIT (production figée) |
| **1a.1 MVP** | Pipeline V4.2 deployée + 4/4 régression PASS | ✅ FAIT (commit `9532d649`) |
| **1a.2** | **Tap audio post-effets → NPU** | ✅ **FAIT** — V3.2.2 hook `dai_dma_cb` + reserved-memory @0x942B0000 + module kernel + userspace, validé hardware J4 stress 10 min (0 race, 0 underrun, 60 epoch transitions, 1.54 MB/s steady) |
| **2** | USB composite gadget UAC2 8×8 + 2×2 + MIDI + RNDIS | ⏳ À venir |
| **3** | FX send 4 bus LV2 sur A53 (JACK + reverb/chorus/flanger/delay) | ⏳ À venir |
| **4** | NPU mastering boucle fermée (TFLite + service inference) | ⏳ À venir |
| **5** | Harmonizer + exciter + presets + Flutter UI avancé | ⏳ À venir |
| **6** | Image Yocto finale + tests stress 24h + mesures THD/SNR | ⏳ À venir |

## 5. Ce qui MARCHE en production (Phase 0 → 1a)

### 5.1 Audio path validé bout-en-bout

```
8× XLR IN ──► 4× TAC5212 (PGA, biquads, gate)
            ──► SAI7 RX TDM 8ch × 32bit @48kHz ASYNC
            ──► HiFi4 DSP (SOF Zephyr IPC3)
            ──► Topology V4.2 : MBDRC → PGA → DRC
            ──► SAI7 TX TDM 8ch
            ──► 4× TAC5212 DAC ──► 8× XLR OUT
```

### 5.2 Composants validés (commits référents)

| Composant | Commit | Mémoire |
|---|---|---|
| TAC5212 ASoC codec driver (TDM, dsp_a, REGCACHE_NONE) | `e93d1f6a`, `41cae6a4` | `tac5212_async_tx_config.md` |
| Patch fsl_sai (async RX consumer + capture TX config) | série mars 2026 | `feedback_diagnostic_rules.md` |
| SOF ASYNC + continuous BCLK (-55 dB → -121 dB noise) | `25d11fea` | `sof_async_solution.md` |
| TAC5212 auto-init au boot via service systemd | `124dfe38` | `tac_reset_required_after_boot.md` |
| Volume duplex fix (suppression mute_stream) | `12121454` | — |
| Topology V4.2 MBDRC+PGA+DRC console 8ch | `9532d649` | `sof_topology_build.md` |
| 8ch full-duplex C loopback + VU meter | `c3cf2701` | `sof_imx_full_duplex_userspace.md` |
| Build SOF firmware + sign + deploy procedure | — | `sof_firmware_build.md`, `sof_firmware_deploy_path.md` |

### 5.3 Tests de régression V4.2 (gate obligatoire)

T1-T6 V4.2 : `aplay siren.wav`, `arecord`, full-duplex C, latence <10 ms, RMS > -100 dB, 0 underrun 10 min. **Doit passer 4/4 après chaque modif firmware** (mémoire `feedback_no_panic_revert.md`).

## 6. Tout ce qui a été TENTÉ et N'A PAS MARCHÉ

> ⚠ **À lire avant toute nouvelle proposition** : ne pas reproduire ces voies.

### 6.1 Phase 1a.2 — Tap NPU : 4 voies bloquées

#### 6.1.1 V1.0 → V1.5 — ALSA capture PCM via BE DAI dummy / MUXDEMUX

**Tenté** : ajouter un 3e PCM ALSA `HP_Monitor` capture via topology m4 (MUXDEMUX cross-pipeline, piggyback scheduling, `VIRTUAL_WIDGET out_drv`, `PCM_DUPLEX_ADD`, BE DAI dummy snd-soc-dummy, patch DT multi-link).

**Bloqué par (purement technique, JAMAIS un refus utilisateur)** :
- IPC3 trigger -22 EINVAL (asymétrie `pipeline_comp_prepare` direction-filter vs `pipeline_comp_trigger` sched-filter)
- Kernel ASoC DPCM `dpcm_end_walk_at_be` rejette les widgets non `dai_in/dai_out/aif_in/aif_out`
- `simple-audio-card` ne supporte pas DPCM dummy BE (snd-soc-dummy n'a pas de binding DT)
- Cumul de blockers (DPCM walk + IPC3 asymétrie + dummy BE binding) → la voie ne menait nulle part même avec un patch kernel

**Verdict** : NE FONCTIONNAIT PAS. La voie ALSA capture PCM est intrinsèquement incompatible avec la pipeline V4.2 sans une refonte ASoC trop importante pour le ROI.
**Docs obsolètes** : `PHASE_1A_2_SPEC_V1.pdf` (b73f0f07), `V1.1` (2c806f07), `V1.2` (71028634), `V1.5_glm` (8fd0bf72), `V1.5` (e6744fa8).

#### 6.1.2 S1 — SOF Probes upstream

**Tenté** : utiliser le mécanisme upstream SOF Probes (`/dev/snd/comprC?D?` + debugfs `probe_points`).

**Côté kernel — DÉPLOYÉ ET FONCTIONNEL** (commits `005047ee` + `c3cf2701`) :
- `meta-local/recipes-kernel/linux/files/imx-probes.c` : nouveau client kernel `snd_sof_imx_probes` (~95 lignes)
- Patch additif kernel (entrée `snd_sof.imx-probes` dans `sof_probes_client_id_table`)
- Activation `CONFIG_SND_SOC_SOF_DEBUG_PROBES=y`
- État board : card 4 sofprobes + /dev/snd/comprC4D0 + debugfs probe_points présents, V4.2 PCM intacte

**Côté firmware — BLOQUÉ silent failure** :
- Cause racine identifiée : `sdma.c:588` guard `if (!buf_addr || !buf_xaddr) return 0;` drop silencieusement les transferts AP2AP quand l'adresse host est nulle.
- Le sous-système probes upstream est conçu pour **gateway hardware HDA Intel / ACP AMD** où le hardware résout l'adresse host implicitement. Sur SDMA i.MX, l'adresse host doit être explicite — non fournie par le mécanisme upstream.

**Verdict** : Côté kernel = OK et infra utilisable (gardée pour compat upstream future, n'empêche rien). Côté firmware = bloqué au niveau ABI sans IPC vendor pour transmettre l'adresse host.

#### 6.1.3 Alt-A — Patch firmware additif IPC vendor `SOF_IPC_PROBE_HOST_BUFFER_SET`

**Tenté** (modifs LOCALES non poussées dans `sof/`) :
- Ajout `SOF_IPC_PROBE_HOST_BUFFER_SET = SOF_CMD_TYPE(0x009)` dans `sof/src/include/ipc/header.h`
- Ajout `struct sof_ipc_probe_host_buffer_params` dans `sof/src/include/ipc3/probe.h`
- Ajout `probe_set_host_buffer()` dans `sof/src/probe/probe.c` (~60 LOC)
- Ajout thunk `ipc_probe_host_buffer_set()` + case dans `ipc_glb_probe()` de `sof/src/ipc/ipc3/handler.c`
- Modif `imx-probes.c` kernel pour envoyer le nouvel IPC

**Bloqué par** : retour `-EINVAL` (-22) silencieux du firmware au IPC HOST_BUFFER_SET. **4 itérations de rebuild** firmware avec codes erreur différenciés (-110 à -202, return 0 hardcoded `case 0x00090000:`) → aucun de mes nouveaux returns n'est jamais visible côté kernel. Le -22 vient d'une couche **AVANT le dispatcher principal** (probable check ABI amont qui rejette tout cmd inconnu).

**Diagnostic impossible** : `/sys/kernel/debug/sof/etrace` retourne « Invalid argument », `fw_state` vide, `inbox/outbox` muets, `sof-logger` ne lit pas les mailbox sur ce build.

**Verdict** : Voie BLOQUÉE par silent failure non-diagnostiquable. **Modifs locales `sof/` à revert avant V2** (cf. section 8 plan d'action).

#### 6.1.4 S6 — Composant SOF custom `mem_sink`/`tap_sink`

**Status** : tenté précédemment selon utilisateur, échec. Détails à reconfirmer dans git log si re-considéré. Workers V1 investigation considèrent que la voie a été explorée et écartée.

### 6.2 Voies hardware-impossibles (confirmées par 6 workers)

- ❌ **SDMA scatter-gather multi-destination** : SDMA i.MX8MP n'a aucun script ROM avec multi-dest, BD mono-source/mono-dest fixe. Patch ROM impossible sans firmware SDMA custom.
- ❌ **Dual-mapping OCRAM/DRAM coherent** : DSP Xtensa i.MX8MP **n'a pas de MMU configurable runtime** (table fixed ROM). DRAM0/1 sont des TCM DSP-only.
- ❌ **SAI hardware loopback via AUDIOMIX** : RM AUDIOMIX confirme **aucun mux interne TX→RX data path**. AUDIOMIX BLK_CTL route les clocks (mclk, fsync) mais pas le data audio.
- ❌ **DMA Trace SOF détourné** : bande passante du ring trace insuffisante pour 1.5 MB/s soutenu (~16-32 KB ring).
- ❌ **Patch kernel `sof-client-probes-ipc3.c`** : ne résout pas, le bug est dans le firmware.
- ❌ **`vdev0buffer@94300000`** : hors fenêtre DSP, inaccessible côté DSP.

## 7. Phase 1a.2 V3.2.2 — DÉPLOYÉ ET VALIDÉ HARDWARE

### 7.1 Évolution spec V1 → V3.2.2 (8 investigations cumulées GO 6/6)

V1 (proposition initiale) → V2 (6 corrections) → V2 NO-GO (overlap HEAP_BUFFER) → V3 (adresse 0x942B0000) → V3.1 (A1-A7) → V3.2 (R1 epoch + R2 macro) → V3.2.1 (R3 ordering + R4 seqcount + R5 build diff) → V3.2.2 (R6 ring align runtime + R7 sentinelle mono-DAI + M1-M6 inline).

8 investigations critic.io toutes archivées : `54dc95c6`, `01688a32`, `64a4b28e`, `23dde533`, `115112b0`, `03b6477a`, `6f4cfb00`, `f6e0efae`. Verdict final V3.2.2 : **GO 6/6 unanime**.

### 7.2 Architecture validée

- **Adresse** : `0x942B0000` (256 KB no-map carve dans `dsp_reserved_heap`, hors SOF `MEMORY{}`)
- **Hook firmware** : `dai_dma_cb()` après `dma_buffer_copy_to()` succès branch (else)
- **DSP write** : `memcpy_s` + `memw` + `dcache_writeback_region` (R3 canonical smp_store_release)
- **Header** : 128 B aligned (`__aligned(128)`, M1+M2 HiFi4 cache line full)
- **Ring runtime** : `hdr->ring_size = (DATA_SIZE_MAX / period_bytes) * period_bytes` = 261120 (R6)
- **Magic handshake** : `magic=0` first, all data fields, `epoch++`, `magic=NPAT` last (M5 boot safety)
- **Sentinelle** : `npu_tap_owner` global, mono-DAI, R7 (single-core, no atomic)
- **A53 read** : mmap `pgprot_writecombine` + `atomic_load_acquire` seqcount-style (R4)
- **DT runtime check** : A7 `if (rmem->base != NPU_TAP_PHYS_ADDR) return -EINVAL`
- **Build-time check** : Yocto `do_configure_prepend` diff R5 (bbfatal si UAPI ≠ SOF header)

### 7.3 Plan exécuté (5 jours → 1 jour réel)

| Jour | Travail | Status | Commit |
|---|---|---|---|
| **J0** | Revert Alt-A (stash) + build vanilla + V4.2 régression | ✅ PASS 5/5 | sof stash@{0} |
| **J1** | Firmware : `npu_tap.h` + struct field + init + hook + cleanup | ✅ Build OK + V4.2 PASS | sof `61fcb2e7d`, `9f6f70a21` |
| **J2** | Kernel module + DT carve + recipe Yocto + image install | ✅ modprobe OK + sysfs | yocto `b083331d`, `b71e9f33` |
| **J3** | Userspace `npu_tap_reader.c` (R4 seqcount + WAV dump) | ✅ siren capturé RMS -5.60 dBFS | yocto `8054f6fd` |
| **J4** | Stress sustain 10 min | ✅ 60 epoch transitions, 0 race, 0 underrun, 1.54 MB/s steady | (validation hardware) |
| **J5** | Push NPU TFLite inference | ⏳ À venir |

### 7.4 Validation hardware finale (192.168.0.9)

- Kernel 6.6.36 + DTB V3.2.2 + firmware V3.2.2.1 + module imx-audio-tap.ko
- `/dev/imx-audio-tap` 10:122 (miscdevice mmap)
- sysfs : `phys_addr=0x00000000942b0000`, `size=262144`
- Header lu post-aplay : magic=NPAT, version=4, ring_size=261120, period=3072, rate=48000, ch=8
- **Stress 10 min** : 0 race retries, 60 epoch transitions monotones, 1.54 MB/s steady, 464 MB consommés, 0 dmesg error
- **V4.2 régression** : T1 play exit 0, T2 cap exit 0, T3 duplex play=0 rec=0, RMS -101 dB, 0 errors
- **WAV dump siren** : 7.38 MB/5.04s, RMS -5.60 dBFS = vrais samples post-DRC capturés

## 8. Découvertes critiques (à NE JAMAIS oublier)

> Ces faits sont coûteux à redécouvrir. Mémoire projet permanente.

1. **Fenêtre DSP HiFi4 = 0x92400000-0x933FFFFF (16 MB)**.
   `memory.h:29-33` : SDRAM0 = 0x92400000 + 8 MB ; SDRAM1 = 0x92C00000 + 8 MB. Tout au-delà n'est pas dans le linker SOF ni la heap firmware.

2. **`vdev0buffer@94300000` HORS fenêtre DSP** (utilisable seulement A53↔M7).

3. **DSP cacheattr SDRAM = write-through** (`cacheattr 0x22212222`, digit 4 = 1 = WT pour 0x80000000-0x9FFFFFFF). Donc cohérence DSP→A53 gratuite, `dcache_writeback_region()` est no-op effectif.

4. **`dd->dma_buffer` est en OCRAM DSP-only** (`HEAP_HP_TX_BASE = 0x3B6F0000`, 8 KB), pas en SDRAM. A53 ne peut pas mmap. Memcpy explicit obligatoire.

5. **`is_shared=true` sur i.MX8MP single-core est un no-op pour le payload audio**. Sur `CONFIG_CORE_COUNT=1`, `SOF_MEM_ZONE_RUNTIME_SHARED` retombe sur `rmalloc_runtime` (`alloc.c:729`). Le payload audio va dans la heap selon `caps`.

6. **SOF Probes upstream assume gateway hardware HDA/ACP**. Sur SDMA i.MX, ce modèle ne fonctionne pas par construction. Le guard `sdma.c:588 if (!buf_addr || !buf_xaddr) return 0;` drop silencieusement.

7. **`etrace` mailbox SOF muette sur ce build** (`Invalid argument`). Tout débogage firmware doit utiliser un canal alternatif (codes erreur différenciés via `reply.error`, ou redécouvrir option Kconfig manquante).

8. **SAI7 doit être en mode ASYNC (SYNC=0)** pour 8ch full-duplex. Mode SYNC = -55 dB de bruit (timing mismatch). ASYNC + continuous BCLK = -121 dB.

9. **TAC5212 nécessite `tac-reset` après chaque boot** sinon samples = strictement 0 (capture muette).

10. **Full-duplex C userspace** : `snd_pcm_link(cap, play)` + `snd_pcm_start(cap)` AVANT pre-fill/start playback, sinon capture pipeline silencieuse.

11. **SDMA AP2AP nécessite `EVTOVR=1` avant HSTART** sinon le canal ne démarre jamais (mémoire `sdma_ap2ap_evtovr.md`).

12. **Build firmware SOF** : après `west sign`, le `.ri` produit contient déjà le manifest. **Ne PAS faire `cat zephyr.ri.xman zephyr.ri`** (double manifest = IPC 108/20 error). Cf. `sof_firmware_build.md`.

13. **Deploy firmware** : kernel charge `/lib/firmware/imx/sof/sof-imx8m.ri` (sans 'p'). Vérifier hash dans dmesg entre builds.

14. **Tests sur board** : stockés dans `/root/tests/`, jamais `/tmp` (qui est tmpfs et perdu au reboot).

## 9. Phases à venir

### 9.1 Phase 2 — USB composite gadgets
- USB1 (PC) : UAC2 8×8 + MIDI + RNDIS via ConfigFS
- USB2 (téléphone) : UAC2 2×2 + MIDI
- Service systemd auto-config au boot
- Bridges ALSA gadget ↔ DSP topology
- Routage via EASRC pour resync horloge USB↔48 kHz local
- Critère : PC voit carte son 8×8 + MIDI + réseau, téléphone 2×2 + MIDI

### 9.2 Phase 3 — FX send LV2 sur A53
- JACK2 + 4 plugins LV2 :
  - Reverb : calf, dragonfly ou lsp
  - Chorus / Flanger / Phaser : x42 ou calf
  - Delay : x42 ou similaire
- Bridges ALSA : DSP FX-send captures → JACK inputs / JACK outputs → DSP FX-return playbacks
- Topology étendue avec PCMs `FX1_Send`..`FX4_Send` + `FX1_Ret`..`FX4_Ret`
- Trade-off : +10-20 ms latence sur les sends, acceptable

### 9.3 Phase 4 — NPU mastering boucle fermée
- Modèle TFLite trained from scratch (pas de pré-existant)
- Input features : FFT master bus 2048 pts fenêtre 20 ms → loudness/bande, balance, crest, corrélation stéréo
- Output : gains EQ 8-10 bandes + seuils multiband DRC + amount exciter
- Service Python/C++ : lit NPU tap, infère, écrit ALSA controls via `sof-ctl`
- Update rate ~10 Hz
- **Pré-requis** : Phase 1a.2 V2 GO (NPU tap fonctionnel)

### 9.4 Phase 5 — Harmonizer + presets + Flutter UI
- Harmonizer : `rubberband` LV2 en A53 (chain master A53 après DSP)
- Exciter : plugin LV2 ou module SOF custom selon mesures latence
- Flutter UI complète : table de mixage, FX returns, master bus, presets
- WebSocket pour VU-mètres temps réel
- Sauvegarde/restauration presets

### 9.5 Phase 6 — Production
- Build image Yocto finale
- Tests stress 24h : 8ch full-duplex + USB + web
- Mesures audio : latence bout-en-bout, THD+N, SNR, bruit de fond
- Documentation utilisateur
- Packaging + livraison

## 10. Fichiers clés du projet

| Fichier | Rôle | État |
|---|---|---|
| `meta-local/recipes-kernel/linux/files/tac5212.c` | Driver ASoC TAC5212 | ✅ Production |
| `meta-local/recipes-kernel/linux/files/tac-daisy.sh` | Init TAC5212 daisy-chain | ✅ Production |
| `meta-local/recipes-kernel/linux/files/tac-reset.service` | Reset PLL au boot | ✅ Production |
| `meta-local/recipes-kernel/linux/files/sof-imx8mp-tac5212-drc.m4` | Topology V4.2 MBDRC+PGA+DRC | ✅ Production |
| `meta-local/recipes-kernel/linux/files/sof-imx8mp-tac5212-eq.m4` | Variante EQ-only (à valider rôle) | À documenter |
| `meta-local/recipes-kernel/linux/files/imx-probes.c` | Client kernel SOF Probes (S1) | ⚪ Présent mais inutilisé V2 |
| `meta-local/recipes-kernel/linux/files/test-sof-dsp.sh` | Tests régression V4.2 | ✅ Utilisé |
| `meta-local/recipes-kernel/linux/files/gpio-monitor.c` | (à documenter) | ? |
| `sof/` (fork Mjxkill/sof) | Firmware SOF Zephyr | ⚠ Modifs Alt-A locales à revert |
| `zephyr/` | Submodule Zephyr | ✅ |
| `modules/` | Submodules SOF (rimage, etc.) | ✅ |
| `Model_AB_Infinity/` | Build directory Yocto | ✅ |
| `imx-audio-tap` (kernel module) | Tap NPU userspace | ⏳ À créer Phase 1a.2 V2 J2 |
| `npu_tap_reader.c` (audio-tools) | App userspace NPU | ⏳ À créer Phase 1a.2 V2 J3 |

## 11. Documents de référence

### 11.1 Documents historiques (conservés mais OBSOLÈTES)

| Doc | Statut |
|---|---|
| `PHASE_1A_SPEC.pdf` → `V4.pdf` | Itérations production console 8ch — toutes archivées |
| `PHASE_1A_SPEC_V4.1.pdf`, `V4.2.pdf` | V4.2 = production en cours |
| `PHASE_1A_2_SPEC_V1.pdf` → `V1.5.pdf` (5 fichiers) | OBSOLÈTES (voie ALSA bloquée techniquement) |
| `PHASE_1A_2_NPU_TAP_V1.md` | OBSOLÈTE (V1 non-validé après investigation 01688a32) |
| `AUDIO_PLATFORM_PLAN.md` | Plan global 2026-03-31, valide pour Phases 2-7 |
| `MASTERING_PLATFORM_PLAN.md` | Plan global 2026-04-21, plus récent — valide pour Phases 1a.2-5 |
| `SOF_ASYNC_AUDIO_GUIDE.md` | Documentation technique SOF ASYNC, valide |
| `TAC5212_*.md` | Docs intégration TAC5212, valides |

### 11.2 Documents à venir

- `PHASE_1A_2_NPU_TAP_V2.md` (à créer après validation user) : spec V2 corrigée
- Doc utilisateur final (Phase 6)

### 11.3 Mémoires projet (`~/.claude/projects/-home-michael-yocto-nxp-debix/memory/`)

Index complet dans `MEMORY.md`. Mémoires critiques pour Phase 1a.2 :
- `phase_1a2_history.md` — toutes les voies tentées
- `npu_tap_v1_proposal.md` — proposition V1 (à mettre à jour vers V2)
- `project_npu_non_negotiable.md` — contrainte projet
- `sof_imx_probes_pattern.md` — pattern S1 kernel

### 11.4 Références matérielles

- `IMX8MPRM.pdf` — Reference Manual i.MX8MP
- `tac5212.pdf` — Datasheet TAC5212
- `sbaa383c.pdf` — TI Application Note daisy-chain
- `DSP_ARCHITECTURES.pdf` — Comparaison architectures DSP (Archi C retenue)

## 12. Protocole de travail

Conformément à `CLAUDE.md` et `REGLES.md` :

1. **Aucune modification source SOF sans `critic_analyze` préalable** (3 itérations max).
2. **Aucun code avant validation utilisateur** sur l'analyse.
3. **Une étape = un commit** validé avant la suivante.
4. **Backups systématiques** avant tout changement risqué.
5. **Régression V4.2 obligatoire** après chaque modif firmware.
6. **Doc PDF mise à jour** avant code (mémoire `feedback_docs_always_updated.md`).
7. **Aucune modif `sof/tools/topology/topology1/sof/*.m4` upstream** → pipelines custom dans `meta-local/`.
8. **Tests sur board dans `/root/tests/`**, jamais `/tmp`.
9. **Patch kernel OK pour nouvelle fonctionnalité isolée**, refusé pour changer le comportement ALSA/driver existant.
10. **Tap NPU non-négociable** : ne JAMAIS abandonner cette contrainte projet.

## 13. État actuel précis (2026-05-13)

**Branch yocto-nxp-debix** : `feature/v7.0-multiband-drc-tap` (HEAD = `6511f273`)
**Branch sof** : `feature/v7.0-multiband-drc-tap` (HEAD = `4bea8e59d`)
**Modifs locales non poussées** : aucune (working tree clean).

### V7.0 roadmap — état réel

| Étape | Statut | Commit | Fiche |
|---|---|---|---|
| E0 — baseline + audit kernel | ✓ GO | — | `TESTS_V7.0_E0.md` |
| E1 — topology simplifiée + ALSA low-lat | ✓ GO | — | `TESTS_V7.0_E1.md` |
| E2 — multiband_drc CAP 8ch | ✓ GO | — | `TESTS_V7.0_E2.md` |
| E3 — strips OUT play 8ch | ✓ GO | — | `TESTS_V7.0_E3.md` |
| E4 — NPU tap IN brut (PIPE 1) | ✓ GO | — | `TESTS_V7.0_E4.md` |
| E5 — NPU tap OUT post-FX (PIPE 2) | ✓ GO | — | `TESTS_V7.0_E5.md` |
| E6.a — USB UAC2 gadget 8×8 | ✓ GO | — | `TESTS_V7.0_E6a.md` |
| E6.b — Phone aloop 2×2 | ✓ GO | — | `TESTS_V7.0_E6b.md` |
| E6.c — Routing ALSA paire-à-paire | ✓ GO | — | `TESTS_V7.0_E6c.md` |
| E6.d — mixer-pro daemon 26 in × 18 out | ✓ MVP | — | `TESTS_V7.0_E6d.md` |
| E6.e — 4 effets natifs C (compr/reverb/delay/EQ) | ✓ MVP | — | `TESTS_V7.0_E6e.md` |
| E6.f — profiling + diagnostic hotspot | ✓ Diag | — | `TESTS_V7.0_E6f.md` |
| E6.g — refactor 2-thread + ring SPSC | ✓ GO 48 kHz | — | `TESTS_V7.0_E6g.md` |
| E6.h — eventfd + ring 8 périodes (latence 14 ms) | ✓ GO **baseline V7.0** | — | `TESTS_V7.0_E6h.md` |
| E6.i / j / k / l — tentatives < 10 ms | ✗ KO empirique | — | `TESTS_V7.0_E6i/j/k/l.md` |
| E7 — GUI HTTP mixer-gui-http + Alpine.js | ✓ GO | `079d3ddb` | `TESTS_V7.0_E7.md` |
| E7.1 — peak meters + SSE 30 Hz | ✓ GO | `d055b946` | `TESTS_V7.0_E7.1.md` |
| E7.2 — UI premium Apple-class + SAI fix | ✓ GO | `e3f3d7f6` | `TESTS_V7.0_E7.2.md` |
| E7.3 — sidebar + FX bus panels complets | ✓ GO | `edf6dd50` + `0d0f844f` | `TESTS_V7.0_E7.3.md` |
| E7.4.b-n — TAC + DRC editor + crossover + layout | ✓ GO | `45e6592b` (bundle) | `TESTS_V7.0_E7.4.md` |
| E7.5 — 4 analyzer taps FFT + scope | ✓ GO | `4f4e4d0b` + `4124f97f` | `TESTS_V7.0_E7.5.md` |
| E7.6 — documentation consolidée | ✓ GO | `18aa700e` + `0d7372e8` | — |
| E7.7 — returns tags + R1..R8 fix + repro infra | ✓ GO | `0216bd71` + `87f71fd1` + `6511f273` | `TESTS_V7.0_E7.7.md` |

### Conclusion architecturale post-E6.l

> Plancher physique V7.0 = TAC ADC 0.5 + DMA RX 2 + cap pipe 0.5 + ALSA cap min 2 + mix 0.5 + ALSA play min 2 + DMA TX 2 + DAC 0.5 = **10 ms exact AVANT marge anti-xrun**.

E6.h (14 ms côté mixer, ~20 ms E2E) est la **limite pratique** sur SOF
i.MX 8M Plus en userspace. Cible < 10 ms reportée à une V8.0 mixer DSP
SOF natif (effort 4-8 semaines, gain structurel -8 à -14 ms).

### État de reproductibilité

Voir `REPRODUCIBILITY.md` (commit `6511f273`) :
- Toutes les recettes Yocto présentes (mixer-pro, mixer-gui-http,
  tac5212-service, sof-firmware-custom)
- Firmware SOF custom et topology vendorés dans
  `meta-local/recipes-bsp/sof-firmware-custom/files/`
- SOF source tree poussé sur `github.com/Mjxkill/sof`
- **Dette connue** : `kernel-module-imx-audio-tap` packaging deb à
  réparer pour qu'un `bitbake imx-image-full -c rootfs` passe sans
  workaround.

### Prochaine étape envisagée

- **E7.8 potentiel** : persistance d'état mixer (snapshot/restore au boot)
- **E7.9 potentiel** : presets nommés (Live / Studio / Conf)
- Réparer le packaging `kernel-module-imx-audio-tap` pour fermer la
  dette reproductibilité
- Sprint V8 mixer DSP natif si la cible < 10 ms redevient prioritaire

---

**Fin du document maître.** Toute info qui contredit ce document doit être réconciliée ici avant d'agir.
