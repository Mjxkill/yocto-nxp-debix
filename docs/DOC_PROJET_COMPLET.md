# Debix Model AB — Console Audio Professionnelle Embarquée
## Documentation Technique Complète V7.0

**Date** : 2026-05-21
**Branche** : `feature/v7.0-multiband-drc-tap`
**Plateforme** : NXP i.MX 8M Plus (Cortex-A53 quad-core + Cortex-M7 + HiFi4 DSP + NPU + Vivante GPU)
**Carte** : Debix Model AB (3 cartes I/O × 4 codecs TAC5212 = 8 mics + 8 speakers TDM)
**OS** : Yocto 5.0 Scarthgap, kernel linux-imx 6.6.36, SOF DSP firmware Zephyr v2.10

---

## Table des matières

1. [Vue d'ensemble du projet](#1-vue-densemble-du-projet)
2. [Plateforme matérielle](#2-plateforme-matérielle)
3. [Yocto — chaîne de build](#3-yocto--chaîne-de-build)
4. [Stack logicielle — vue système](#4-stack-logicielle--vue-système)
5. [Kernel Linux — drivers audio](#5-kernel-linux--drivers-audio)
6. [SOF DSP firmware](#6-sof-dsp-firmware)
7. [Configuration SAI continue + DMA RX/TX](#7-configuration-sai-continue--dma-rxtx)
8. [Codec TAC5212 — configuration et reset PLL](#8-codec-tac5212--configuration-et-reset-pll)
9. [NPU TAP — architecture dual-tap pour ingé son ML](#9-npu-tap--architecture-dual-tap-pour-ingé-son-ml)
10. [USB UAC2 gadget — interface DAW](#10-usb-uac2-gadget--interface-daw)
11. [Problème fondamental DSP ↔ USB — fréquences indépendantes](#11-problème-fondamental-dsp--usb--fréquences-indépendantes)
12. [mixer-pro — daemon C userspace 26×18 + ASRC](#12-mixer-pro--daemon-c-userspace-2618--asrc)
13. [Stabilité temps réel — isolcpus, CPUAffinity, scheduling](#13-stabilité-temps-réel--isolcpus-cpuaffinity-scheduling)
14. [Support LV2 — plugin audio (futur)](#14-support-lv2--plugin-audio-futur)
15. [mixer-gui-http — frontend web](#15-mixer-gui-http--frontend-web)
16. [Build, déploiement, debug](#16-build-déploiement-debug)
17. [Roadmap et problèmes connus](#17-roadmap-et-problèmes-connus)
18. [Annexes — extraits clés](#18-annexes--extraits-clés)

---

## 1. Vue d'ensemble du projet

### 1.1 Objectif

Construire une **console de mixage audio professionnelle embarquée** sur carte Debix Model AB (i.MX 8M Plus), capable de :

- Capturer **8 microphones simultanés** via 4 codecs TAC5212 en mode TDM 8 slots
- Effectuer du **traitement audio temps réel** (DSP-based) : compression multibande, DRC dynamique, égalisation, gain
- S'interfacer avec un **DAW (Digital Audio Workstation)** externe via **USB UAC2 gadget 8×8** (la carte est vue comme une carte son externe par le PC hôte)
- Gérer une **voix téléphonique 2×2** (modem voix / VoIP) en parallèle
- Exposer une **GUI web temps réel** (mixer, vu-mètres, FFT, scopes stéréo, réglage effets) accessible via navigateur sur réseau local
- Permettre **l'analyse par NPU** des signaux audio via deux taps (entrée brute pré-effets + sortie post-effets) pour un futur module d'**ingé son ML**

**Cible métier** : matériel audio professionnel temps réel, latence acoustique <10 ms end-to-end, qualité broadcast (0 glitch audible).

### 1.2 Pourquoi ce projet existe

Sur l'écosystème audio embarqué Linux, il n'existe pas de plateforme SBC commerciale qui combine :

1. Un **DSP HiFi4 programmable** côté firmware (SOF — Sound Open Firmware)
2. Un **NPU intégré au SoC** capable de calculs ML en temps réel sur l'audio
3. Suffisamment de **canaux audio simultanés** (8 in / 8 out + USB + tél)
4. Un **kernel Linux mainline** maintenu (lignée NXP linux-imx 6.6)

Le projet vise à combler ce gap en construisant une **Yocto BSP** complète + un **firmware DSP custom** + un **mixer userspace temps réel**, le tout open-source (sauf code spécifique métier).

### 1.3 Contraintes de design

| Contrainte | Source | Détail |
|---|---|---|
| Latence acoustique < 10 ms | Cahier des charges | Mic → DSP cap → mixer Linux → DSP play → speaker en moins de 10 ms |
| 0 glitch audible | Cahier des charges (cible pro) | Pas de click, pop, drop-out, distorsion |
| 8 mics + 8 speakers minimum | Cahier des charges | 4 TAC5212 codec stéréo × 2 ch = 8 ch |
| Interface DAW pro | Cahier des charges | USB UAC2 8×8 — la carte = sound card externe pour Reaper/Ardour/etc. |
| NPU tap toujours présent | Memory `[[project_npu_non_negotiable]]` | Tap signal audio vers DRAM partagée NPU, JAMAIS désactiver |
| Multi-band DRC indépendant par canal | Cahier des charges | 8 channels DRC indépendants côté cap (mic AGC) et côté play (limiter) |
| Reproductibilité Yocto | Standard industriel | Build complet `bitbake imx-image-full` génère .wic flashable |

### 1.4 Architecture en une image (ASCII)

```
┌─────────────────────────────────────────────────────────────────────────┐
│              i.MX 8M Plus SoC (Cortex-A53 quad + HiFi4 DSP + NPU)        │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                         │
│   ┌────────────────────┐                  ┌──────────────────────┐     │
│   │  HiFi4 DSP (SOF)   │                  │   Cortex-A53 (Linux) │     │
│   │  - 1 pipe cap 8ch  │   shared mem    │   - mixer-pro daemon │     │
│   │  - 1 pipe play 8ch │  ◄──IPC────►    │   - mixer-gui-http   │     │
│   │  - multiband_drc   │                  │   - ALSA, systemd    │     │
│   │  - drc D3, pga     │                  │   - f_uac2 gadget    │     │
│   │  - SAI7 DMA driver │                  │   - USB DWC3 driver  │     │
│   └─────────┬──────────┘                  └──────────┬───────────┘     │
│             │                                         │                 │
│   tap-in 0x94270000 (256K) ┐               ┌── tap-out 0x942B0000 (256K)│
│   ────────────────────────►│               │◄──────────────────────────│
│                            ▼               ▼                            │
│              ┌──────────────────────────────────┐                       │
│              │       NPU (Vivante GC8000UL)      │  futur ingé son ML  │
│              └──────────────────────────────────┘                       │
│                                                                         │
└────────┬──────────────────────────┬─────────────────────────┬───────────┘
         │                          │                         │
    SAI7 TX/RX 8 slots TDM     USB OTG (DWC3)              Ethernet
         │                          │                         │
         ▼                          ▼                         ▼
   ┌──────────┐                ┌─────────┐              ┌─────────┐
   │ 4× TAC5212│                │  PC Host │              │ Browser  │
   │  TDM bus  │                │   DAW    │              │  mixer   │
   │  8 mics   │                │ (Reaper, │              │ web GUI  │
   │ 8 speakers│                │ Ardour,  │              │ port 8080│
   └──────────┘                │ Bitwig…) │              └─────────┘
                                └─────────┘
```

### 1.5 Statut actuel (V8.33 baseline)

- **Captation** : 4 TAC5212 en TDM 8 slots S32_LE 48 kHz, mics fonctionnels
- **Restitution** : 8 voies speakers via mêmes TAC5212 DAC
- **DSP cap pipeline** : SAI7 RX → multiband_drc 8ch → pga → PCM 0 cap
- **DSP play pipeline** : PCM 1 play → multiband_drc 8ch → pga → SAI7 TX
- **NPU taps** : 2 zones DRAM partagées 256 KB chacune (`/dev/imx-audio-tap-in`, `/dev/imx-audio-tap-out`), driver kernel + UAPI header partagé avec SOF firmware
- **USB UAC2 8×8** : gadget actif, host enumère carte son comme "UAC2 PCM"
- **mixer-pro** : daemon C 2412 lignes, 26 inputs × 18 outputs avec 4 bus FX, ASRC drop/insert, RT prio 80, isolé cores 2-3
- **GUI web** : libmicrohttpd + Alpine.js + Tailwind, 4 analyzer FFT, scope stéréo, sliders mixer, paged-blob effets TAC5212 et DSP
- **Latence mesurée** : ~8-10 ms acoustique end-to-end MIC→HP avec USB DAW dans la chaîne
- **Glitchs résiduels** : présents, scalent avec la magnitude du drift USB↔DSP résiduel (voir §11)

---

## 2. Plateforme matérielle

### 2.1 SoC i.MX 8M Plus (NXP)

| Bloc | Détail |
|---|---|
| CPU | 4× ARM Cortex-A53 @ 1.8 GHz (cluster, cache L1 32K I+D, L2 512K partagé) |
| Coprocesseur RT | 1× ARM Cortex-M7 @ 800 MHz (FreeRTOS / Zephyr) |
| **DSP audio** | **Cadence HiFi4 @ 800 MHz** (Tensilica Xtensa, VLIW, 1×SIMD 64-bit), cache D 32K, cache I 32K |
| GPU | Vivante GC7000UL (Vulkan, OpenGL ES 3.x) |
| **NPU** | **Vivante GC8000UL @ 1.0 GHz**, 2.3 TOPS INT8 (cible audio ML) |
| ISP | Image Signal Processor 12 MP camera |
| DRAM | LPDDR4 jusqu'à 6 GB |
| eMMC | jusqu'à 32 GB (Debix Model AB : SD card seulement) |
| Audio | 7× SAI (Synchronous Audio Interface), 1× S/PDIF, 1× DMIC, 4× ASRC HW |
| USB | 2× USB 3.0 OTG (DWC3 IP) |
| Ethernet | 2× Gigabit MAC |
| PCIe | 1× PCIe Gen3 1-lane |
| Sécurité | CAAM (crypto offload), ARM TrustZone |

**Documentation référence** : NXP IMX8MPRM (i.MX 8M Plus Applications Processor Reference Manual), >5000 pages. Acroname Debix Model AB Schematic Rev 1.0.3.

### 2.2 Debix Model AB — Carte produit

| Connecteur | Fonction |
|---|---|
| 1× HDMI 4K | Display Wayland/Weston |
| 1× MIPI-DSI | Display port (alternative) |
| 2× USB 3.0 | Type-A host |
| 1× USB 2.0 Type-C | OTG (USB UAC2 gadget) |
| 1× microSD | Boot rootfs FAT + ext4 |
| 2× Gigabit Ethernet | RJ45 |
| Audio | 4× TAC5212 en TDM (8 mics analog + 8 speakers) |
| GPIO header | 40 pin (compatible RPi) |
| M2 Key E | Wi-Fi/BT |
| M2 Key M | NVMe |

### 2.3 Topologie audio matérielle

```
                                  ┌────────────────────────────────┐
                                  │       i.MX 8M Plus SoC          │
                                  │                                  │
                                  │   SAI7 master  (BCLK/FSYNC out)  │
                                  │       │                          │
                                  │       │ TX out / RX in           │
                                  │       │                          │
                                  └───────┼──────────────────────────┘
                                          │
       PCB Daisy chain TDM ─────────┬─────┴────┬──────────────┬────────────────┐
                                    │          │              │                │
                                    ▼          ▼              ▼                ▼
                              ┌────────┐  ┌────────┐    ┌────────┐      ┌────────┐
                              │TAC0    │  │TAC1    │    │TAC2    │      │TAC3    │
                              │0x50 I2C│  │0x51 I2C│    │0x52 I2C│      │0x53 I2C│
                              │slot 0,1│  │slot 2,3│    │slot 4,5│      │slot 6,7│
                              └─┬────┬─┘  └─┬────┬─┘    └─┬────┬─┘      └─┬────┬─┘
                                │    │      │    │        │    │          │    │
                              MIC0 MIC1   MIC2 MIC3     MIC4 MIC5       MIC6 MIC7
                              SPK0 SPK1   SPK2 SPK3     SPK4 SPK5       SPK6 SPK7
```

- **SAI7** = master, génère BCLK (3.072 MHz pour 48 kHz × 16 slots × 32 bits / 2 sides) et FSYNC (48 kHz)
- **TAC5212** = slaves I2C (bus 3, addr 0x50..0x53), TDM in/out sur DOUT shared (4 TACs partagent un wire DOUT, chacun ne pilote que ses 2 slots assignés)
- **TDM** = 8 slots × 32 bits S32_LE @ 48 kHz, frame size = 256 bits = 3.072 MHz BCLK
- **Format** : DSP_A (frame high, frame starts at last bit of previous data)

---

## 3. Yocto — chaîne de build

### 3.1 Layers structure

Le projet utilise Yocto **5.0 Scarthgap** (LTS), forké du `Linux 6.12.3-NXP` release. Les layers sont assemblés via `Model_AB_Infinity/conf/bblayers.conf` :

```bash
# sources/ — ~40 layers upstream (read-only, do not edit)
sources/poky                              # Yocto core (oe-core)
sources/meta-openembedded/{oe,multimedia,python,networking,filesystems}
sources/meta-freescale                    # NXP/Freescale BSP
sources/meta-freescale-3rdparty
sources/meta-freescale-distro
sources/meta-imx/{meta-imx-bsp,meta-imx-sdk,meta-imx-ml,meta-imx-v2x}
sources/meta-nxp-demo-experience          # NXP demos
sources/meta-nxp-matter-baseline
sources/meta-nxp-openthread
sources/meta-arm{,/meta-arm-toolchain}
sources/meta-clang                        # Clang/LLVM
sources/meta-gnome                        # Optional GNOME stack
sources/meta-qt6                          # Qt6 framework
sources/meta-parsec                       # Parsec security
sources/meta-tpm                          # TPM 2.0
sources/meta-virtualization               # Xen/Jailhouse
sources/meta-musicians  (submodule git)   # OE musicians ecosystem (Ardour, LV2…)

# meta-local — tout le code custom du projet (BBFILE_PRIORITY = 1)
meta-local/
├── recipes-audio/                # mixer-pro, mixer-gui-http
├── recipes-bsp/                  # boot-script-rt, snd-aloop-phone, usb-uac2-gadget, sof-firmware-custom
├── recipes-core/images/          # imx-image-full bbappend
├── recipes-devtools/             # flutter, dart
├── recipes-fsl/images/           # imx-image-full bbappend (NXP variant)
├── recipes-graphics/flutter/     # Flutter embedder + runner
├── recipes-kernel/
│   ├── imx-audio-tap/            # Kernel module out-of-tree
│   └── linux/                    # linux-imx bbappend (patches + cfg fragments)
└── recipes-musicians/            # Ardour 6.9 + deps
```

**Priorité** : `meta-local` a `BBFILE_PRIORITY_meta-local = 1` (vs 5 pour meta-imx) — `1` est LE PLUS BAS donc s'applique en DERNIER → override gagne.

### 3.2 Commandes build essentielles

```bash
# 1. Initialiser l'environnement (1× par session shell)
cd /home/michael/yocto-nxp-debix
EULA=1 DISTRO=fsl-imx-xwayland MACHINE=imx8mpevk \
    source imx-setup-release.sh -b Model_AB_Infinity

# 2. Build image complète
bitbake imx-image-full
# Génère : Model_AB_Infinity/tmp/deploy/images/imx8mpevk/
#   ├── imx-image-full-imx8mpevk.rootfs-*.wic    (13 GB, flash sur SD)
#   ├── Image                                     (kernel 6.6.36)
#   ├── imx8mp-evk.dtb                            (device tree)
#   ├── boot.scr                                  (U-Boot bootargs RT)
#   └── deb/imx8mpevk/*.deb                       (paquets séparés)

# 3. Rebuild d'un recipe (ex après edit mixer-pro.c)
bitbake -c cleansstate mixer-pro       # force rebuild (PAS cleanall !)
bitbake mixer-pro
# Sortie deb : tmp/deploy/deb/armv8a/mixer-pro_1.0-r0_arm64.deb

# 4. Devshell interactif pour debug recipe
bitbake -c devshell linux-imx
# → shell dans le sysroot kernel, make olddefconfig, etc.

# 5. SDK pour cross-compile host
bitbake imx-image-full -c populate_sdk
# Génère un installer .sh ~3 GB qui crée /opt/Model_AB_Infinity/...
```

**ATTENTION** : `bitbake -c cleanall` est **interdit** sur ce projet (memory `[[feedback_cleanall]]`). Il supprime les stamps de download et casse les fetch de branches custom (linux-imx, SOF custom). Utiliser `cleansstate` exclusivement.

### 3.3 Recipes meta-local clés

#### 3.3.1 `meta-local/recipes-audio/mixer-pro/mixer-pro_1.0.bb`

```bitbake
SUMMARY = "mixer-pro — daemon C console DAW SW (V7.0-E6.d)"
LICENSE = "GPL-2.0-or-later"
SRC_URI = " \
    file://mixer-pro.c \
    file://mixer-pro.h \
    file://analyzer.c file://analyzer.h \
    file://effects.c file://effects.h \
    file://Makefile \
    file://mixer-pro.service \
"
S = "${WORKDIR}"
DEPENDS = "libmicrohttpd alsa-lib"
inherit systemd
SYSTEMD_SERVICE:${PN} = "mixer-pro.service"
```

#### 3.3.2 `meta-local/recipes-kernel/imx-audio-tap/imx-audio-tap_0.1.bb`

Module out-of-tree NPU tap. Cross-check au build entre UAPI userspace et headers SOF firmware :

```bitbake
do_configure:prepend() {
    UAPI_HDR="${WORKDIR}/imx-audio-tap-uapi.h"
    SOF_HDR="${TOPDIR}/../sof/src/include/sof/audio/npu_tap.h"
    for SYM in NPU_TAP_IN_PHYS_ADDR NPU_TAP_OUT_PHYS_ADDR; do
        UAPI_VAL=$(grep -E "^[[:space:]]*#define[[:space:]]+$SYM" "$UAPI_HDR" | awk '{print $3}' | tr -d 'Uu')
        SOF_VAL=$(grep -E "^[[:space:]]*#define[[:space:]]+$SYM" "$SOF_HDR" | awk '{print $3}' | tr -d 'Uu')
        if [ "$UAPI_VAL" != "$SOF_VAL" ]; then
            bbfatal "$SYM mismatch: kernel UAPI ($UAPI_VAL) vs SOF firmware ($SOF_VAL)."
        fi
    done
}
```

C'est un **invariant single-source-of-truth** garanti au build : si quelqu'un modifie NPU_TAP_IN_PHYS_ADDR dans le firmware sans modifier le kernel UAPI (ou inversement), `bitbake` échoue net.

#### 3.3.3 `meta-local/recipes-kernel/linux/linux-imx_%.bbappend`

```bitbake
FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

SRC_URI += "file://0001-imx8mp-evk-audio-mipi.patch"
SRC_URI += "file://spdif.cfg"
SRC_URI += "file://disable-at24.cfg"

# SOF probes — patch additif (nouveau fichier, no behavior change)
SRC_URI += "file://imx-probes.c"
SRC_URI += "file://apply-imx-probes.py"
SRC_URI += "file://sof-imx-probes.cfg"

# TAC5212 codec — driver + DT
SRC_URI += "file://tac5212.c file://tac5212.h"
SRC_URI += "file://apply-tac5212-dt.py"
SRC_URI += "file://tac5212.cfg"

# NPU tap V7.0-E4 — DT carve out (rmem 0x94270000 + 0x942B0000)
SRC_URI += "file://apply-npu-tap-dt.py"

# V5.4.1 SDRAM2 — DT carve 8 MB (mailbox DSP)
SRC_URI += "file://apply-sdram2-dt.py"

# Multi-codec TDM (4 TACs sur 1 DAI link)
SRC_URI += "file://apply-simple-card-multicodec.py"
SRC_URI += "file://apply-imx-card-linkid.py"

do_patch:prepend() {
    cp ${WORKDIR}/imx-probes.c ${S}/sound/soc/sof/imx/imx-probes.c
    python3 ${WORKDIR}/apply-imx-probes.py ${S}
    cp ${WORKDIR}/tac5212.c ${S}/sound/soc/codecs/tac5212.c
    cp ${WORKDIR}/tac5212.h ${S}/sound/soc/codecs/tac5212.h
    # Ajout Kconfig + Makefile entries
    ...
}
do_patch:append() {
    python3 ${WORKDIR}/apply-tac5212-dt.py ${S}/arch/arm64/boot/dts/freescale/imx8mp-evk.dts
    python3 ${WORKDIR}/apply-npu-tap-dt.py  ${S}/arch/arm64/boot/dts/freescale/imx8mp-evk.dts
    python3 ${WORKDIR}/apply-sdram2-dt.py   ${S}/arch/arm64/boot/dts/freescale/imx8mp-evk.dts
    python3 ${WORKDIR}/apply-simple-card-multicodec.py ${S}
    python3 ${WORKDIR}/apply-imx-card-linkid.py        ${S}
}
do_configure:append() {
    cfg="${B}/.config"
    sed -i 's/# CONFIG_SND_SOC_TAC5212 is not set/CONFIG_SND_SOC_TAC5212=m/' "$cfg"
    sed -i 's/# CONFIG_SND_SOC_SOF_IMX_PROBES is not set/CONFIG_SND_SOC_SOF_IMX_PROBES=m/' "$cfg"
    oe_runmake -C ${S} O=${B} olddefconfig
}
```

**Stratégie de patch** : scripts Python idempotents qui détectent si la modification est déjà appliquée (re-runs no-op). Pattern utilisé pour DT, Kconfig, Makefile — évite les conflits de patch lors d'un rebase upstream.

#### 3.3.4 `meta-local/recipes-bsp/boot-script-rt/boot-script-rt_1.0.bb`

V8.33 — recipe Yocto qui compile `boot.cmd` → `boot.scr` (U-Boot script) avec les bootargs isolcpus pour RT :

```bitbake
SUMMARY = "V8.33 U-Boot boot.scr — bootargs RT (isolcpus=2,3) pour mixer-pro"
SRC_URI = "file://boot.cmd"
DEPENDS = "u-boot-tools-native"

do_compile() {
    mkimage -A arm -O linux -T script -C none -n "Debix RT boot" \
        -d ${WORKDIR}/boot.cmd ${B}/boot.scr
}
do_install() {
    install -d ${D}/boot
    install -m 0644 ${B}/boot.scr ${D}/boot/boot.scr
}
FILES:${PN} = "/boot/boot.scr"
COMPATIBLE_MACHINE = "(imx8mpevk|imx8mp-debix-model-ab)"
```

Contenu de `boot.cmd` :

```bash
echo "=== Debix Model AB — RT bootargs (isolcpus=2,3 for mixer-pro) ==="
setenv bootargs "Debix_Model_AB V1.0.3 console=ttymxc1,115200 root=/dev/mmcblk1p2 \
    rootwait rw isolcpus=2,3 nohz_full=2,3 rcu_nocbs=2,3"
load mmc ${mmcdev}:${mmcpart} ${loadaddr} Image
load mmc ${mmcdev}:${mmcpart} ${fdt_addr_r} imx8mp-evk.dtb
booti ${loadaddr} - ${fdt_addr_r}
```

U-Boot charge ce script avant `booti`, ce qui injecte les bootargs **sans rebuild U-Boot**.

### 3.4 Image manifest — packages installés

`imx-image-full.bbappend` (meta-local) ajoute au-dessus de la base NXP :

```bitbake
# Audio toolchain de base
IMAGE_INSTALL:append = " sox alsa-plugins alsa-tools"
IMAGE_INSTALL:append = " imx-dsp imx-dsp-codec-ext imx-dspc-asrc speexdsp ladspa-sdk spandsp"

# SOF firmware
IMAGE_INSTALL:append = " sof-zephyr sof-tools sof-firmware-custom"

# NPU tap (méta-package, pull kernel-module-imx-audio-tap-6.6.36)
IMAGE_INSTALL:append = " imx-audio-tap"

# Init TAC5212
IMAGE_INSTALL:append = " tac5212-service"

# USB gadget
IMAGE_INSTALL:append = " usb-uac2-gadget"

# Phone aloop
IMAGE_INSTALL:append = " snd-aloop-phone"
IMAGE_INSTALL:append = " alsa-route-bridge"

# Mixer userspace + GUI
IMAGE_INSTALL:append = " mixer-pro mixer-gui-http"

# V8.33 isolcpus
IMAGE_INSTALL:append = " boot-script-rt"

# Python audio
IMAGE_INSTALL:append = " python3-numpy python3-pyaudio"

# ML/NPU stack
IMAGE_INSTALL:append = " packagegroup-imx-ml"
```

---

## 4. Stack logicielle — vue système

### 4.1 Les 4 niveaux d'exécution

```
┌──────────────────────────────────────────────────────────────────────────┐
│  Niveau 4 : Browser (PC distant)                                          │
│    - Alpine.js + Tailwind CSS                                             │
│    - REST /api/state, /api/cmd, /api/drift, /api/meters, /api/stream      │
│    - WebSocket streaming pour analyzers FFT temps réel                    │
└────────────────────────────────────┬─────────────────────────────────────┘
                                     │ HTTP/JSON
┌────────────────────────────────────▼─────────────────────────────────────┐
│  Niveau 3 : Linux userspace (Cortex-A53)                                  │
│    - mixer-gui-http (libmicrohttpd) — backend HTTP REST + WebSocket       │
│    - mixer-pro (daemon C) — 26 inputs × 18 outputs + 4 bus FX             │
│      ▸ audio_thread     (SCHED_FIFO prio 80, cores 2-3 isolcpus)          │
│      ▸ play_thread      (SCHED_FIFO prio 81, ring SPSC)                   │
│      ▸ cap_uac2_thread  (SCHED_FIFO prio 80, NONBLOCK USB readi)          │
│      ▸ play_uac2_thread (SCHED_FIFO prio 80)                              │
│      ▸ control_thread   (Unix socket /run/mixer-pro.sock)                 │
│      ▸ analyzer_thread  (SCHED_FIFO prio 60, FFT 1024 + scope)            │
│    - tac5212-service (systemd oneshot — init TAC PLL au boot)             │
│    - usb-uac2-gadget (configfs script, expose carte son USB)              │
└────────────────────────────────────┬─────────────────────────────────────┘
                                     │ ALSA /dev/snd, IPC SOF, mmap NPU tap
┌────────────────────────────────────▼─────────────────────────────────────┐
│  Niveau 2 : Kernel Linux 6.6.36 (Cortex-A53)                              │
│    - tac5212.c — codec driver (snd_soc_codec_driver)                      │
│    - imx-audio-tap.c — module out-of-tree (miscdevice mmap)               │
│    - fsl_sai.c — SAI driver (mainline, NPU-tap aware via SOF)             │
│    - imx-sdma.c — SDMA driver (mainline)                                  │
│    - dwc3 + f_uac2 — USB gadget UAC2 8×8                                  │
│    - sof-imx + sof-imx8m — SOF host driver (IPC, topology load)           │
│    - snd-aloop — phone loopback simulator                                  │
│    - rpmsg — communication A53 ↔ Cortex-M7 (non utilisé audio)            │
└────────────────────────────────────┬─────────────────────────────────────┘
                                     │ Mailbox IPC, DRAM partagée
┌────────────────────────────────────▼─────────────────────────────────────┐
│  Niveau 1 : SOF DSP firmware (HiFi4 Xtensa, Zephyr RTOS v2.10.0)          │
│    - sai.c — driver SAI HiFi4 (DMA cb, register access)                   │
│    - sdma.c — driver SDMA HiFi4 (channel mgmt)                             │
│    - multiband_drc — composant 8ch, multi-config blob                     │
│    - drc D3 — composant per-channel arrays (state + multi-blob)          │
│    - pga — gain 8ch                                                        │
│    - dai-zephyr.c — pipeline orchestration                                 │
│    - npu_tap hooks dai_dma_cb() — publication DRAM partagée               │
│    - 1 pipe cap unique (PCM 0) + 1 pipe play unique (PCM 1)               │
└──────────────────────────────────────────────────────────────────────────┘
```

### 4.2 Flux audio principal (steady state)

```
   MIC →  TAC ADC →  SAI7 RX  →  SDMA  →  DSP cap pipe  →  PCM 0  →  mixer-pro (cap_dsp)
                                                                          │
                                                                          ▼
                                                                  matrix 26×18
                                                                          │
                                                                          ▼
                                                              ┌──────────┴──────────┐
                                                              ▼                     ▼
                                              ring SPSC (audio→play)         ring USB play
                                                              │                     │
                                                              ▼                     ▼
                                              play_thread (PCM 1 play)    play_uac2_thread
                                                              │                     │
                                                              ▼                     ▼
                                                       DSP play pipe        USB host (DAW)
                                                              │
                                                              ▼
                                                          SAI7 TX
                                                              │
                                                              ▼
                                                          TAC DAC
                                                              │
                                                              ▼
                                                          SPEAKER
```

Et l'inverse côté USB cap (DAW → mixer) :

```
   DAW (PC host) → USB UAC2 cap → f_uac2 (kernel) → snd_pcm_readi
                                                          │
                                                          ▼
                                              cap_uac2_thread (NONBLOCK)
                                                          │
                                                          ▼
                                              ASRC drop/insert ±1
                                                          │
                                                          ▼
                                              ring SPSC (cap USB)
                                                          │
                                                          ▼
                                              audio_thread pop
                                                          │
                                                          ▼
                                              matrix mix 26×18  (←── routed to outputs)
```

### 4.3 Domaines d'horloge (clock domains)

Trois horloges indépendantes coexistent :

| Domaine | Source | Fréquence | Stabilité |
|---|---|---|---|
| **DSP audio** | quartz XO i.MX 8M Plus → audiomix PLL → SAI7 BCLK | 48 000 Hz hardware-exact (jitter ppm) | ± ~10 ppm typique |
| **USB host** | quartz XO PC hôte → USB SOF clock 1 ms | 48 000 Hz nominal mais selon quartz PC | ± 50 à 100 ppm |
| **CPU Linux** | quartz XO i.MX 8M Plus → ARM PLL → CLOCK_MONOTONIC | 1 GHz / sec | identique DSP quartz |

**Conséquence majeure** : DSP et USB ont des horloges **physiquement différentes** (deux quartz indépendants). En 48 kHz nominal, le DSP délivre exactement 48 000 samples/s alors que l'USB host peut délivrer 48 047 samples/s. Ce **drift** est la cause principale des problèmes audio (voir §11).

Le CPU Linux est sur le **même quartz** que le DSP (i.MX 8M Plus interne) donc en théorie pas de drift CPU↔DSP, juste du jitter de scheduling.

---

## 5. Kernel Linux — drivers audio

### 5.1 Stack ALSA SoC + SOF

```
                    User : ALSA app (mixer-pro)
                              │
                              ▼
                  ╔═══════════════════════╗
                  ║   ALSA core (snd_*)    ║  /dev/snd/pcmC*D*
                  ╚═══════════════════════╝
                              │
            ┌─────────────────┴─────────────────┐
            ▼                                   ▼
   snd_soc_card (machine driver)       f_uac2 (USB gadget)
   "imx-card", "snd-aloop-phone"
            │
            ▼
   ┌─────────────────────────────┐
   │ DAI link 0 (DSP ← SOF)       │  "softac5212tdm" (SOF FE/BE)
   │                              │
   │  - CPU DAI = SOF DSP pipe    │
   │  - Codec DAI = tac5212 (×4)  │
   │  - DAI fmt = DSP_A 8-slot TDM│
   └─────────────────────────────┘
            │
   ┌────────┴────────┐
   ▼                 ▼
SOF driver       TAC5212 codec
(sof-imx)        driver (snd_soc_dai_driver)
   │                 │ (I2C bus 3)
   ▼                 ▼
SOF firmware       TAC5212 chip
(HiFi4 DSP)        ×4 (0x50..0x53)
```

### 5.2 Driver TAC5212 (`meta-local/recipes-kernel/linux/files/tac5212.c`)

1281 lignes, copié dans `sound/soc/codecs/` au build kernel.

#### 5.2.1 Structure de probe

```c
struct tac5212_priv {
    struct regmap          *regmap;
    struct i2c_client      *client;
    struct device          *dev;
    int                     pdm_ch_sel;       /* User's PDM_CH_SEL choice */
    bool                    needs_reset;      /* Set on bind, cleared on first hw_params */
    struct gpio_desc       *reset_gpio;       /* Optional, currently unused (PCB-tied) */
    /* DRC, biquad, agc state arrays */
    ...
};
```

`needs_reset` est central : il déclenche un **reset SW logiciel** du codec à la première ouverture stream (voir §8.4).

#### 5.2.2 Configuration TDM par slot

Chaque TAC5212 est assigné 2 slots TDM (chaîne daisy DOUT) via le DT :

```
TAC0 (0x50) : slot 0, 1   (MIC0/MIC1, SPK0/SPK1)
TAC1 (0x51) : slot 2, 3
TAC2 (0x52) : slot 4, 5
TAC3 (0x53) : slot 6, 7
```

L'I2C écriture set_tdm_slot dans driver (lignes ~1000-1050) :

```c
/* TX_OFFSET, RX_OFFSET en bits depuis FSYNC, slot_width en bits */
slot1_bits = slot1 * slot_width;
slot2_bits = slot2 * slot_width;
regmap_write(regmap, TAC5212_TX_OFFSET1, slot1_bits);
regmap_write(regmap, TAC5212_TX_OFFSET2, slot2_bits);
regmap_write(regmap, TAC5212_RX_OFFSET1, slot1_bits);
regmap_write(regmap, TAC5212_RX_OFFSET2, slot2_bits);
```

### 5.3 Driver SAI (mainline `sound/soc/fsl/fsl_sai.c`, patché par SOF)

Le SAI est piloté **uniquement par le firmware SOF** côté DSP. Le driver Linux fsl_sai.c initialise les clocks et la machine driver, mais en mode SOF il **délègue le set_config au DSP via IPC**. Pas de patches custom dans meta-local pour SAI.

Côté kernel, c'est la **topology SOF** (`.tplg` chargée par sof-imx-driver) qui décrit le DAI SAI7 :

```
.tplg fichier : sof-imx8mp-tac5212.tplg
  └── DAI link : "SAI7"
        ├── type : SOF_DAI_IMX_SAI
        ├── format : SOF_DAI_FMT_DSP_A | SOF_DAI_FMT_CBM_CFM  (master = SAI side)
        ├── mclk_id : 1 (audiomix PLL)
        ├── bclk_rate : 12288000 (32 bits × 8 slots × 48 kHz)
        ├── tdm_slots : 8
        ├── tdm_slot_width : 32
        └── rx_mask = tx_mask = 0xFF (all 8 slots actifs)
```

### 5.4 Module imx-audio-tap (out-of-tree)

Voir `/home/michael/yocto-nxp-debix/meta-local/recipes-kernel/imx-audio-tap/files/imx-audio-tap.c` (227 lignes, déjà cité §1.5).

Points clés :

1. **Probe par DT compatible** `"electrosens,imx-audio-tap"` — 2 instances bindées (tap-in, tap-out)
2. **Adresse phys récupérée du DT** via `of_reserved_mem_lookup()`, **PAS** hardcodée au runtime (la constante UAPI sert uniquement à la cross-check Yocto)
3. **mmap PROT_READ writecombine** :
   ```c
   vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
   if (remap_pfn_range(vma, vma->vm_start, priv->phys_addr >> PAGE_SHIFT, size, vma->vm_page_prot))
       return -EAGAIN;
   ```
4. **sysfs attrs** `phys_addr`, `size` (read-only) pour debug
5. **device-name from DT** `"imx-audio-tap-in"` ou `"imx-audio-tap-out"` → `/dev/imx-audio-tap-{in,out}`

Le module ne fait **aucune copie** : juste un mapping mémoire physique vers userspace. Le DSP firmware écrit directement dans cette zone, le userspace lit en seqcount-style synchronisé via header partagé (voir §9).

### 5.5 USB DWC3 + f_uac2 gadget

Driver mainline `drivers/usb/dwc3/` + `drivers/usb/gadget/function/f_uac2.c`.

Configuration via configfs script (`usb-uac2-gadget` recipe) :

```bash
cd /sys/kernel/config/usb_gadget
mkdir g1 && cd g1

echo 0x1d6b > idVendor          # Linux Foundation
echo 0x0104 > idProduct         # Multifunction Composite Gadget
mkdir strings/0x409
echo "Debix Model AB Audio" > strings/0x409/product

mkdir functions/uac2.0
echo 8 > functions/uac2.0/c_chmask      # 8 ch capture
echo 8 > functions/uac2.0/p_chmask      # 8 ch playback
echo 48000 > functions/uac2.0/c_srate
echo 48000 > functions/uac2.0/p_srate
echo 4 > functions/uac2.0/c_ssize       # S32_LE (4 bytes)
echo 4 > functions/uac2.0/p_ssize
echo 96 > functions/uac2.0/req_number   # ALSA period size 96 = 2 ms

mkdir configs/c.1
ln -s functions/uac2.0 configs/c.1/
echo 32f10100.usb > UDC   # Bind to USB OTG controller
```

Côté PC host, la carte apparaît comme :

```
$ arecord -l
card 2: UAC2Gadget [UAC2_Gadget], device 0: UAC2 PCM
$ aplay -l
card 2: UAC2Gadget [UAC2_Gadget], device 0: UAC2 PCM
```

8 canaux capture + 8 canaux playback, S32_LE 48 kHz. DAW (Reaper/Ardour/Bitwig) sélectionne cette carte comme interface audio.

---

## 6. SOF DSP firmware

### 6.1 Quel SOF ?

Le projet utilise un **fork custom** de [SOF (Sound Open Firmware)](https://github.com/thesofproject/sof), version basée sur **Zephyr RTOS v2.10**, compilé pour le DSP HiFi4 du i.MX 8M Plus.

**Remote** :
- `github` → `https://github.com/Mjxkill/sof.git` (fork avec patches projet)
- `origin` → cache local du upstream `thesofproject/sof`

**Branche actuelle** : `feature/v7.0-multiband-drc-tap`, dernier commit `4bea8e59d` (V7.0-E7.2 TX FIFO alignment).

### 6.2 Patches projet appliqués au SOF

| Patch | Détail |
|---|---|
| **TAC5212 codec descriptor** | Ajout du DAI codec TAC5212 dans `src/audio/dai-zephyr.c` |
| **multiband_drc multi-config** | Patch `multiband_drc` pour supporter N blobs (1 par canal) au lieu d'1 blob global |
| **drc D3 state arrays** | Patch `drc` (component) pour avoir des state arrays per-channel (8 instances vs 1) + multi-blob detection ABI back-compat |
| **NPU tap dual** | Hook `dai_dma_cb()` après `dma_buffer_copy_from/to` OK pour publier vers `tap-in` (cap) et `tap-out` (play) DRAM partagée |
| **SAI TX FIFO align** | `sai_set_config` prime N zeros au lieu de 1 dans tdm_slots — fix décalage TX wire permanent (V7.0-E7.2) |
| **SAI idempotence** | `sai_set_config` skip si `configured` flag déjà set — évite glitch BCLK lors d'un 2e set_config (cap après play déjà active) |
| **SAI ASYNC + continuous clock** | Mode ASYNC : TX master génère BCLK continu, RX clocke sur ce BCLK via PCB loopback. Ne pas stopper TX au stop pour éviter TAC PLL unlock |
| **multiband_drc copy seq guard** | Fix explosion combinatoire walk multi-fanout intra-pipeline (E6.a matrix 16×8) |
| **pipeline_complete auto-detect** | Détection auto pipeline complet, skip locked param dans pcm_params (E5.e.1 Option F++) |

### 6.3 Topologies (.m4 → .tplg)

Le fichier source M4 est compilé par `m4 + alsatplg` :

```
meta-local/recipes-kernel/linux/files/sof-imx8mp-tac5212.m4
                  ↓ m4
sof-imx8mp-tac5212.conf
                  ↓ alsatplg -c sof-imx8mp-tac5212.conf -o sof-imx8mp-tac5212.tplg
sof-imx8mp-tac5212.tplg  → /lib/firmware/imx/sof-tplg/
```

La topology décrit la pipeline DSP :

```
PIPE 1 CAP (SAI7 RX → ... → PCM 0)
  ├── DAI_IN  : SAI7 (cap, 8ch S32_LE, period 96 = 2 ms, DMA)
  ├── BUF (8ch × 96 frames × 4 bytes = 3072 B)
  ├── PGA (volume 8ch indépendants)
  ├── MULTIBAND_DRC (8ch, 4 bands per channel, blob)
  └── HOST  : PCM 0 cap (8ch S32_LE)

PIPE 2 PLAY (PCM 1 → ... → SAI7 TX)
  ├── HOST  : PCM 1 play (8ch S32_LE)
  ├── PGA
  ├── MULTIBAND_DRC
  └── DAI_OUT : SAI7 (play, 8ch S32_LE, period 96)
```

**Invariant clé** : 1 pipe cap + 1 pipe play (mono-instance), pas de cross-pipeline. Memory `[[project_v7_audio_devices]]` + `[[sof_pivot_1comp_8ch]]`.

### 6.4 Composant `multiband_drc` patché

Le composant SOF mainline supporte 1 blob par instance. Pour le projet, on a 8 canaux indépendants chacun avec son blob de coefficients (DRC, EQ, attack/release différents). Solution : **patch multi-config** qui charge N blobs séquentiels et les distribue par channel-map.

```c
/* Pseudocode du patch */
struct multiband_drc_state {
    struct drc_state state[MAX_CHANNELS];  /* 8 instances de state DRC */
    struct multiband_drc_config *config[MAX_CHANNELS];  /* 8 blobs */
};

static int multiband_drc_set_data(struct comp_dev *dev, struct sof_ipc_ctrl_data *cdata) {
    /* Parse multi-config blob : header magic + N sub-configs */
    if (cdata->data->magic == MULTI_CONFIG_MAGIC) {
        for (int ch = 0; ch < dev->channels; ch++) {
            extract_sub_config(cdata, ch, &cd->config[ch]);
        }
    } else {
        /* Single config (ABI compat) — broadcast à tous les ch */
        for (int ch = 0; ch < dev->channels; ch++)
            cd->config[ch] = single_config;
    }
}
```

### 6.5 Hook NPU tap dans dai_dma_cb

Le hook critique est dans `src/audio/dai-zephyr.c`, juste après `dma_buffer_copy_from()` (capture) ou `dma_buffer_copy_to()` (playback) :

```c
static void dai_dma_cb(struct comp_dev *dev, enum dma_cb_status status)
{
    /* ... normal SOF processing ... */

    if (status != DMA_CB_STATUS_OK)
        return;

    /* === V7.0-E4 NPU TAP HOOK === */
    if (dev->direction == SOF_IPC_STREAM_CAPTURE && dd == npu_tap_in_owner) {
        npu_tap_publish(NPU_TAP_IN_PHYS_ADDR, dd->local_buffer,
                        dd->period_bytes, /* direction=1 */ 1);
    } else if (dev->direction == SOF_IPC_STREAM_PLAYBACK && dd == npu_tap_out_owner) {
        npu_tap_publish(NPU_TAP_OUT_PHYS_ADDR, dd->local_buffer,
                        dd->period_bytes, /* direction=0 */ 0);
    }
}
```

La fonction `npu_tap_publish` (dans `src/audio/npu_tap.c`) :

1. Copie `period_bytes` du buffer SOF vers la zone DRAM partagée (offset = write_idx % ring_size + header_size)
2. Met à jour `write_idx` avec wrap modulo `ring_size`
3. Increment `epoch` (counter de seqcount)
4. `dcache_writeback_region()` pour pousser hors cache HiFi4
5. `memw` barrière mémoire
6. Magic header écrit en DERNIER (R3 publish-pattern)

Sentinelles `npu_tap_in_owner` / `npu_tap_out_owner` (1 par direction) permettent à un seul DAI de capturer le hook par direction — invariant R7.

---

## 7. Configuration SAI continue + DMA RX/TX

C'est l'un des sujets les plus délicats du projet. Le SAI (Synchronous Audio Interface) de l'i.MX 8M Plus génère les horloges BCLK/FSYNC qui pilotent **tous** les codecs TAC5212. Sa configuration détermine le comportement temps réel de toute la chaîne audio.

### 7.1 Mode ASYNC vs SYNC — le choix architectural

Le SAI supporte 2 modes :

**Mode SYNC** : TX et RX partagent le même module de génération d'horloge. Tout est synchronisé en interne. Mais le timing entre BCLK et data peut diverger (notamment quand TX est désactivé temporairement).

**Mode ASYNC** : TX et RX sont 2 modules indépendants. TX génère BCLK/FSYNC vers les pins, **les pins sont physiquement bouclées en PCB vers les pins RX** (via traces sur la carte Debix Model AB). RX reçoit BCLK/FSYNC comme s'ils venaient d'un master externe.

**Choix projet : ASYNC + continuous clock**. Memory `[[sof_async_solution]]`.

```c
/* SOF sai.c — config RX en mode ASYNC */
mask_cr2 |= REG_SAI_CR2_SYNC_MASK;
val_cr2 &= ~REG_SAI_CR2_BCD_MSTR;    /* RX BCLK = INPUT (= consumer) */
val_cr4 &= ~REG_SAI_CR4_FSD_MSTR;    /* RX FSYNC = INPUT */

dai_update_bits(dai, REG_SAI_XCR2(REG_RX_DIR), mask_cr2, val_cr2);
```

**Pourquoi ASYNC** : avec ASYNC, RX consume les data depuis le pin DIN du PCB **avec la même propagation delay que les codecs** (path traces matérielles identiques). On obtient un alignement temporel naturel entre BCLK et data RX, ce qu'un mode SYNC interne ne peut pas garantir (timing internal vs external différent).

### 7.2 Continuous BCLK/FSYNC — pourquoi ne jamais stopper TX

```c
/* SOF sai.c — sai_stop, direction PLAYBACK avec sai->configured = true */
if (direction == DAI_DIR_PLAYBACK && sai->configured) {
    dai_update_bits(dai, REG_SAI_XCSR(direction), REG_SAI_CSR_FRDE, 0);
    return;
}

/* ASYNC mode with continuous clock:
 * - TX (playback) direction: NEVER stop TRCE or TERE. TX is the clock
 *   master and its output pins are physically looped back to RX pins
 *   on the PCB. Stopping TX would drop BCLK/FSYNC and unlock the TAC
 *   codec PLL. Only FRDE (DMA request) is disabled above, which stops
 *   data movement while keeping BCLK/FSYNC running.
 * - RX (capture) direction: stop TRCE and TERE normally. RX is
 *   consumer-only; stopping it has no effect on clock generation.
 */
```

Les bits SAI critiques :

| Bit | Effet |
|---|---|
| **TERE** (Transmit Enable) | Active TX. Stoppe BCLK/FSYNC quand mis à 0 |
| **TRCE** (Transmit Channel Enable) | Active les channels TX |
| **FRDE** (FIFO Request DMA Enable) | Active la requête DMA — peut être togglé sans toucher BCLK |
| **MCLK_EN** | Master clock enable. Bound to TERE sur i.MX8MP — TE doit être set pour générer MCLK |
| **SR** (Software Reset) | Reset SW interne. Si appliqué à TX, BCLK glitch |

**Règle d'or** (memory `[[feedback_tx_master_drives_all]]`) :

> TX pilote TOUT, jamais le TAC. Si RX 0 IRQ, chercher SAI/SDMA/pinmux, JAMAIS le TAC.

### 7.3 Idempotence sai_set_config — le bug invisible

Quand mixer-pro ouvre **simultanément cap_dsp et play_dsp** (deux directions du même device DSP), le kernel SOF appelle `sai_set_config()` **2 fois** :

```
1. Open cap_dsp     → sof-imx → sai_set_config(SAI7) → TX TERE=1 + RX configured
2. Open play_dsp    → sof-imx → sai_set_config(SAI7) → ré-écrit TX TERE=1
```

Le 2e write semble redondant mais **glitche le BCLK** pendant l'écriture (read-modify-write transient). Le TAC PLL perd la lock pendant ~quelques µs → restitution corrompue (artefacts métalliques).

**Fix** (memory `[[sof_sai_idempotence_patch]]`) :

```c
static inline int sai_set_config(struct dai *dai, ...)
{
    /* IDEMPOTENCE: configure SAI exactly once. */
    if (sai->configured) {
        dai_info(dai, "SAI: sai_set_config skipped (already configured)");
        return 0;
    }
    /* ... full config ... */
    sai->configured = true;
    return 0;
}
```

Une fois `configured = true`, les appels suivants sont no-op. Idem pour `sai_start`, `sai_stop`, `sai_release` qui ne touchent plus que `FRDE` (toggle sans impact clock).

### 7.4 V7.0-E7.2 — TX FIFO alignment

Bug observé empiriquement : à chaque boot, TDM TX out arrivait décalé d'1 slot (slot 0 = ch0 OK, slot 1 = ch7, slot 2..7 silent).

**Cause** : avant `TERE=1`, le code primait **1 zero** dans `TDR0`. Insuffisant pour aligner le FIFO read pointer avec FSYNC slot 0 → décalage permanent sur le wire.

**Fix V7.0-E7.2** :

```c
/* V7.0-E7.2 TX FIFO alignment fix : prime N zeros (= 1 full TDM frame)
 * BEFORE TERE=1 so the SAI starts with one frame of silent data already
 * in the FIFO. This guarantees the read pointer R is aligned with FSYNC
 * slot 0 at first frame edge. */
{
    int i_prime;
    for (i_prime = 0; i_prime < sai->params.tdm_slots; i_prime++)
        dai_write(dai, REG_SAI_TDR0, 0x0);
}
dai_update_bits(dai, REG_SAI_XCSR(DAI_DIR_PLAYBACK),
                REG_SAI_CSR_TERE, REG_SAI_CSR_TERE);
dai_update_bits(dai, REG_SAI_MCTL, REG_SAI_MCTL_MCLK_EN, REG_SAI_MCTL_MCLK_EN);
```

8 zeros écrits (= 8 slots = 1 frame TDM) avant TERE=1. Garantit alignement FIFO ↔ FSYNC.

### 7.5 DMA SDMA — Continuous mode

Le SDMA (Smart Direct Memory Access) gère le transfert entre buffer DSP (DRAM) et FIFO SAI. Pour l'audio temps réel, on utilise un **buffer cyclique** :

```
                          DSP DRAM buffer (8 ch × 4 periods × 96 frames × 4 B = 12 KB)
                          ┌────────────────────────────────────────────┐
                          │ Period 0  │ Period 1  │ Period 2  │ Per. 3 │
                          └─────┬─────┴─────┬─────┴─────┬─────┴───┬────┘
                                │           │           │         │
                                └───────────┴───────────┴─────────┘
                                       Continuous cyclic DMA
                                              │
                          ┌───────────────────▼──────────────────────┐
                          │ SAI7 TX/RX FIFO 64 × 32 bits             │
                          └───────────────────┬──────────────────────┘
                                              │
                                       TDM wire 8 slots
                                              │
                                              ▼
                                       4× TAC5212
```

Le DMA est **continuously running** : à chaque period (96 frames = 2 ms), il transfère 96×8×4 = 3072 bytes entre DRAM et SAI FIFO. À la fin du dernier period, il wrappe au premier (cyclic).

Callback DMA (`dma_irq_cb`) appelé par interruption à la fin de chaque period :
1. Update `read_idx` / `write_idx` du buffer DRAM
2. Notify SOF pipeline (signal `pipe_task`)
3. Hook NPU tap → publish vers DRAM partagée
4. Si pipeline pas prêt → underrun/overrun (xrun)

**Continuous mode** = pas de start/stop par period. Le DMA tourne tant que le PCM stream est actif. Seul `FRDE` toggle pour stopper la requête (drain le FIFO).

### 7.6 Le "même copy en DMA" — mécanisme de chaining

Quand on a 2 directions (cap + play) qui doivent se synchroniser :

```
Cap DMA channel   : SAI7 RX FIFO → DRAM cap buffer (cyclic, 4 periods)
Play DMA channel  : DRAM play buffer → SAI7 TX FIFO (cyclic, 4 periods)
                                  ↓
                          Triggered par même MCLK
```

Les 2 channels SDMA sont **indépendants** mais **alimentés par la même horloge SAI**. Synchronisation naturelle : 1 period TX = 1 period RX = 2 ms.

Le DSP HiFi4 maintient les 2 buffers cycliques en DRAM. Le DSP pipeline (multiband_drc, pga…) traite chaque period à mesure qu'elle arrive, dans le timer `pipe_task` réveillé par `dma_cb`.

```c
/* Pseudo-code du chemin cap → process → play */
void dma_cap_cb(void) {
    /* 1. Tap NPU : copie raw vers tap-in DRAM partagée */
    npu_tap_publish(NPU_TAP_IN_PHYS_ADDR, cap_dma_buffer, 3072, /*cap*/1);
    /* 2. Réveille le pipe_task cap qui lance le pipeline DSP */
    pipe_task_schedule(pipe_cap);
}

void pipe_task_cap(void) {
    /* Multiband DRC + PGA */
    multiband_drc_process(...);
    pga_process(...);
    /* Push au HOST PCM */
    host_pcm_copy(...);
}
```

Le **HOST PCM** est l'interface entre DSP et Linux. À chaque period, le DSP signale via IPC mailbox que de nouvelles data sont prêtes pour readi côté Linux.

### 7.7 Récap config SAI7 valide V7.0

```
Registers fondamentaux (mode ASYNC, master TX, slave RX) :

TX side (REG_SAI_XCSR(0), XCR2(0), XCR4(0), XCR5(0)) :
  - TERE  = 1   (TX Enable, génère BCLK)
  - TRCE  = 1   (TX Channel Enable channel 0)
  - FRDE  toggled par sai_start/stop
  - BCD_MSTR = 1   (BCLK direction master, génère vers pin)
  - FSD_MSTR = 1   (FSYNC direction master)
  - DIV    = (MCLK / BCLK / 2) - 1
  - SYWD   = 32   (slot width 32 bits)
  - FRSZ   = 8    (8 slots TDM)
  - FCONT  = 1    (frame continuous = no idle slots)
  - FPACK  = ... (packing 8/16/32 selon mode)

RX side (REG_SAI_XCSR(1)) :
  - SYNC   = ASYNC (les 2 directions indépendantes)
  - BCD_MSTR = 0  (BCLK = INPUT depuis pin RX-BCLK)
  - FSD_MSTR = 0  (FSYNC = INPUT)
  - FRDE  toggled par sai_start/stop
  - RR (RX Reset) = 0 (jamais full reset, juste SR pour clear FIFO)

MCTL_MCLK_EN = 1   (audiomix PLL délivre MCLK à SAI7)

XMR (slot mask) :
  - tx_slots = 0xFF  (all 8 active)
  - rx_slots = 0xFF
```

---

## 8. Codec TAC5212 — configuration et reset PLL

### 8.1 Présentation TAC5212

Le **TAC5212** de Texas Instruments est un codec audio stéréo intégré (datasheet [SLASF23A](https://www.ti.com/product/TAC5212)) :

- 2 canaux ADC (analog mic in)
- 2 canaux DAC (analog speaker out)
- Format I/O : TDM, I2S, Left-J, Right-J, PDM
- Sample rates : 8/16/24/32/44.1/48/96/192 kHz
- Résolution : 16/20/24/32 bits
- Contrôle : I2C @ 100-400 kHz (4 adresses possibles : 0x50, 0x51, 0x52, 0x53)
- **Effets ADC intégrés** : AGC, HPF, biquads, gain, decimation filter (linear-phase / low-lat / ultra-low-lat), digital channel mixer
- **Effets DAC intégrés** : interpolation filter (linear-phase / low-lat / ultra-low-lat), biquads, DRC, gain/volume, distortion limiter, thermal foldback, battery guard, tone generator
- **VAD/UAD** : Voice Activity Detection / Ultrasonic Activity Detection
- **PLL interne** : auto-lock sur BCLK externe

### 8.2 Topologie chaîne TDM 4× TAC5212

Sur la carte Debix Model AB, **4 TAC5212** sont en daisy chain TDM 8 slots, partageant un wire DOUT physique :

```
SAI7 (master)
  │
  │ BCLK ───────────────────────────────────────────────┐
  │ FSYNC ──────────────────────────────────────────────│
  │ DIN  ─────────────────────────────────────────────┐ │  (data IN to TACs, ADC reads ignore)
  │ DOUT ◄──────┬──────────────┬──────────────┬────────│─│─── (shared bus, ADC writes ICOM)
  │             │              │              │        │ │
  ▼             ▼              ▼              ▼        ▼ ▼
TAC0          TAC1           TAC2           TAC3      (PCB feedback to RX SAI)
(I2C 0x50)    (0x51)         (0x52)         (0x53)
slot 0,1      slot 2,3       slot 4,5       slot 6,7
```

**Daisy chain DOUT** : les 4 TACs partagent un single wire DOUT. Chaque TAC ne pilote ce wire que pendant **ses slots assignés** (lui-même configuré via I2C). Pendant les autres slots, son driver DOUT est en haute impédance.

Pour éviter contention DOUT, chaque TAC est configuré avec :
- `TX_OFFSET1` = slot_a * slot_width (en bits) — offset 1er slot
- `TX_OFFSET2` = slot_b * slot_width — offset 2e slot
- `KEEPER` activé (sauf TAC0 qui le désactive — c'est lui qui tient le bus quand tous les autres sont en HiZ)

### 8.3 Pourquoi le reset TAC est nécessaire

#### 8.3.1 Le problème

À la mise sous tension de la carte :
1. SAI7 n'est pas encore configuré → BCLK absent
2. Les TAC5212 boot sans BCLK
3. Leur PLL interne n'a **pas de référence** pour locker
4. Le décodeur PDM/ADC s'initialise avec un **état indéterminé** (memoire `[[tac_reset_required_after_boot]]`)
5. **Conséquence empirique** : samples ADC = strictement 0 ou bruit aléatoire

Pour que les TAC5212 fonctionnent correctement, il faut leur faire un **reset I2C software** **APRÈS** que SAI7 ait commencé à émettre BCLK.

#### 8.3.2 La solution kernel-side : `needs_reset` flag

Dans `tac5212.c` (kernel driver), au probe I2C :

```c
priv->needs_reset = true;  /* ligne 1150 */
```

Puis dans `hw_params` (appelé à la 1re ouverture de stream, donc BCLK déjà actif depuis SAI7 setup) :

```c
if (priv->needs_reset) {
    unsigned int saved_cfg4;
    priv->needs_reset = false;
    regmap_read(priv->regmap, TAC5212_INTF_CFG4, &saved_cfg4);
    saved_cfg4 &= (TAC5212_PDM_CH1_SEL | TAC5212_PDM_CH2_SEL);

    regmap_write(priv->regmap, TAC5212_SW_RESET, TAC5212_SW_RESET_BIT);
    msleep(2);
    regmap_write(priv->regmap, TAC5212_DEV_MISC_CFG,
                 TAC5212_SLEEP_ENZ | TAC5212_SLEEP_EXIT_VREF_EN);
    msleep(10);
    /* ... full re-initialization with BCLK present ... */
    regmap_write(priv->regmap, TAC5212_CLK_CFG2, TAC5212_AUTO_PLL_FR_ALLOW);
    /* Wait for PLL to lock on new BCLK frequency, then clear errors */
}
```

Le PLL interne du TAC verrouille sur BCLK (auto-locks) après ce reset. À partir de là, l'ADC produit des samples valides.

#### 8.3.3 La solution userspace : `tac-reset` systemd service

En plus du flag kernel, le projet expose un **script shell `/usr/bin/tac-reset`** (recipe `tac5212-service`) qui fait un reset complet via i2cset, garantissant que **tous les 4 TACs** sont initialisés au boot, **avant** que mixer-pro ouvre les streams.

```bash
#!/bin/sh
# tac-reset — reset sequential des 4 TACs sur le bus TDM partagé
# Avec BCLK continu (mode ASYNC SOF), reset simultané causerait
# contention DOUT. Reset séquentiel par TAC.

MODE=${1:-analog}
BUS=3
ADDRS="0x50 0x51 0x52 0x53"

case "$MODE" in
    pdm)    CFG4=0x8C ;;
    analog) CFG4=0x0C ;;
esac

for addr in $ADDRS; do
    BASE=$(( ($addr - 0x50) * 2 ))
    SLOT1=$(( 0x20 | $BASE ))      # 0x20, 0x22, 0x24, 0x26
    SLOT2=$(( 0x20 | $BASE + 1 ))  # 0x21, 0x23, 0x25, 0x27
    KEEPER=0x40
    [ "$addr" = "0x50" ] && KEEPER=0x48   # TAC0 = bus keeper

    # 1. Software reset
    i2cset -f -y $BUS $addr 0x01 0x01
    usleep 50000

    # 2. Exit sleep mode (power up VREF)
    i2cset -f -y $BUS $addr 0x02 0x09
    usleep 50000

    # 3. Interface config TDM
    i2cset -f -y $BUS $addr 0x10 0x51   # INTF_CFG1
    i2cset -f -y $BUS $addr 0x11 0x80   # INTF_CFG2
    i2cset -f -y $BUS $addr 0x18 0x40   # ASI_CFG0
    i2cset -f -y $BUS $addr 0x04 0x40   # MISC_CFG

    # 4. TX config : TX_FILL=1 (zeros pendant slots non assignés)
    #    BEFORE setting slots → critique sinon contention DOUT
    i2cset -f -y $BUS $addr 0x1b $KEEPER   # PASI_TX_CFG0 (KEEPER actif sauf TAC0)
    i2cset -f -y $BUS $addr 0x1c 0x01      # PASI_TX_CFG1

    # 5. RX config
    i2cset -f -y $BUS $addr 0x26 0x01      # PASI_RX_CFG0

    # 6. GPIO/PDM
    i2cset -f -y $BUS $addr 0x0c 0x41      # GPO1_CFG0
    i2cset -f -y $BUS $addr 0x0d 0x02      # GPI_CFG
    i2cset -f -y $BUS $addr 0x13 $CFG4     # INTF_CFG4 (0x0C analog, 0x8C PDM)

    # 7. TDM format
    i2cset -f -y $BUS $addr 0x1a 0x30      # PASI_CFG0 (TDM 32-bit)

    # 8. Slot assignments (TX OFFSET et RX OFFSET pour CH1, CH2)
    i2cset -f -y $BUS $addr 0x1e $SLOT1    # PASI_TX_CH1_CFG
    i2cset -f -y $BUS $addr 0x1f $SLOT2    # PASI_TX_CH2_CFG
    i2cset -f -y $BUS $addr 0x28 $SLOT1    # PASI_RX_CH1_CFG
    i2cset -f -y $BUS $addr 0x29 $SLOT2    # PASI_RX_CH2_CFG

    # 9. Clock config (CLK_CFG2 : auto PLL fractional)
    i2cset -f -y $BUS $addr 0x34 0x40

    # 10. Channel enable + power up
    i2cset -f -y $BUS $addr 0x76 0xCC      # CH_EN (IN1, IN2, OUT1, OUT2 enabled)
    i2cset -f -y $BUS $addr 0x78 0xE0      # PWR_CFG (ADC, DAC, MICBIAS power on)

    # 11. Wait PLL lock for THIS TAC (BCLK present → ~10 ms lock time)
    usleep 500000
    i2cget -f -y $BUS $addr 0x3c           # Read CLK_ERR_STS0 (clear latched errors)
    i2cget -f -y $BUS $addr 0x3d           # Read CLK_ERR_STS1
done
```

Le service systemd `tac-reset.service` est lancé **avant** mixer-pro :

```ini
[Unit]
Description=Initialize TAC5212 codecs after boot
After=systemd-modules-load.service sound.target
Before=mixer-pro.service

[Service]
Type=oneshot
ExecStart=/usr/bin/tac-reset analog
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
```

### 8.4 Pourquoi le reset doit être séquentiel (et pas parallèle)

Memory `[[sof_async_solution]]` + commentaire `tac-reset.sh:5` :

> With continuous BCLK (SOF ASYNC mode), resetting all TACs simultaneously causes DOUT bus contention. Reset one TAC at a time instead.

**Raison** : pendant le reset, le TAC reconfigure ses TX_OFFSET. Si plusieurs TACs sont en train de switcher leurs slot ownership simultanément, **2 TACs peuvent piloter DOUT en même temps** → court-circuit logique (contention) → bruit, oscillation, parfois damage.

**Reset séquentiel** garantit qu'à chaque instant, **un seul TAC** est en cours de reconfig, les autres sont en mode normal (slot ownership stable).

### 8.5 PLL TAC interne — détails

Chaque TAC5212 a un **PLL interne** qui multiplie BCLK pour générer les clocks ADC/DAC internes. Configuration via `CLK_CFG2` :

| Bit | Mode |
|---|---|
| 0 | PLL bypass (utiliser BCLK direct, sample rate fixe) |
| 1 | PLL enabled, source = BCLK, auto fractional |
| 2 | PLL enabled, source = MCLK externe (non utilisé sur Debix Model AB) |

Le projet utilise **`TAC5212_AUTO_PLL_FR_ALLOW`** = PLL enabled + auto fractional → lock automatique sur n'importe quel BCLK supporté (1-50 MHz). 

Si BCLK transitoirement coupe (BCLK glitch < quelques ms), le PLL **perd la lock**, et il faut quelques ms après retour BCLK pour re-locker. Pendant ce temps : **bruit blanc** sur ADC, **silence** sur DAC. C'est exactement ce qui se passe quand on stoppe TX SAI sans précaution (cf §7.2).

### 8.6 Chaîne d'effets TAC dans le pipeline V7.0

Memory `[[tac5212_effects_in_chain]]` :

```
Capture path :
mic → [TAC ADC : AGC → HPF → biquads × N → gain → decim → digital ch mix] → SAI7 RX
    → [DSP cap : multiband_drc → pga] → PCM 0 cap
    → [mixer-pro userspace]

Playback path :
[mixer-pro userspace] → PCM 1 play
                     → [DSP play : multiband_drc → pga] → SAI7 TX
                     → [TAC DAC : interp filter → biquads × N → DRC → gain
                                  → limiter+foldback → battery guard] → speaker
```

**Le TAC n'est PAS un convertisseur transparent**. Tous ses effets internes font partie de la chaîne audio V7.0, et le NPU doit pouvoir agir sur l'**ensemble unifié** (TAC + DSP). Source : datasheet SLASF23A § 7.1, p. 28.

### 8.7 Kcontrols ALSA exposés (mixer-gui-http)

Le driver `tac5212.c` expose les paramètres via kcontrols ALSA :

```bash
$ amixer -c softac5212tdm controls | head -20
numid=1,iface=MIXER,name='TAC5212-0 ADC1 Volume'
numid=2,iface=MIXER,name='TAC5212-0 ADC2 Volume'
numid=3,iface=MIXER,name='TAC5212-0 DAC1 Volume'
numid=4,iface=MIXER,name='TAC5212-0 DAC2 Volume'
numid=5,iface=MIXER,name='TAC5212-0 Biquad 1 Coefficients'  (blob, 24 biquads)
numid=6,iface=MIXER,name='TAC5212-0 Biquad 2 Coefficients'
...
numid=N,iface=MIXER,name='TAC5212-0 AGC Config'              (blob, paged)
numid=N+1,iface=MIXER,name='TAC5212-0 DRC Config'            (blob, paged)
numid=N+2,iface=MIXER,name='TAC5212-0 HPF Config'            (blob, paged)
```

Au total **339 kcontrols** pour 4 TACs (memory `[[sof_e5e1_optionFpp_validated]]`). Le mixer-gui-http les expose en REST sous `/api/alsa/contents` (liste) et `/api/dsp/blob/<numid>/raw` (lecture/écriture blob).

---

## 9. NPU TAP — architecture dual-tap pour ingé son ML

### 9.1 Vision du système

L'objectif final du projet est un **ingé son automatique par NPU** : un modèle ML qui analyse l'audio en temps réel et ajuste **automatiquement** les paramètres du mixer (gains, EQ, DRC, send FX) pour optimiser la qualité subjective.

Pour que le NPU puisse "écouter" le signal, il a besoin d'accéder à 2 points stratégiques de la chaîne :

| Tap | Position | Quoi | Pourquoi |
|---|---|---|---|
| **tap-in** | SAI7 RX → DSP cap raw (pre-multiband_drc) | Signal mic brut, après ADC TAC mais avant tout traitement DSP | Permet au NPU de voir le signal "non altéré" pour décisions a priori (détecter clipping, bruit, voix vs musique) |
| **tap-out** | DSP play post-effets, pre-SAI7 TX | Signal speaker final, après tous les FX | Permet au NPU de monitorer le résultat final pour boucle de feedback (vérifier qualité, ajuster paramètres) |

### 9.2 Architecture mémoire — 2 zones DRAM partagées

Deux **reserved memory carves** dans le device tree, chacune 256 KB :

```
DRAM physique addresses :
  0x94270000 - 0x942AFFFF  (256 KB)  : tap-in buffer
  0x942B0000 - 0x942EFFFF  (256 KB)  : tap-out buffer
```

Ces adresses sont **fixes** et déclarées dans 2 endroits :
1. **Yocto** : `meta-local/recipes-kernel/imx-audio-tap/files/imx-audio-tap-uapi.h`
2. **SOF firmware** : `sof/src/include/sof/audio/npu_tap.h`

Cross-check au build par `do_configure:prepend` (cf §3.3.2).

### 9.3 Device Tree carve out (apply-npu-tap-dt.py)

Le script `apply-npu-tap-dt.py` patche `imx8mp-evk.dts` pour ajouter :

```dts
/ {
    reserved-memory {
        #address-cells = <2>;
        #size-cells = <2>;
        ranges;

        npu_tap_in_buffer: tap_in_buffer@94270000 {
            no-map;
            reg = <0x0 0x94270000 0x0 0x40000>;
        };

        npu_tap_out_buffer: tap_out_buffer@942B0000 {
            no-map;
            reg = <0x0 0x942B0000 0x0 0x40000>;
        };
    };

    imx_audio_tap_in {
        compatible = "electrosens,imx-audio-tap";
        memory-region = <&npu_tap_in_buffer>;
        device-name = "imx-audio-tap-in";
    };

    imx_audio_tap_out {
        compatible = "electrosens,imx-audio-tap";
        memory-region = <&npu_tap_out_buffer>;
        device-name = "imx-audio-tap-out";
    };
};
```

`no-map` = la mémoire n'est PAS mappée dans le address space kernel par défaut. Elle est réservée et accessible uniquement par le DSP (cacheattr write-through) + via mmap utilisateur.

### 9.4 Header partagé — npu_tap_hdr layout

Le layout du header (128 B, exactement 1 cache line HiFi4) :

```c
struct npu_tap_hdr {
    uint32_t magic;          /* @0   : NPU_TAP_MAGIC = 0x5441504E "NPAT", écrit en DERNIER */
    uint32_t version;        /* @4   : 5 (V7.0-E4 dual-tap) */
    uint32_t ring_size;      /* @8   : taille ring runtime (multiple period_bytes) */
    uint32_t hdr_size;       /* @12  : 128 (= NPU_TAP_HDR_SIZE) */
    uint32_t epoch;          /* @16  : counter de seqcount (R1 — DSP increments par dai_common_params) */
    uint32_t write_idx;      /* @20  : DSP writer (wrap mod ring_size) */
    uint32_t read_idx;       /* @24  : A53 reader (wrap mod ring_size) */
    uint32_t period_bytes;   /* @28  : 3072 nominal (8ch × 4B × 96 frames @ 2 ms) */
    uint32_t sample_rate;    /* @32  : 48000 */
    uint32_t channels;       /* @36  : 8 */
    uint32_t frame_fmt;      /* @40  : SOF_IPC_FRAME_S32_LE */
    uint32_t direction;      /* @44  : 0=playback (tap-out), 1=capture (tap-in) */
    uint32_t reserved[18];   /* @48..@124 : zero-init, padding 128 B (M2) */
} __attribute__((packed, aligned(128)));
```

Layout du buffer complet :

```
+----------------------------------+ offset 0x0000
|        npu_tap_hdr (128 B)        |
+----------------------------------+ offset 0x0080
|                                  |
|       ring data (262016 B)        |  ← multiple de 3072 (period_bytes)
|       = 85 periods × 2 ms         |
|       = 170 ms d'audio max         |
|                                  |
+----------------------------------+ offset 0x40000 (256 KB)
```

### 9.5 Pattern publication DSP (R3 canonique, smp_store_release)

Quand le DSP a une nouvelle period prête à publier :

```c
void npu_tap_publish(phys_addr_t base, const void *period_data,
                     size_t period_bytes, int direction)
{
    struct npu_tap_hdr *hdr = (struct npu_tap_hdr *)base;
    uint8_t *ring_data = (uint8_t *)base + NPU_TAP_HDR_SIZE;
    uint32_t wi, new_wi;

    /* 1. Invalidate magic during the publish (signals "in flux") */
    hdr->magic = 0;
    dcache_writeback_region(&hdr->magic, sizeof(hdr->magic));

    /* 2. Compute next write_idx */
    wi = hdr->write_idx;
    new_wi = (wi + period_bytes) % hdr->ring_size;

    /* 3. Copy period data into ring */
    if (wi + period_bytes <= hdr->ring_size) {
        /* Contiguous copy */
        memcpy(ring_data + wi, period_data, period_bytes);
    } else {
        /* Wrap split */
        size_t part1 = hdr->ring_size - wi;
        memcpy(ring_data + wi, period_data, part1);
        memcpy(ring_data, period_data + part1, period_bytes - part1);
    }
    dcache_writeback_region(ring_data + wi, period_bytes);

    /* 4. Update write_idx (release semantics via dcache writeback + barrier) */
    hdr->write_idx = new_wi;
    dcache_writeback_region(&hdr->write_idx, sizeof(hdr->write_idx));

    /* 5. Increment epoch (seqcount counter, for atomic read consistency) */
    hdr->epoch++;
    dcache_writeback_region(&hdr->epoch, sizeof(hdr->epoch));

    __asm__ volatile("memw" ::: "memory");   /* HiFi4 memory write barrier */

    /* 6. Publish magic LAST — readers wait for this to know hdr is valid */
    hdr->magic = NPU_TAP_MAGIC;
    dcache_writeback_region(&hdr->magic, sizeof(hdr->magic));
    __asm__ volatile("memw" ::: "memory");
}
```

### 9.6 Pattern lecture A53 (R4 seqcount-style)

Côté userspace (A53), avec mmap PROT_READ writecombine :

```c
int read_npu_tap(int fd, void *out_buf, size_t out_size, uint32_t *last_epoch)
{
    void *base = mmap(NULL, NPU_TAP_RING_SIZE, PROT_READ, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) return -1;

    const struct npu_tap_hdr *hdr = base;
    const uint8_t *ring_data = (uint8_t *)base + NPU_TAP_HDR_SIZE;

    /* Step 1 : wait for valid magic (boot handshake) */
    uint32_t magic;
    do {
        magic = atomic_load_explicit((_Atomic uint32_t *)&hdr->magic, memory_order_acquire);
    } while (magic != NPU_TAP_MAGIC);

    /* Step 2 : seqcount-style read */
    uint32_t e1, e2, w;
    do {
        e1 = atomic_load_explicit((_Atomic uint32_t *)&hdr->epoch, memory_order_acquire);
        w  = atomic_load_explicit((_Atomic uint32_t *)&hdr->write_idx, memory_order_acquire);

        /* Copy data from ring at offset = (w - read_size) % ring_size */
        size_t rs = hdr->ring_size;
        size_t to_read = MIN(out_size, hdr->period_bytes);
        size_t start = (w - to_read + rs) % rs;
        if (start + to_read <= rs) {
            memcpy(out_buf, ring_data + start, to_read);
        } else {
            size_t part1 = rs - start;
            memcpy(out_buf, ring_data + start, part1);
            memcpy(out_buf + part1, ring_data, to_read - part1);
        }

        e2 = atomic_load_explicit((_Atomic uint32_t *)&hdr->epoch, memory_order_acquire);
    } while (e1 != e2);   /* retry if writer raced us */

    if (e1 != *last_epoch) {
        *last_epoch = e1;   /* new epoch detected (e.g. DSP restarted) */
    }
    return 0;
}
```

**Le pattern garantit la cohérence sans lock** : si write_idx change pendant qu'on lit, e1 != e2 et on retry.

### 9.7 Driver kernel imx-audio-tap

Voir code complet §5.4. Points clés :

1. **probe via DT** (`compatible = "electrosens,imx-audio-tap"`)
2. **Récupère adresse physique via DT** (`memory-region` phandle), pas hardcoded
3. **Crée miscdevice** `/dev/imx-audio-tap-in` ou `/dev/imx-audio-tap-out` (selon `device-name` DT prop)
4. **mmap** retourne `pgprot_writecombine` → write-combine sur ARM64 = Normal Non-Cacheable, ordering relâché mais visibilité garantie via barrière

### 9.8 Userspace consumers

Outils userspace fournis pour lire les taps :

```bash
# meta-local/audio-tools/
npu_tap_reader.c       # Lecteur C ring, validation magic, dump périodes
tap_channel_monitor.py # Monitor Python avec FFT par canal
npu_master_loop.py     # Master loop de l'ingé son (futur, skeleton)
```

`npu_master_loop.py:5` :

> Reads post-effets audio from /dev/imx-audio-tap (mmap shared memory written by SOF firmware via dai_dma_cb hook). The future NPU master loop reads tap data, runs inference, and feeds back commands to mixer-pro via Unix socket /run/mixer-pro.sock.

### 9.9 Pourquoi 2 taps et pas 1

Pourquoi avoir séparé tap-in et tap-out plutôt qu'un seul tap au point central ?

Le NPU doit pouvoir comparer **input** et **output** pour mesurer l'effet des FX appliqués. Si on n'avait qu'un seul tap (par exemple post-effets), le NPU verrait le signal traité mais ne saurait pas quel signal d'entrée a généré ce résultat.

**Cas d'usage typique de l'ingé son ML** :

1. NPU lit tap-in 100 ms → analyse spectrale du signal mic brut
2. NPU lit tap-out 100 ms → analyse du signal speaker final
3. Inference : compare les 2 → décide si le DRC est trop agressif (= mic dynamique compressée tuée)
4. Envoie commande mixer-pro pour ajuster DRC ratio

### 9.10 Invariant — NPU tap non-négociable

Memory `[[project_npu_non_negotiable]]` :

> Le NPU monitoring est non-négociable. L'utilisateur est prêt à refaire SOF complet plutôt que d'abandonner le tap.

Tout choix architectural doit préserver la possibilité de tapping. Notamment :
- Pas de DMA direct DSP→USB (court-circuite le tap)
- Pas de pipeline cross-direction (cap↔play SOF), car break la sentinelle R7
- Période ≥ 2 ms pour avoir assez de samples par publish (1 ms serait trop overhead pour tap)

---

## 10. USB UAC2 gadget — interface DAW

### 10.1 Le besoin

La carte Debix Model AB doit être vue par un PC hôte comme une **carte son professionnelle 8×8**. Cela permet à n'importe quel DAW (Reaper, Ardour, Bitwig, ProTools…) sur le PC d'enregistrer / lire 8 canaux audio simultanément vers/depuis la carte.

### 10.2 Implémentation : USB Audio Class 2 gadget

Linux fournit le driver `f_uac2.c` (drivers/usb/gadget/function/) qui implémente la spécification USB Audio Class 2.0 en mode gadget (carte côté périphérique).

Configuration via configfs (script `usb-uac2-gadget`) :

```bash
#!/bin/sh
GADGET_DIR=/sys/kernel/config/usb_gadget/g1

# 1. Création du gadget
mkdir -p $GADGET_DIR
cd $GADGET_DIR

echo 0x1d6b > idVendor          # Linux Foundation
echo 0x0104 > idProduct         # Multifunction Composite Gadget
echo 0x0100 > bcdDevice
echo 0x0200 > bcdUSB            # USB 2.0

mkdir -p strings/0x409
echo "Electrosens" > strings/0x409/manufacturer
echo "Debix Model AB Audio" > strings/0x409/product
echo "0000000001" > strings/0x409/serialnumber

# 2. Création de la fonction UAC2 (8×8)
mkdir functions/uac2.0
echo 8 > functions/uac2.0/c_chmask         # 0xFF — 8 channels capture (mic input)
echo 8 > functions/uac2.0/p_chmask         # 0xFF — 8 channels playback
echo 48000 > functions/uac2.0/c_srate      # Sample rate capture
echo 48000 > functions/uac2.0/p_srate
echo 4 > functions/uac2.0/c_ssize          # S32_LE
echo 4 > functions/uac2.0/p_ssize

# Period size : 96 frames = 2 ms (= 1 USB SOF group)
echo 96 > functions/uac2.0/req_number

# 3. Configuration USB
mkdir configs/c.1
ln -s functions/uac2.0 configs/c.1/

# 4. Bind to UDC controller
ls /sys/class/udc > UDC
```

Une fois ce script lancé, côté PC hôte :

```
$ lsusb
Bus 003 Device 005: ID 1d6b:0104 Linux Foundation Multifunction Composite Gadget

$ arecord -l
**** List of CAPTURE Hardware Devices ****
card 2: UAC2Gadget [UAC2_Gadget], device 0: UAC2 PCM
  Subdevices: 0/1
  Subdevice #0: subdevice #0
```

### 10.3 Mécanique USB UAC2

USB Audio Class 2 transporte l'audio via **USB Isochronous endpoints** :

- **Endpoint IN** (host→device) : data audio playback du host vers la carte
- **Endpoint OUT** (device→host) : data audio capture de la carte vers le host
- **Microframe SOF** : USB envoie un Start-Of-Frame (SOF) toutes les **1 ms** (USB 2.0) ou 125 µs (USB 3.0). Le data audio est groupé par SOF.

À 48 kHz × 8 ch × 4 bytes = **1536 bytes/ms** envoyés à chaque SOF.

### 10.4 ALSA period vs USB SOF

Le driver f_uac2 expose un PCM ALSA avec period 96 frames = 2 ms. Mais USB SOF est à 1 ms. Donc :

```
ALSA period   :  96 frames     | 96 frames    |
                              ▲              ▲
                              │              │
USB SOF       : SOF1  SOF2    SOF1  SOF2
                |    |        |    |
              48 fr  48 fr   48 fr 48 fr
              (real timing)
```

À chaque SOF, USB transfère ~48 frames (parfois 47 ou 49 selon timing). Sur 2 ms = 2 SOFs = ~96 frames. Le driver f_uac2 accumule les SOFs jusqu'à avoir 1 period (96), puis signale `period_elapsed` à ALSA.

### 10.5 Limitation f_uac2 — pas de HW timestamping

Sur Linux mainline, **f_uac2 ne supporte pas `snd_pcm_status_get_audio_htstamp()`** (memory `[[mixer_pro_v8_33]]`). Le timestamp HW retourne 0.0 systématiquement.

Conséquence : pour mesurer le drift USB↔DSP, mixer-pro doit utiliser un **fallback** :

```c
/* mixer-pro.c — V8.26 fallback drift via CLOCK_MONOTONIC + appl_ptr + avail */
struct timespec now;
clock_gettime(CLOCK_MONOTONIC, &now);
snd_pcm_sframes_t avail_now = snd_pcm_avail(g_st.cap_uac2.pcm);
uint64_t hw_pos_now = drift_samples + (uint64_t)avail_now;

double dt_sec = (now - drift_t0);
double rate = (double)(hw_pos_now - drift_hw_pos_0) / dt_sec;
double ppm = (rate - 48000.0) / 48000.0 * 1e6;
```

C'est un drift mesuré "logiciel" (incrément du nombre de samples disponibles par seconde, comparé à CLOCK_MONOTONIC) — moins précis que HW htstamp mais suffisant à l'ordre du ppm.

### 10.6 Le problème silencieux : USB unplug / suspend

Si le PC hôte met le USB en suspend ou disconnect :

1. f_uac2 cesse d'envoyer/recevoir SOFs
2. mixer-pro reste bloqué sur `snd_pcm_readi(UAC2)` blocking
3. **Tout le pipeline DSP** est gelé en cascade

C'est pour ça que mixer-pro a un **thread dédié** par direction USB (cap + play) qui isole le PCM USB du DSP :

```c
/* V8.1 isolation UAC2 : 1 thread = 1 PCM. Si USB suspend,
 * le thread bloque, mais audio_thread continue sur DSP. */
pthread_create(&cap_uac2_thread, ..., cap_uac2_thread_fn, ...);
pthread_create(&play_uac2_thread, ..., play_uac2_thread_fn, ...);
```

Les threads communiquent avec audio_thread via **ring SPSC lock-free** (single-producer single-consumer) :

```c
static uac2_ring_t g_ring_uac2_cap;   /* USB → audio_thread (cap) */
static uac2_ring_t g_ring_uac2_play;  /* audio_thread → USB (play) */
```

Si USB bloque, le ring se vide (cap) ou se remplit (play), mais le DSP continue à tourner.

---

## 11. Problème fondamental DSP ↔ USB — fréquences indépendantes

### 11.1 Le problème en une phrase

**Deux quartz indépendants ne battent jamais à la même fréquence exacte. Il y a toujours un drift en ppm.**

| Source | Fréquence nominal | Drift typique |
|---|---|---|
| Quartz i.MX 8M Plus (XO 24 MHz) → audiomix PLL → SAI BCLK → 48 kHz | 48 000 Hz exactement | tolérance ±20 ppm spec, ~5 ppm typique |
| Quartz PC hôte (XO 24/25/27/etc. MHz) → USB clock → SOF 1 kHz | 1000 Hz exactement (SOF) → 48 kHz audio nominal | tolérance ±100 ppm spec, ~30-50 ppm typique |

**Différence DSP↔USB** : 1-100 ppm de drift permanent. En audio, c'est énorme : 1 ppm = 1 sample/seconde d'écart sur 48 kHz.

### 11.2 Conséquence en absence de correction

Si on relie le USB directement au DSP sans correction :

- USB producer ramène **48 011 samples/s** (drift +230 ppm)
- DSP consumer prend **48 000 samples/s** (exact)
- Excès : 11 samples/s accumulés

Après 1 minute : 660 samples excédentaires dans le ring buffer. Soit le ring overflow (drop samples = clic), soit le DSP underrun (joue zéros = silence).

### 11.3 La solution standard : ASRC (Asynchronous Sample Rate Converter)

Un **ASRC** prend un stream à la fréquence source (USB) et le ré-échantillonne vers la fréquence destination (DSP) avec un ratio non-entier.

**ASRC matériel** : i.MX 8M Plus a 4 modules ASRC HW dans le SoC. Mais le projet ne les utilise PAS car :
1. Architecture cible = 2 USB (USB DAW + USB téléphone futur). Avec 2 sources USB asynchrones, **aucun ASRC HW unique** ne peut suivre 2 horloges en même temps.
2. Activation ASRC HW = patch kernel + SOF firmware lourd (memory `[[v8-33-suite]]`).

**ASRC logiciel** : mixer-pro implémente un **drop/insert simple** :
- Si drift > 0 (USB plus rapide), on **drop** des samples périodiquement
- Si drift < 0 (USB plus lent), on **insert** des samples interpolés

### 11.4 mixer-pro `compute_correction` — l'algorithme actuel (V8.35)

```c
static int compute_correction(int *samples_acc, int is_play)
{
    if (atomic_load(&g_no_asrc)) return 0;
    int shift = atomic_load(&g_shift_ppm);
    if (shift == 0) return 0;
    /* V8.35 — interval = 500000/|shift| (vs 1000000/|shift| en V8.33).
     * Combiné avec correction ±1 sample (au lieu de ±2), net rate identique
     * mais artefacts 2× moins forts. */
    int interval = 500000 / (shift > 0 ? shift : -shift);
    if (*samples_acc < interval) return 0;
    *samples_acc -= interval;
    if (is_play) return (shift > 0) ? -1 : +1;
    else         return (shift > 0) ? +1 : -1;   /* cap : drop if shift>0 */
}
```

`shift_ppm` est mis à jour dynamiquement par `cap_uac2_thread` à partir de la mesure de drift CLOCK_MONOTONIC :

```c
/* mixer-pro.c — calcul drift et update shift_ppm */
drift_ppm_ema = 0.9f * drift_ppm_ema + 0.1f * ppm;
atomic_store(&g_usb_drift_ppm_x100, (int)(drift_ppm_ema * 100.0f));

int new_shift = (int)(drift_ppm_ema + (drift_ppm_ema >= 0 ? 0.5f : -0.5f));
atomic_store(&g_shift_ppm, new_shift);
```

EMA (exponential moving average) avec α=0.1 lisse le drift mesuré bruité. shift_ppm = round(drift_ppm_ema).

### 11.5 Drop fusion 2-en-1 (V8.35)

Quand correction = +1 (mode cap, drift positif), `input_n = 97`. Le drop fusion :

```c
if (diff == +1) {
    /* V8.35 — Drop 97 → 96 : fusion 2-en-1 unique au centre (pos 48).
     * out[48] = avg(in[48], in[49]). */
    memcpy(period_buf, buf_acc, 48 * UAC2_CH * sizeof(int32_t));
    for (int ch = 0; ch < UAC2_CH; ch++) {
        int64_t a = buf_acc[48 * UAC2_CH + ch];
        int64_t b = buf_acc[49 * UAC2_CH + ch];
        period_buf[48 * UAC2_CH + ch] = (int32_t)((a + b) / 2);
    }
    memcpy(period_buf + 49 * UAC2_CH,
           buf_acc + 50 * UAC2_CH,
           47 * UAC2_CH * sizeof(int32_t));
}
```

97 samples d'entrée → 96 sortie. La fusion remplace `in[48], in[49]` par leur moyenne sur 1 sample. Discontinuité de pente locale = audible si signal haute fréquence, inaudible si signal lent.

### 11.6 Insert symétrique

Quand correction = -1 (drift négatif), `input_n = 95` :

```c
if (diff == -1) {
    /* Insert 95 → 96 : interpolation 2-en-1 au centre (pos 48). */
    memcpy(period_buf, buf_acc, 48 * UAC2_CH * sizeof(int32_t));
    for (int ch = 0; ch < UAC2_CH; ch++) {
        int64_t a = buf_acc[47 * UAC2_CH + ch];
        int64_t b = buf_acc[48 * UAC2_CH + ch];
        period_buf[48 * UAC2_CH + ch] = (int32_t)((a + b) / 2);
    }
    memcpy(period_buf + 49 * UAC2_CH,
           buf_acc + 48 * UAC2_CH,
           47 * UAC2_CH * sizeof(int32_t));
}
```

95 samples → 96 sortie. On insère un sample interpolé moyenne(in[47], in[48]) à la position 48.

### 11.7 Les limites de cette approche

L'ASRC drop/insert simple a 2 défauts inhérents :

**a) Artefact spectral** : la fusion 2-en-1 est un filtre passe-bas brutal au point de fusion. Crée un défaut spectral à la fréquence où elle se produit (~1 Hz pour drift 11 ppm).

**b) Réactivité limitée** : le compute_correction n'agit que quand l'accumulator dépasse le seuil. Variations rapides de drift (USB hub mute/unmute, hôte qui charge le CPU) ne sont pas suivies finement.

Solutions plus avancées :
- **Polyphase FIR resampling** : filtres bank de N filtres FIR, interpolation propre. Plus de CPU.
- **Cubic / sinc interpolation** : meilleure qualité sur le sample interpolé. Plus de CPU.
- **Activer constantes fill-based** : UAC2_FILL_LOW/HIGH/etc. déclarées mais inutilisées (code mort à activer).

### 11.8 Mesure de drift — méthodologie

mixer-pro mesure le drift en continu sur fenêtre 10 sec :

```c
if (dt_sec >= 10.0) {
    uint64_t df = hw_pos_now - drift_hw_pos_0;
    double rate = (double)df / dt_sec;
    double ppm = (rate - (double)SAMPLE_RATE) / (double)SAMPLE_RATE * 1e6;
    /* Gardes : rate dans ±1% et |ppm| < 500 sinon invalidate (USB suspended) */
    if (fabs(rate - SAMPLE_RATE) > 0.01 * SAMPLE_RATE || fabs(ppm) > 500.0) {
        /* Mesure invalide (USB stopped), garder shift_ppm précédent */
    } else {
        drift_ppm_ema = 0.9f * drift_ppm_ema + 0.1f * ppm;
        /* update shift_ppm */
    }
    drift_t0 = now;
    drift_hw_pos_0 = hw_pos_now;
}
```

Mesure typique board V8.33 : drift = +9 à +12 ppm stable, parfois swings à -20 ppm pendant transients DAW.

### 11.9 État des tentatives

| Version | Approche ASRC | Résultat |
|---|---|---|
| V8.0-V8.32 | Multiples itérations drop/insert ±2, ±6, fill-based, etc. | Plusieurs régressions reverted, drift parfois mesuré +554 ppm (irréaliste, bug mesure) |
| V8.33 | drop/insert ±2 fusion 3-en-1, shift dynamic | Glitchs résiduels présents, scale avec \|drift\| |
| V8.34 | + PLC packet loss concealment | PLC marche mais pas la bonne cible (glitchs viennent de cap_full, pas cap_empty). **Reverted** |
| V8.35 | drop/insert ±1 fusion 2-en-1 + interval/2 | En test au moment d'écrire ce doc |

Memory `[[v8-33-suite]]` : pistes futures = compute_correction plus fréquent, smooth interp sinc, fill-based activation, hystérésis tracking.

---

## 12. mixer-pro — daemon C userspace 26×18 + ASRC

### 12.1 Vue d'ensemble

`mixer-pro` est le **coeur userspace** de la console. Daemon C 2412 lignes, single binary, sans dépendance lourde (ALSA + libmicrohttpd uniquement).

```
                      mixer-pro main()
                          │
                          ▼
        ┌─────────────────────────────────────┐
        │   Création threads pthread_create    │
        │   Tous SCHED_FIFO sauf control       │
        └─────────────────────────────────────┘
                          │
        ┌───────┬─────────┼─────────┬──────────┬──────────┐
        ▼       ▼         ▼         ▼          ▼          ▼
   audio   play     cap_uac2   play_uac2   control   analyzer
   thread  thread   thread     thread      thread    thread
   prio80  prio81   prio80     prio80      OTHER     prio60
```

### 12.2 Architecture matrice 26×18

26 sources d'entrée :
- 8 inputs DSP (mics ADC TAC5212)
- 8 inputs UAC2 (DAW → board)
- 2 inputs phone (loopback simulé)
- 8 returns FX (4 bus × 2 ch stéréo)

18 sorties :
- 8 outputs DSP (speakers DAC TAC5212)
- 8 outputs UAC2 (board → DAW)
- 2 outputs phone

Par-source :
- `input_gain[26]` (volume)
- `mute_mask` (bitmask 26)
- `send_gain[26][8]` (level vers 4 bus FX stéréo)

Par-output :
- `master_gain[26][18]` (gain de routage 26 sources × 18 outputs)

```c
/* mixer-pro.h excerpt */
#define N_INPUT_MICS    8      /* DSP TAC5212 cap */
#define N_INPUT_STEMS   8      /* UAC2 in (DAW PC) */
#define N_INPUT_PHONE   2      /* Phone aloop in */
#define N_INPUT_REAL    18     /* 8 + 8 + 2 */
#define N_BUS_FX        4      /* 4 bus stéréo */
#define N_BUS_FX_CH     8      /* 4 × 2 ch */
#define N_RETURN_CH     8      /* 4 returns stéréo */
#define N_INPUT_TOTAL   26     /* 18 + 8 returns */
#define N_OUTPUT_DSP    8
#define N_OUTPUT_UAC2   8
#define N_OUTPUT_PHONE  2
#define N_OUTPUT_TOTAL  18
#define SAMPLE_RATE     48000
#define PERIOD_FRAMES   96     /* 2 ms @ 48 kHz */
```

### 12.3 Le boucle audio principale (audio_thread)

```c
static void *audio_thread(void *arg)
{
    /* RT priority */
    struct sched_param sp = { .sched_priority = RT_PRIO_AUDIO };
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);

    /* Pre-allocate buffers */
    int32_t cap_dsp_buf  [PERIOD_FRAMES * N_INPUT_MICS];
    int32_t cap_uac2_buf [PERIOD_FRAMES * N_INPUT_STEMS];
    int32_t cap_phone_buf[PERIOD_FRAMES * N_INPUT_PHONE];
    int32_t play_dsp_buf [PERIOD_FRAMES * N_OUTPUT_DSP];
    int32_t play_uac2_buf[PERIOD_FRAMES * N_OUTPUT_UAC2];
    int32_t play_phone_buf[PERIOD_FRAMES * N_OUTPUT_PHONE];

    /* Self-paced clock_nanosleep ABSTIME 2 ms */
    struct timespec t_next;
    clock_gettime(CLOCK_MONOTONIC, &t_next);
    const long PERIOD_NS = 2000000L;

    snd_pcm_start(g_st.cap_dsp.pcm);

    while (atomic_load(&g_st.running)) {
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t_next, NULL);
        t_next.tv_nsec += PERIOD_NS;
        normalize(&t_next);

        clock_gettime(CLOCK_MONOTONIC, &t_iter_start);

        /* 1. DSP cap = horloge maître (blocking read) */
        r = snd_pcm_readi(g_st.cap_dsp.pcm, cap_dsp_buf, PERIOD_FRAMES);
        if (r < 0) { pcm_recover(g_st.cap_dsp.pcm, r); memset(cap_dsp_buf, 0, sizeof(cap_dsp_buf)); }

        /* 2. UAC2 cap : pop du ring SPSC (alimenté par cap_uac2_thread) */
        if (g_skip_uac2) memset(cap_uac2_buf, 0, sizeof(cap_uac2_buf));
        else             uac2_ring_pop_period(&g_ring_uac2_cap, cap_uac2_buf);

        /* 3. Phone cap NONBLOCK */
        ...

        /* 4. Conversion S32 → float, matrix mix 26×18 → float out */
        for (int f = 0; f < PERIOD_FRAMES; f++) {
            float in[N_INPUT_REAL];
            float out[N_OUTPUT_TOTAL];
            /* fill in[] from cap_dsp/uac2/phone */
            ...
            apply_matrix_mix(in, out);
            /* split out[] to play_dsp/uac2/phone */
            ...
        }

        /* 5. Push to play_thread via ring SPSC */
        push_ring(&g_st.ring_buf, play_dsp_buf);

        /* 6. UAC2 play : push via play_uac2 ring */
        if (!g_skip_uac2) uac2_ring_push_period(&g_ring_uac2_play, play_uac2_buf);

        /* 7. Phone play NONBLOCK */
        ...

        clock_gettime(CLOCK_MONOTONIC, &t_play_done);
        prof_iter_us = elapsed(t_iter_start, t_play_done);
    }
    return NULL;
}
```

**Self-paced** via `clock_nanosleep ABSTIME` : la boucle se réveille exactement toutes les 2 ms (V8.33). Empêche le busy-loop apparu après isolcpus=2,3 où le thread tournait à fond sur core 2 sans rate limiting.

### 12.4 Ring SPSC lock-free (audio→play_thread)

Le ring buffer entre `audio_thread` (producer) et `play_thread` (consumer) utilise des indices atomiques avec acquire/release :

```c
/* mixer-pro.h */
#define N_RING_PERIODS  8
#define RING_FRAMES    (PERIOD_FRAMES * N_RING_PERIODS)   /* 8 × 96 = 768 frames = 16 ms */

/* mixer-pro.c — struct state */
struct state {
    int32_t ring_buf[RING_FRAMES * N_OUTPUT_DSP];
    atomic_uint ring_write_idx;
    atomic_uint ring_read_idx;
    atomic_ulong ring_drops;
    ...
};

/* Push (in audio_thread) */
unsigned wi = atomic_load_explicit(&g_st.ring_write_idx, memory_order_relaxed);
unsigned ri = atomic_load_explicit(&g_st.ring_read_idx,  memory_order_acquire);
unsigned avail = wi - ri;
if (avail + PERIOD_FRAMES > RING_FRAMES) {
    atomic_fetch_add(&g_st.ring_drops, PERIOD_FRAMES);  /* Drop period */
} else {
    /* Copy 96 frames × 8 ch */
    for (int f = 0; f < PERIOD_FRAMES; f++) {
        unsigned slot = (wi + f) % RING_FRAMES;
        memcpy(&g_st.ring_buf[slot * N_OUTPUT_DSP], &play_dsp_buf[f * N_OUTPUT_DSP],
               N_OUTPUT_DSP * sizeof(int32_t));
    }
    atomic_store_explicit(&g_st.ring_write_idx, wi + PERIOD_FRAMES, memory_order_release);
}

/* Pop (in play_thread) */
unsigned wi = atomic_load_explicit(&g_st.ring_write_idx, memory_order_acquire);
unsigned ri = atomic_load_explicit(&g_st.ring_read_idx,  memory_order_relaxed);
unsigned avail = wi - ri;
if (avail >= PERIOD_FRAMES) {
    /* Copy + writei to DSP play PCM */
    for (int f = 0; f < PERIOD_FRAMES; f++) {
        unsigned slot = (ri + f) % RING_FRAMES;
        memcpy(&local_buf[f * N_OUTPUT_DSP], &g_st.ring_buf[slot * N_OUTPUT_DSP],
               N_OUTPUT_DSP * sizeof(int32_t));
    }
    snd_pcm_writei(g_st.play_dsp.pcm, local_buf, PERIOD_FRAMES);
    atomic_store_explicit(&g_st.ring_read_idx, ri + PERIOD_FRAMES, memory_order_release);
}
```

**Tradeoff buffer size** : 8 periods × 2 ms = 16 ms de tolérance jitter, latence sortie ajoutée 8-16 ms selon fill. Memory `[[sof_v322_baseline]]` confirme 8 periods comme baseline.

### 12.5 Pourquoi un play_thread dédié

Pourquoi pas écrire directement le DSP play depuis audio_thread ?

Parce que `snd_pcm_writei` blocking peut prendre **5-60 ms en recover** lors d'un xrun SOF (SOF IPC roundtrip + DMA restart). Si on bloque dans audio_thread, on perd les samples cap qui arrivent pendant ce temps → cascade d'xruns.

Le `play_thread` isole le blocking : si writei bloque 60 ms, audio_thread continue son boulot (cap + mix), accumule dans le ring, et play_thread reprend quand il peut.

```c
static void *play_thread(void *arg)
{
    struct sched_param sp = { .sched_priority = RT_PRIO_AUDIO + 1 };  /* prio 81 */
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    ...
    while (atomic_load(&g_st.running)) {
        /* Wait pour event sur ring (eventfd ou poll) */
        ...
        /* Pop 1 period et writei */
        ...
    }
}
```

### 12.6 cap_uac2_thread — isolation USB

```c
static void *cap_uac2_thread(void *arg)
{
    struct sched_param sp = { .sched_priority = RT_PRIO_AUDIO };
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    mlog("cap_uac2_thread : SCHED_FIFO prio %d (NONBLOCK)", RT_PRIO_AUDIO);

    int32_t buf_acc[(BUFFER_FRAMES + 1) * UAC2_CH];   /* 384+1 frames accumulator */
    int acc_n = 0;
    snd_pcm_nonblock(g_st.cap_uac2.pcm, 1);

    while (atomic_load(&g_st.running)) {
        /* 1. Combien de samples USB disponibles ? */
        snd_pcm_sframes_t avail = snd_pcm_avail_update(g_st.cap_uac2.pcm);
        if (avail < 0 && avail != -EAGAIN) {
            /* xrun recover */
            atomic_fetch_add(&g_ring_uac2_cap.xruns, 1);
            snd_pcm_recover(g_st.cap_uac2.pcm, (int)avail, 1);
            snd_pcm_start(g_st.cap_uac2.pcm);
            acc_n = 0;
            usleep(200);
            continue;
        }
        if (avail <= 0) {
            usleep(200);
            goto cap_drift_calc;
        }

        /* 2. Lire ce qui rentre dans buf_acc */
        int space = BUFFER_FRAMES - acc_n;
        int n_to_read = MIN(avail, space);
        if (n_to_read <= 0) goto cap_drift_calc;
        snd_pcm_sframes_t r = snd_pcm_readi(g_st.cap_uac2.pcm,
            buf_acc + acc_n * UAC2_CH, n_to_read);
        if (r > 0) acc_n += r;

        /* 3. Push periods 96 frames vers ring SPSC */
        while (acc_n >= PERIOD_FRAMES + 2) {
            int correction = compute_correction(&corr_samples_acc, 0);
            int input_n = PERIOD_FRAMES + correction;   /* V8.35 : 95/96/97 */
            int32_t period_buf[PERIOD_FRAMES * UAC2_CH];
            /* ASRC drop/insert si diff != 0 */
            ...
            /* Push atomique 96 frames vers ring */
            if (!uac2_ring_try_push_period(&g_ring_uac2_cap, period_buf))
                break;   /* ring full, keep buf_acc, retry next iter */
            /* Shift buf_acc */
            memmove(buf_acc, buf_acc + input_n * UAC2_CH,
                    (acc_n - input_n) * UAC2_CH * sizeof(int32_t));
            acc_n -= input_n;
        }

      cap_drift_calc:
        /* Calcul drift via CLOCK_MONOTONIC + appl_ptr + avail */
        ...
    }
    return NULL;
}
```

**Atomic push 96** : `uac2_ring_try_push_period` push EXACTEMENT 96 frames ou échoue (memory `[[push-atomic-96]]`). Pas de tronquage silencieux.

```c
static int uac2_ring_try_push_period(uac2_ring_t *r, const int32_t *in)
{
    unsigned wi = atomic_load_explicit(&r->wr, memory_order_relaxed);
    unsigned ri = atomic_load_explicit(&r->rd, memory_order_acquire);
    unsigned used = wi - ri;
    unsigned free_space = UAC2_RING_FRAMES - used;

    if (free_space < PERIOD_FRAMES) {
        atomic_fetch_add(&r->drops_evt, 1);
        return 0;   /* Ring full, rien écrit */
    }
    /* Copy 96 frames × 8 ch */
    for (unsigned f = 0; f < PERIOD_FRAMES; f++) {
        unsigned slot = (wi + f) % UAC2_RING_FRAMES;
        memcpy(&r->buf[slot * UAC2_CH], &in[f * UAC2_CH],
               UAC2_CH * sizeof(int32_t));
    }
    atomic_store_explicit(&r->wr, wi + PERIOD_FRAMES, memory_order_release);
    return 1;
}
```

### 12.7 control_thread — Unix socket IPC

```c
static void *control_thread(void *arg)
{
    /* /run/mixer-pro.sock — JSON line-based */
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    bind(fd, MIXER_SOCK_PATH);
    listen(fd, 8);

    while (atomic_load(&g_st.running)) {
        int cli = accept(fd, NULL, NULL);
        char buf[4096];
        int n = read(cli, buf, sizeof(buf));
        /* Parse op JSON, exécute (set_master, set_send, mute, reset_fx, ...) */
        char reply[1024];
        process_command(buf, reply);
        write(cli, reply, strlen(reply));
        close(cli);
    }
    return NULL;
}
```

API JSON line-based :

```json
{"op":"set_master","in_id":3,"out_id":0,"gain":0.5}
{"op":"set_send","in_id":3,"bus":1,"gain":0.3}
{"op":"set_mute","in_id":3,"mute":1}
{"op":"reset_fx","bus":0}
{"op":"reset_drift_stats"}
{"op":"set_fx_bus","bus":1,"engine":"reverb"}
{"op":"get_state"}
```

### 12.8 Effects engine — 4 bus stéréo

`effects.c` (1450 lignes) implémente 4 engines :

```c
typedef struct fx_engine {
    enum fx_type type;   /* FX_COMP, FX_REVERB, FX_DELAY, FX_EQ */
    void (*process)(struct fx_engine *eng, float in_l, float in_r,
                    float *out_l, float *out_r);
    /* Per-engine state */
    union {
        comp_state_t   comp;
        reverb_state_t reverb;
        delay_state_t  delay;
        eq_state_t     eq;
    };
} fx_engine_t;
```

Engines sample-précis (process appelé pour chaque sample), state per-engine. Routing via `send_gain[in][bus]` (level vers bus FX), retour des 8 returns dans la matrix master.

### 12.9 analyzer_thread — FFT taps

```c
/* mixer-pro.h */
#define N_TAPS         4        /* 4 GUI analyzer slots */
#define TAP_FFT_N      1024     /* ~21 ms @ 48 kHz, 23 Hz/bin */
#define TAP_BINS_OUT   128      /* downsampled mag 4:1 peak hold */
#define TAP_SCOPE_N    64       /* stereo X-Y scope */
#define ANALYZER_PERIOD_US 33000   /* 30 Hz refresh */
#define RT_PRIO_ANALYZER  60

typedef enum {
    TAP_KIND_NONE     = 0,
    TAP_KIND_INPUT    = 1,
    TAP_KIND_BUS_PRE  = 2,
    TAP_KIND_OUTPUT   = 3,
} tap_kind_t;
```

4 taps GUI configurables runtime, FFT radix-2 1024 inline, 128 bins dB output, 64 stereo scope pairs. Implémenté dans `analyzer.c` (~700 lignes).

### 12.10 Statistiques temps réel (V8.32 timing GUI)

mixer-pro expose en continu des stats timing dans la topbar GUI :

| Stat | Source | Cible |
|---|---|---|
| **wr min/avg10s/max** | Intervalle entre 2 push successifs vers `g_ring_uac2_cap` | ~2000 µs |
| **rd min/avg10s/max** | Intervalle entre 2 pop successifs depuis `g_ring_uac2_cap` | ~2000 µs |
| **cap_full / cap_empty** | Events ring USB cap saturé / vide | 0 idéal |
| **xruns cap / play** | Recovers ALSA sur readi/writei UAC2 | 0 idéal |
| **drift_ppm** | Mesure CLOCK_MONOTONIC + appl_ptr (USB↔CLOCK) | 0 idéal |
| **shift_ppm** | Correction ASRC appliquée | mirroir drift |
| **prof_iter_us** | Durée audio_thread iter | < 2000 µs |
| **ring_fill_frames** | Niveau ring SPSC DSP | < 384 |

Affichage GUI :

```
TopBar : wr 439/1999/3544 µs  |  rd 595/1999/5959 µs  |  drift +11 ppm
         shift +11  |  xrun 0  |  cap_full 12  |  cap_empty 2
```

Bouton **Reset Stats** (V8.30) reset min/max/buckets/events sans relancer le daemon.

### 12.11 Code organization

| Fichier | Lignes | Rôle |
|---|---|---|
| `mixer-pro.c` | 2412 | main, audio_thread, play_thread, cap_uac2_thread, play_uac2_thread, control_thread, ASRC, ring SPSC, IPC handler |
| `mixer-pro.h` | 109 | Constantes (PERIOD_FRAMES, N_RING_PERIODS, RT_PRIO_*, …), enums, structs publics |
| `effects.c` | ~1450 | FX engines (comp, reverb, delay, eq) |
| `effects.h` | ~100 | fx_engine_t struct, factory fn |
| `analyzer.c` | ~700 | FFT taps (4× SPSC ring + radix-2 1024) |
| `analyzer.h` | ~80 | tap_kind_t, analyzer state |
| `Makefile` | ~30 | gcc -O2 -lasound -lmicrohttpd -lpthread -lm |
| `mixer-pro.service` | ~25 | systemd unit, RT permissions, CPUAffinity=2 3 |

---

## 13. Stabilité temps réel — isolcpus, CPUAffinity, scheduling

### 13.1 Le problème de l'audio temps réel sur Linux générique

Linux scheduler "completely fair scheduler" (CFS) garantit la fair-share du CPU entre tous les tâches. Excellent pour batch / interactive workloads, **catastrophe pour audio temps réel** : un thread audio qui doit s'exécuter toutes les 2 ms peut être préempté 5-50 ms par une tâche concurrente.

Solutions Linux RT :

1. **SCHED_FIFO priority** : thread devient prioritaire absolu vs SCHED_OTHER
2. **CONFIG_PREEMPT** : kernel préemptible au-delà des sections critiques
3. **CONFIG_PREEMPT_RT** : kernel fully preemptible (rt-tasks préemptent même les IRQs)
4. **mlockall** : verrouille la mémoire process pour éviter swap-in latency
5. **isolcpus** : retire des cores du scheduler général, dédiés à des tâches choisies
6. **CPUAffinity** : pin un thread à un set de cores spécifiques

### 13.2 Stack RT du projet (V8.33+)

```
┌─────────────────────────────────────────────────────────────┐
│  Niveau 1 : Kernel build CONFIG_PREEMPT=y (déjà activé)      │
│            → low-latency, jitter scheduling ~100 µs          │
│            → PREEMPT_RT non activé (verdict empirique B0 :   │
│              pics 5 ms ne sont pas du scheduling latency)    │
├─────────────────────────────────────────────────────────────┤
│  Niveau 2 : Boot cmdline (boot.scr V8.33)                    │
│            isolcpus=2,3 nohz_full=2,3 rcu_nocbs=2,3          │
│            → cores 2 et 3 retirés du scheduler général       │
│            → no tick scheduler, no RCU callback overhead     │
├─────────────────────────────────────────────────────────────┤
│  Niveau 3 : systemd service mixer-pro.service                │
│            CPUAffinity=2 3                                    │
│            → tous les threads mixer-pro pinned cores 2-3     │
│                                                              │
│            mixer-gui-http.service                            │
│            CPUAffinity=0 1                                    │
│            → GUI/HTTP sur cores 0-1, non-isolés              │
├─────────────────────────────────────────────────────────────┤
│  Niveau 4 : Thread setsched runtime                          │
│            pthread_setschedparam(SCHED_FIFO, prio=80)        │
│            → audio_thread, cap/play uac2_thread              │
│            pthread_setschedparam(SCHED_FIFO, prio=81)        │
│            → play_thread (slightly higher)                   │
│            pthread_setschedparam(SCHED_FIFO, prio=60)        │
│            → analyzer_thread                                  │
├─────────────────────────────────────────────────────────────┤
│  Niveau 5 : mlockall(MCL_CURRENT | MCL_FUTURE)                │
│            → empêche le swap des pages process                │
└─────────────────────────────────────────────────────────────┘
```

### 13.3 isolcpus + nohz_full + rcu_nocbs — la trinité RT

**isolcpus=2,3** : retire cores 2 et 3 du scheduler général. Les tâches sans CPUAffinity explicite ne tournent QUE sur cores 0-1. Les cores 2-3 sont "déserts", parfaits pour des tâches RT pinées.

**nohz_full=2,3** : sur les cores isolés, désactive le **timer tick** (interruption périodique 1 kHz du kernel). Sans tick : pas d'interruption du thread RT par le timer interrupt → latence réduite. **Pré-requis** : un seul thread runnable sur le core (sinon le tick revient pour scheduler).

**rcu_nocbs=2,3** : RCU (Read-Copy-Update) callbacks sont migrés des cores isolés vers les non-isolés. Sinon RCU softirq peut préempter le thread RT.

### 13.4 Configuration U-Boot via boot.scr

`meta-local/recipes-bsp/boot-script-rt/files/boot.cmd` :

```bash
setenv bootargs "Debix_Model_AB V1.0.3 console=ttymxc1,115200 root=/dev/mmcblk1p2 \
    rootwait rw isolcpus=2,3 nohz_full=2,3 rcu_nocbs=2,3"
load mmc ${mmcdev}:${mmcpart} ${loadaddr} Image
load mmc ${mmcdev}:${mmcpart} ${fdt_addr_r} imx8mp-evk.dtb
booti ${loadaddr} - ${fdt_addr_r}
```

Compilé par mkimage en `boot.scr` (binary U-Boot script). Installé sur partition FAT `/boot/`. U-Boot charge ce script avant `booti`, donc les bootargs sont injectés sans recompile U-Boot.

Vérification après boot :

```bash
$ cat /proc/cmdline
... isolcpus=2,3 nohz_full=2,3 rcu_nocbs=2,3

$ cat /sys/devices/system/cpu/isolated
2-3

$ cat /sys/devices/system/cpu/nohz_full
2-3

$ taskset -p $(pidof mixer-pro)
pid 16280's current affinity mask: c   # = 0xc = cores 2,3 ✓
```

### 13.5 systemd CPUAffinity

`meta-local/recipes-audio/mixer-pro/files/mixer-pro.service` :

```ini
[Unit]
Description=mixer-pro daemon
After=tac-reset.service alsa-state.service
Requires=tac-reset.service

[Service]
Type=simple
ExecStart=/usr/bin/mixer-pro --no-phone
Restart=on-failure
RestartSec=2s

# RT permissions
LimitMEMLOCK=infinity
LimitRTPRIO=99
LimitNICE=-20

# V8.33 — Affinité aux cores isolés (isolcpus=2,3 dans bootargs)
CPUAffinity=2 3

[Install]
WantedBy=multi-user.target
```

`mixer-gui-http.service` :

```ini
# V8.33 — Cores non isolés (0,1) pour ne pas polluer les cores audio (2,3)
CPUAffinity=0 1
```

### 13.6 SCHED_FIFO + prio dans mixer-pro

```c
/* mixer-pro.c — chaque thread fait son setsched */
static void *audio_thread(void *arg)
{
    struct sched_param sp = { .sched_priority = RT_PRIO_AUDIO };  /* 80 */
    int rt_ok = (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0);
    mlog("audio thread : SCHED_FIFO prio %d %s", RT_PRIO_AUDIO,
         rt_ok ? "OK" : "(failed, fallback SCHED_OTHER)");
    ...
}

static void *play_thread(void *arg)
{
    struct sched_param sp = { .sched_priority = RT_PRIO_AUDIO + 1 };  /* 81 */
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    mlog("play_thread : SCHED_FIFO prio %d", RT_PRIO_AUDIO + 1);
    ...
}
```

Priorities :
- 60 : analyzer (FFT)
- 80 : audio_thread, cap_uac2_thread, play_uac2_thread
- 81 : play_thread (slightly higher pour drainer play DSP en priorité absolue)
- 99 : réservé futur (IRQ thread)

### 13.7 mlockall pour éviter swap

```c
/* main() au démarrage */
if (mlockall(MCL_CURRENT | MCL_FUTURE) < 0) {
    perror("mlockall");
}
```

Verrouille toutes les pages mémoire du process en RAM. Sans ça, un major page fault sur un thread RT peut prendre 100+ ms (page-in depuis disk/swap). Avec mlockall = 0 ms guarantee.

### 13.8 Self-paced clock_nanosleep ABSTIME

V8.33 — sans rate limiting, audio_thread tourne à fond sur core 2 (isolcpus → no scheduler tick → no preemption). Solution :

```c
struct timespec t_next;
clock_gettime(CLOCK_MONOTONIC, &t_next);
const long PERIOD_NS = 2000000L;   /* 2 ms */

while (atomic_load(&g_st.running)) {
    /* Wait jusqu'à l'heure cible */
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t_next, NULL);
    t_next.tv_nsec += PERIOD_NS;
    while (t_next.tv_nsec >= 1000000000L) {
        t_next.tv_nsec -= 1000000000L;
        t_next.tv_sec  += 1;
    }
    /* Itération audio */
    ...
}
```

`TIMER_ABSTIME` = wake-up à un instant absolu (vs relatif). Garantit un period constant **moyen** indépendant du temps d'itération.

**Limitation observée** (V8.34 diag) : si l'itération prend > 2 ms (snd_pcm_readi DSP block + mix = parfois 2.2 ms), le wake-up suivant est en passé → no sleep → loop runs at natural rate. Empêche pas le fail mode mais garantit l'absence de busy-loop quand iter < 2 ms.

### 13.9 IRQ affinity — l'expérience B2 ratée

Théorie : router IRQ USB DWC3 (numéro 223, 28M IRQs/s) sur cores 2-3 (audio) pour réduire cross-core cache contention.

**Test empirique** (memory `[[v8-33-suite]]`) :

| Affinity | wr_us_max | rd_us_max | cap_full_evt | drift_ppm | Verdict |
|---|---|---|---|---|---|
| 0xf (default = core 0) | 2700 | 2946 | 0 | -13 | ✅ baseline |
| 0xc (cores 2,3 = audio) | **5054** | **7419** | **15745** | **-367** | ❌ catastrophe |

**Pourquoi catastrophe** : router IRQ haute fréquence sur core où tourne audio_thread RT = préempte audio_thread toutes les 1 ms (USB SOF) + cache line bouncing entre IRQ context et audio thread.

**Pattern Linux RT** (confirmé empiriquement) :

> Never route high-frequency IRQs on cores running latency-sensitive RT tasks.

Solution : default (core 0) reste optimal. mixer-gui-http aussi sur core 0, partage le core avec USB IRQ. Tradeoff acceptable car GUI tolère le jitter, audio_thread non.

### 13.10 PREEMPT_RT — pourquoi pas activé

PREEMPT_RT est le patch kernel qui rend Linux 100% preemptible (sauf 2-3 sections critiques). Pics de latence < 100 µs garantis.

**Pourquoi pas activé dans V8.33** :

- Baseline V8.33 (avec DAW actif) : wr/rd max ~3 ms steady, pics 5959 µs sont **transitoires** (DAW burst, recover xrun)
- CONFIG_PREEMPT = y déjà → jitter scheduling ~100 µs
- Pics 5 ms ne sont PAS du scheduling latency (gain RT marginal)
- Risque haut : RT patches 6.6.x ne s'appliquent pas cleanly sur fork debix-tech linux-imx (conflits patches NXP/Debix)

Décision (memory `[[kernel_preempt_status]]`) : ne pas activer PREEMPT_RT tant que le bottleneck est ailleurs (ASRC, USB drift).

### 13.11 Vérification état RT runtime

```bash
# Affinity mixer-pro
$ ps -L -p $(pidof mixer-pro) -o tid,psr,policy,rtprio,comm
   TID PSR POLICY RTPRIO COMMAND
 16280   3 FF     80    mixer-pro       # audio_thread
 16281   2 FF     81    mixer-pro       # play_thread
 16282   3 FF     80    mixer-pro       # cap_uac2_thread
 16283   2 FF     80    mixer-pro       # play_uac2_thread
 16284   2 TS      -    mixer-pro       # control_thread (OTHER)
 16285   3 FF     60    mixer-pro       # analyzer_thread

# IRQ counters
$ cat /proc/interrupts | grep dwc3
223:   28877980          0          0          0     GICv3  72 Level     dwc3
# Toutes les IRQs DWC3 sur core 0 (col 1) — comportement attendu

# Cmdline
$ cat /proc/cmdline
... isolcpus=2,3 nohz_full=2,3 rcu_nocbs=2,3
```

---

## 14. Support LV2 — plugin audio (futur)

### 14.1 Pourquoi LV2

[LV2](https://lv2plug.in/) (Linux Audio Developer's Simple Plugin API V2) est le format standard de plugin audio open-source sur Linux. Adopté par Ardour, Carla, Qtractor, Reaper (via wine bridge), etc.

L'objectif est de **transformer mixer-pro en host LV2** pour permettre :

1. Chargement de plugins audio tiers (compresseurs, EQ, reverbs, distortion…) sans recompile
2. Écosystème massif (Calf, ZynAddSubFX, Drobilla LV2 suite, NoiseRipper, IronOxide, etc.)
3. Possibilité de plugins custom NPU-accelerated (futur tap consumer)
4. Plugins MIDI controllables via OSC ou /api/cmd JSON

### 14.2 Stack LV2 dans Yocto

Le projet inclut déjà `sources/meta-musicians` (submodule git) qui apporte la stack OE audio :

```
sources/meta-musicians/recipes-audio/
├── lilv/              # lib LV2 host (charge .ttl, instancie, control)
├── lv2/               # core LV2 spec headers + serd/sord
├── suil/              # UI bridge (GTK, Qt, X11)
├── lv2-plugin-*/      # collections de plugins (Calf, swh, etc.)
└── jack/              # JACK audio server (alternatif à mixer-pro routing)
```

LV2 plugins sont stockés sous `/usr/lib/lv2/<plugin>.lv2/` :

```
/usr/lib/lv2/calf.lv2/
├── manifest.ttl           # RDF manifest
├── compressor.ttl         # Plugin descriptor
├── compressor.so          # Shared object
└── compressor_gui.so      # Optional UI
```

### 14.3 Plan d'intégration mixer-pro ⟷ LV2

**Phase 1 (planifiée)** : ajouter un slot LV2 dans chaque bus FX (4 bus × 1 plugin LV2 stéréo) :

```c
typedef struct {
    enum fx_type type;   /* FX_LV2 nouveau */
    void *lv2_instance;  /* lilv_instance_t */
    void *control_ports; /* float[] pour contrôles */
    void *audio_in_l, *audio_in_r, *audio_out_l, *audio_out_r;
    ...
} fx_engine_t;
```

Process callback :

```c
void fx_lv2_process(fx_engine_t *eng, float l, float r,
                    float *out_l, float *out_r)
{
    eng->lv2_in_l_buffer[eng->buf_idx] = l;
    eng->lv2_in_r_buffer[eng->buf_idx] = r;
    eng->buf_idx++;
    if (eng->buf_idx == PERIOD_FRAMES) {
        lilv_instance_run(eng->lv2_instance, PERIOD_FRAMES);
        eng->buf_idx = 0;
    }
    *out_l = eng->lv2_out_l_buffer[eng->buf_idx];
    *out_r = eng->lv2_out_r_buffer[eng->buf_idx];
}
```

**Phase 2** : exposer dans GUI HTTP la liste des plugins LV2 disponibles + leurs contrôles paramétriques. Sliders/knobs dynamiques générés depuis le RDF `.ttl`.

### 14.4 Considérations RT

LV2 plugins ne sont pas tous **realtime-safe**. Risques :
- Allocation mémoire en process callback (malloc → blocking)
- I/O bloquantes (file open, audio render to disk)
- Threads non-pinned

Solution : héritage de la stack **rtkit** + **pulseaudio-style RT prio** + utiliser uniquement plugins LV2 avec descriptor `lv2:hardRTCapable` :

```turtle
@prefix lv2: <http://lv2plug.in/ns/lv2core#> .
<urn:plugin:my-comp> a lv2:Plugin , lv2:CompressorPlugin ;
    lv2:hardRTCapable true ;   # MUST be present sinon refus de loading
    ...
```

mixer-pro filtrerait au load time : si pas `hardRTCapable` → skip.

### 14.5 État actuel — non implémenté

À 2026-05-21, le support LV2 n'est PAS encore code dans mixer-pro. Les engines FX actuels (`effects.c`) sont **builtin** (compresseur, reverb, delay, EQ codés en C dans le binaire).

Roadmap LV2 = phase B5 ou +, après stabilisation USB drift et 0 glitch atteint.

---

## 15. mixer-gui-http — frontend web

### 15.1 Stack technique

| Composant | Détail |
|---|---|
| Backend HTTP | `mixer-gui-http` daemon C, libmicrohttpd |
| Port | 8080 (configurable) |
| Frontend | HTML/JS single-page, Alpine.js + Tailwind CSS via CDN |
| Reverse proxy | Aucun (accès direct LAN) |
| Comm avec mixer-pro | Unix socket `/run/mixer-pro.sock` connect-per-request |
| CORS | `*` (toléré, LAN privé) |
| Real-time stream | Server-Sent Events (`/api/stream`) pour topbar refresh 10 Hz |
| FFT taps streaming | WebSocket-like via long polling `/api/meters` |

### 15.2 Endpoints REST

```
GET  /                          → static index.html
GET  /api/state                 → snapshot complet mixer (gains, mutes, sends)
GET  /api/drift                 → stats timing temps réel (wr/rd µs, drift, xrun)
GET  /api/meters                → niveaux VU mètres 26 ch + FFT 4 taps
GET  /api/stream                → SSE flow drift + state changes
GET  /api/reset_drift_stats     → reset min/max/buckets/events ASRC
GET  /api/apply_drift_as_shift  → force shift_ppm = round(drift_ppm)
GET  /api/alsa/contents         → liste de tous les kcontrols ALSA
GET  /api/dsp/blob/<numid>/raw  → lecture brute d'un kcontrol blob (DSP DRC/EQ)
POST /api/cmd                   → forward JSON vers mixer-pro
POST /api/alsa/set              → set kcontrol (amixer cset wrapper)
POST /api/dsp/blob/set          → écrit blob TLV (24 biquads, DRC config…)
```

### 15.3 Frontend index.html

Le frontend est un **single-page HTML** avec Alpine.js pour le state management :

```html
<div x-data="mixerApp()" x-init="connect()">
  <!-- TopBar : stats temps réel -->
  <div class="topbar">
    <div class="app-stat">
      shift <span x-text="drift.shift_ppm"></span> ppm
    </div>
    <div class="app-stat" :class="(drift.cap_full_evt > 0) ? 'warn' : ''">
      cap_full <span x-text="drift.cap_full_evt"></span>
    </div>
    <div class="app-stat" :title="'Intervalle entre 2 push wr du ring USB cap'">
      wr <span x-text="drift.wr_us_min + '/' + drift.wr_us_avg + '/' + drift.wr_us_max"></span> µs
    </div>
    ...
  </div>

  <!-- Layout grid : inputs / fx bus / outputs -->
  <div class="grid">
    <div class="inputs">
      <template x-for="(ch, idx) in masterIn">
        <div class="strip">
          <input type="range" :value="ch" @input="setInput(idx, $event.target.value)">
          <button @click="toggleMute(idx)">M</button>
          <!-- Sends ×4 -->
          <template x-for="bus in 4">
            <input type="checkbox" :checked="sends[idx][bus-1]" @change="setSend(idx, bus-1)">
          </template>
        </div>
      </template>
    </div>
    <div class="fxbus">
      <!-- 4 bus FX avec engine selector -->
    </div>
    <div class="outputs">
      <!-- 18 outputs avec master gain -->
    </div>
  </div>

  <!-- Sidebar effects TAC5212 + DSP -->
  <div class="sidebar">
    <div class="tabs">
      <div @click="tab='biquads'">Biquads</div>
      <div @click="tab='drc'">DRC</div>
      <div @click="tab='multiband'">Multiband</div>
    </div>
    <div x-show="tab=='biquads'">
      <!-- 24 biquads × 4 TACs, inline editor -->
    </div>
  </div>

  <!-- 4 analyzer FFT + scope -->
  <div class="analyzers">
    <template x-for="tap in 4">
      <canvas :id="'fft-'+tap"></canvas>
      <canvas :id="'scope-'+tap"></canvas>
    </template>
  </div>
</div>

<script>
function mixerApp() {
    return {
        drift: { ppm: 0, shift_ppm: 0, wr_us_max: 0, rd_us_max: 0, ... },
        masterIn: Array.from({length: 26}, () => 0),
        mute: Array.from({length: 26}, () => false),
        sends: Array.from({length: 26}, () => Array(4).fill(false)),
        ...
        connect() {
            const es = new EventSource('/api/stream');
            es.onmessage = (e) => {
                const d = JSON.parse(e.data);
                this.drift = { ...this.drift, ...d };
            };
            this.refreshState();
            setInterval(() => this.refreshMeters(), 100);
        },
        ...
    };
}
</script>
```

### 15.4 Connexion mixer-pro socket — connect-per-request

`mixer-gui-http.c` ouvre une nouvelle connexion Unix socket pour chaque requête HTTP. Évite le partage de connexion (thread safety) et permet `mixer-pro` restart sans casser le frontend (connexion suivante = recovery automatique).

```c
static int mixer_request(const char *json_request, char *reply_buf, size_t reply_size)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strcpy(addr.sun_path, MIXER_SOCK_PATH);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    write(fd, json_request, strlen(json_request));
    int n = read(fd, reply_buf, reply_size - 1);
    close(fd);
    if (n > 0) reply_buf[n] = 0;
    return n;
}
```

### 15.5 Analyzer FFT streaming

Le mixer-pro `analyzer_thread` calcule un FFT 1024 bins + scope 64 stereo pairs à 30 Hz pour chaque tap actif. Les résultats sont sérialisés en JSON et streamés via `/api/meters` :

```json
{
  "taps": [
    {
      "id": 0,
      "kind": "input",
      "source_a": 0,
      "source_b": -1,
      "fft": [-60.2, -59.1, ...],  /* 128 bins dB */
      "scope": [[0.1, 0.0], [0.2, 0.1], ...]  /* 64 stereo pairs */
    },
    ...
  ]
}
```

Le frontend dessine canvas FFT (bar graph 128 bins) + scope (X-Y stereo, points 64).

---

## 16. Build, déploiement, debug

### 16.1 Build complet from scratch

```bash
# 0. Cloner le repo + initialiser submodules
git clone git@github.com:Mjxkill/yocto-nxp-debix.git
cd yocto-nxp-debix
git submodule update --init --recursive   # → sources/meta-musicians + sof/

# 1. Pré-requis disque : >120 GB libres
df -h .

# 2. Setup env (1× par session shell)
EULA=1 DISTRO=fsl-imx-xwayland MACHINE=imx8mpevk \
    source imx-setup-release.sh -b Model_AB_Infinity

# 3. Build
bitbake imx-image-full
# Durée : ~6-10 h sur PC moderne au premier build, ~30 min en incrémental
```

### 16.2 Flash SD card

```bash
DEPLOY=Model_AB_Infinity/tmp/deploy/images/imx8mpevk
WIC=$DEPLOY/imx-image-full-imx8mpevk.rootfs-*.wic
sudo dd if=$WIC of=/dev/sdX bs=4M status=progress conv=fsync
sudo sync
```

### 16.3 Boot + premier accès

```bash
# Board boot ~30 sec, IP = 192.168.0.9 (DHCP fallback ?)
ping 192.168.0.9
ssh root@192.168.0.9   # no password sur build dev
```

### 16.4 Iteration rapide : rebuild + redéploie composant

Pour edit mixer-pro.c + tester rapidement sans flash SD :

```bash
# Sur PC
source sources/poky/oe-init-build-env Model_AB_Infinity > /dev/null
bitbake -c cleansstate mixer-pro
bitbake mixer-pro

# Deploy deb directement
DEB=$(find Model_AB_Infinity/tmp/deploy/deb -name "mixer-pro_*_arm64.deb" | head -1)
scp $DEB root@192.168.0.9:/tmp/
ssh root@192.168.0.9 "systemctl stop mixer-pro && dpkg -i /tmp/mixer-pro_*_arm64.deb && systemctl start mixer-pro"
```

Pour kernel modules (ex tac5212) :

```bash
bitbake -c cleansstate linux-imx
bitbake linux-imx   # full kernel rebuild (~10 min)

DEB=Model_AB_Infinity/tmp/deploy/deb/imx8mpevk/kernel-module-snd-soc-tac5212-*_arm64.deb
scp $DEB root@192.168.0.9:/tmp/
ssh root@192.168.0.9 "dpkg -i /tmp/kernel-module-snd-soc-tac5212-*.deb && \
                       rmmod snd_soc_tac5212; modprobe snd_soc_tac5212"
```

Pour kernel/dtb (besoin reboot) :

```bash
scp Model_AB_Infinity/tmp/deploy/images/imx8mpevk/Image \
    Model_AB_Infinity/tmp/deploy/images/imx8mpevk/imx8mp-evk.dtb \
    root@192.168.0.9:/boot/
ssh root@192.168.0.9 "sync && reboot"
```

### 16.5 Debug runtime

```bash
# Logs mixer-pro
journalctl -fu mixer-pro

# Status stream
curl -s http://192.168.0.9:8080/api/drift | jq .
curl -s http://192.168.0.9:8080/api/state | jq .

# IRQ counts
ssh root@192.168.0.9 "cat /proc/interrupts | grep -E 'dwc3|sai'"

# Thread states
ssh root@192.168.0.9 "ps -L -p \$(pidof mixer-pro) -o tid,psr,policy,rtprio,comm"

# ALSA xrun cap_dsp
ssh root@192.168.0.9 "cat /proc/asound/card3/pcm0c/sub0/status"

# Reset all stats
curl -s http://192.168.0.9:8080/api/reset_drift_stats
```

### 16.6 SOF firmware deploy

Si on edit le SOF firmware :

```bash
# Sur PC, dans sof/
cd sof
# Build via west (Zephyr build system)
west build -b imx8mp_qemu_adsp ...   # commande détaillée dans README projet sof/

# Output : build/zephyr/sof.ri (raw image)
# IMPORTANT : il faut concaténer le .ri.xman (manifest) au début sinon IPC 108/20 error
cat build/sof.ri.xman build/sof.ri > sof-imx8m.ri
scp sof-imx8m.ri root@192.168.0.9:/lib/firmware/imx/sof/sof-imx8m.ri

# Reload
ssh root@192.168.0.9 "systemctl restart mixer-pro"
# (le restart re-fait le firmware download via sof-imx driver IPC)
```

### 16.7 Topology deploy

```bash
cd meta-local/recipes-kernel/linux/files/
m4 sof-imx8mp-tac5212.m4 > sof-imx8mp-tac5212.conf
alsatplg -c sof-imx8mp-tac5212.conf -o sof-imx8mp-tac5212.tplg
scp sof-imx8mp-tac5212.tplg root@192.168.0.9:/lib/firmware/imx/sof-tplg/
ssh root@192.168.0.9 "reboot"   # topology rechargée au boot SOF
```

### 16.8 Critic system (memory `[[feedback_critic_imperative]]`)

Le projet utilise un système de **critic externe** via MCP pour valider chaque décision technique. Protocol obligatoire :

1. Phase 1 — Analyse écrite
2. `critic_analyze` → soumet l'analyse, attend response
3. Phase 2 — Correction si non approuvé
4. Phase 3 — Implémentation
5. `critic_review` sur le code
6. `critic_decision` documente

Configuration dans `/home/michael/yocto-nxp-debix/REGLES.md`.

---

## 17. Roadmap et problèmes connus

### 17.1 État des sprints (depuis ARCHI_V7.0.md §9)

| Sprint | Status | Sujet |
|---|---|---|
| E0 | ✅ | Baseline E6.a |
| E1-E3 | ✅ | Multiband DRC 8ch, DRC D3 patch state arrays, removed final drc limiter |
| E4 | ✅ | NPU tap-in V7.0 (capture brut) |
| E5 | ✅ | NPU tap-out (post-effets, alias V3.2.2) |
| E6.a-e | ✅ | USB UAC2 8×8 gadget, matrix N×M, mixer-pro daemon |
| E6.i, j, k, l | ❌ | Tentatives optim latence userspace — KO reverted (architectural floor) |
| E7 | ✅ | GUI HTTP V7.0 (libmicrohttpd + Alpine.js) |
| E7.4-7.6 | ✅ | Effets TAC + DSP DRC editor + crossover + 4 analyzers FFT |
| V8.0-8.32 | ⚠ partiel | UAC2 ASRC + ring + timing stats |
| V8.33 | ✅ | RT baseline (isolcpus + self-paced) |
| B0/B2 | ❌ | IRQ affinity testé, rejeté empiriquement |
| V8.34 PLC | ❌ | Tentative PLC, reverted (pas la bonne cible) |
| V8.35 | 🟡 en test | ASRC drop ±1 fine granularité |

### 17.2 Problèmes connus actuels (V8.35)

1. **Glitchs USB ↔ DSP** : drift résiduel +11 ppm cause clicks audibles. Pas de fix définitif → solution attendue dans futur sprint (sinc interp, fill-based correction)
2. **cap_full_evt élevé** : 728 events/s en moyenne sur USB cap ring saturé → producer drop occasionnel
3. **prof_iter_us** parfois > 2 ms (jusqu'à 2.2 ms) sur transitoires → consumer fall-behind brève
4. **f_uac2 no HW htstamp** : drift mesuré soft (CLOCK_MONOTONIC) moins précis que HW timestamping
5. **2× USB asynchrones impossibles** : si on ajoute USB téléphone, drift double, ASRC HW pas applicable

### 17.3 Roadmap future

#### Court terme (sprints suivants)

- **V8.36 sinc resampling** : remplacer drop fusion 2-en-1 par interpolation sinc (polyphase FIR). Plus de CPU mais artefacts spectraux mieux contrôlés.
- **Fill-based correction** : activer UAC2_FILL_LOW/HIGH constantes inactives, ajouter mode "hard override" quand fill > 75% ou < 25%.
- **PLC repenser** : non sur cap_empty (rare) mais sur cap_full (insérer interp samples temporaires côté push).

#### Moyen terme

- **Support LV2** : intégration host LV2 dans bus FX (cf §14)
- **2e USB téléphone** : ajout PCM tél via gadget USB + ring dédié dans mixer-pro
- **NPU master loop production** : skeleton existe (`audio-tools/npu_master_loop.py`), reste à coder l'inference + feedback loop

#### Long terme

- **PREEMPT_RT** : si transitoires kernel scheduling deviennent le bottleneck après autres optims
- **NPU ML inference** : modèle audio classifier (voix/musique/bruit) + auto-EQ + auto-compress paramètres
- **Hardware ASRC i.MX 8M Plus** : malgré incompatibilité 2-USB, possible utilisation pour 1 ASRC USB↔DSP avec mixer-pro fallback côté tél

### 17.4 Memory consolidée (mémoire persistante Claude)

Les mémoires consolidées du projet sont dans `/home/michael/.claude/projects/-home-michael-yocto-nxp-debix/memory/` :

- `mixer_pro_v8_33.md` — V8.33 baseline RT
- `v8_33_suite.md` — diagnostic empirique B0/B2/PLC + pistes futures ASRC
- `gui_timing_stats.md` — widgets timing GUI
- `push_atomic_96.md` — atomic push ring USB cap
- `sof_async_solution.md` — mode ASYNC TX continuous clock
- `sof_sai_idempotence_patch.md` — flag configured pour skip glitch
- `tac_reset_required_after_boot.md` — séquence reset PLL TAC
- `npu_tap_v1_proposal.md` — design dual-tap NPU
- `project_v7_audio_devices.md` — invariants V7.0 (mixer Linux ⇆ DSP)
- `project_npu_non_negotiable.md` — NPU monitoring obligatoire

---

## 18. Annexes — extraits clés

### 18.1 Structure repo

```
/home/michael/yocto-nxp-debix/
├── ARCHI/
│   └── ARCHI_V7.0.md                              # Doc architecture maître
├── TESTS/
│   └── TESTS_V8.33_RT_isolcpus.md                 # Fiches de test par version
├── DOC_PROJET_COMPLET.md                          # CE DOCUMENT
├── REGLES.md                                      # Protocole de travail
├── CLAUDE.md                                      # Instructions IA assistant
├── Model_AB_Infinity/                             # Build directory Yocto
│   ├── conf/{bblayers,local}.conf
│   └── tmp/                                       # Generated, ~100 GB
├── sources/                                       # Yocto layers upstream
│   ├── poky/
│   ├── meta-imx/
│   ├── meta-freescale*/
│   ├── meta-musicians/                            # submodule
│   └── (~40 layers en tout)
├── meta-local/                                    # Code custom projet
│   ├── recipes-audio/
│   │   ├── mixer-pro/
│   │   │   ├── files/
│   │   │   │   ├── mixer-pro.c (2412 lignes)
│   │   │   │   ├── mixer-pro.h
│   │   │   │   ├── analyzer.{c,h}
│   │   │   │   ├── effects.{c,h}
│   │   │   │   ├── Makefile
│   │   │   │   └── mixer-pro.service
│   │   │   └── mixer-pro_1.0.bb
│   │   └── mixer-gui-http/
│   │       ├── files/
│   │       │   ├── mixer-gui-http.c
│   │       │   ├── www/index.html
│   │       │   └── mixer-gui-http.service
│   │       └── mixer-gui-http_1.0.bb
│   ├── recipes-bsp/
│   │   ├── boot-script-rt/                        # V8.33 isolcpus
│   │   ├── snd-aloop-phone/
│   │   ├── usb-uac2-gadget/
│   │   └── sof-firmware-custom/                   # Package SOF .ri custom
│   ├── recipes-kernel/
│   │   ├── imx-audio-tap/                         # Module out-of-tree NPU tap
│   │   │   ├── files/
│   │   │   │   ├── imx-audio-tap.c
│   │   │   │   ├── imx-audio-tap-uapi.h
│   │   │   │   └── Makefile
│   │   │   └── imx-audio-tap_0.1.bb
│   │   └── linux/
│   │       ├── files/
│   │       │   ├── tac5212.c (1281 lignes)
│   │       │   ├── tac5212.h
│   │       │   ├── apply-tac5212-dt.py
│   │       │   ├── apply-npu-tap-dt.py
│   │       │   ├── apply-sdram2-dt.py
│   │       │   ├── apply-simple-card-multicodec.py
│   │       │   ├── apply-imx-card-linkid.py
│   │       │   ├── apply-imx-probes.py
│   │       │   ├── imx-probes.c
│   │       │   ├── tac-reset.sh
│   │       │   ├── tac-reset.service
│   │       │   ├── sof-imx8mp-tac5212.m4
│   │       │   └── *.cfg                          # Kernel config fragments
│   │       └── linux-imx_%.bbappend
│   ├── recipes-fsl/images/
│   │   └── imx-image-full.bbappend
│   ├── audio-tools/
│   │   ├── npu_tap_reader.c
│   │   ├── tap_channel_monitor.py
│   │   └── npu_master_loop.py
│   └── recipes-musicians/                         # Ardour 6.9 + deps
├── sof/                                           # SOF firmware fork (submodule)
│   └── src/
│       ├── audio/
│       │   ├── dai-zephyr.c                       # Pipeline orchestration
│       │   ├── npu_tap.c                          # Hook dual-tap publish
│       │   └── components/
│       │       ├── multiband_drc/                 # Patché multi-config
│       │       └── drc/                           # D3 state arrays
│       ├── drivers/imx/
│       │   ├── sai.c (641 lignes)
│       │   ├── sdma.c (1293 lignes)
│       │   └── edma.c
│       └── include/sof/audio/
│           └── npu_tap.h                          # UAPI shared kernel/SOF
└── downloads/                                     # Yocto fetch cache (~30 GB)
```

### 18.2 Variables de build essentielles

```bash
# Dans Model_AB_Infinity/conf/local.conf
MACHINE = "imx8mpevk"
DISTRO = "fsl-imx-xwayland"
PACKAGE_CLASSES = "package_deb"
EXTRA_IMAGE_FEATURES = "debug-tweaks tools-debug ssh-server-openssh"
ACCEPT_FSL_EULA = "1"
LICENSE_FLAGS_ACCEPTED = "commercial"
INHERIT += "rm_work"          # Nettoie work/ après chaque recipe (économie disque)
```

### 18.3 Commits récents notables

```
92e8ddac  ARCHI V7.0 §9: doc V8.34 PLC tentative + revert, vrai bottleneck = ASRC
18570966  Revert "V8.34: PLC packet loss concealment côté cap ring + diag empirique B0/B2"
fb86e3d8  V8.34: PLC packet loss concealment côté cap ring + diag empirique B0/B2
a2d0e9e3  imx-image-full: fix kernel-module-imx-audio-tap resolution
d86e8d28  V8.33: RT baseline isolcpus + self-paced + boot.scr Yocto
8518f9fb  mixer-pro: V8.1.b — drift meter robust (window 10s + reset on xrun)
b083331d  meta-local: J2 V3.2.2 NPU tap kernel module + DT carve
4bea8e59d (SOF) imx sai: V7.0-E7.2 TX FIFO alignment — prime N zeros instead of 1
```

### 18.4 Commande de référence pour tester un audio loop

```bash
# Sur board, après tac-reset + mixer-pro actif
ssh root@192.168.0.9

# Test capture DSP cap (8 ch S32 48 kHz)
arecord -D hw:softac5212tdm,0 -c 8 -f S32_LE -r 48000 -d 5 /tmp/cap_dsp.wav

# Test playback DSP play
aplay -D hw:softac5212tdm,0 -c 8 -f S32_LE -r 48000 /tmp/cap_dsp.wav

# Test capture USB UAC2 (depuis PC hôte, board = USB device)
# Sur PC : arecord -D hw:UAC2Gadget,0 -c 8 -f S32_LE -r 48000 -d 5 /tmp/usb_cap.wav

# Generate sine 1 kHz et envoyer dans cap_uac2 (PC → board USB)
# Sur PC : sox -n -r 48000 -c 8 -b 32 -e signed-integer /tmp/sine.wav synth 5 sine 1000
#         aplay -D hw:UAC2Gadget,0 /tmp/sine.wav

# Vérifier mixer-pro stats
curl -s http://192.168.0.9:8080/api/drift | jq .
```

### 18.5 Documentation externe référencée

| Source | Contenu utile |
|---|---|
| NXP IMX8MPRM | Reference manual SoC (5000+ pages), section SAI, SDMA, audiomix PLL |
| TI SLASF23A | Datasheet TAC5212 codec, registers, PLL, effets internes |
| SOF docs (thesofproject.github.io) | Architecture SOF, IPC, topology format |
| Linux ALSA docs | snd_pcm_* API, kcontrol, machine drivers |
| Yocto Project Mega-Manual | Recipes, bbclass, image targets |
| LV2 spec (lv2plug.in) | Plugin format, RDF descriptors |
| USB Audio Class 2.0 spec | Endpoint, descriptors, gadget mode |

---

## Fin du document

**Statistique** : ~150 pages markdown rendered, ~25 000 mots, ~50 extraits de code factuel, 18 sections.

Pour toute question sur un point précis, se référer directement aux fichiers cités (paths complets fournis dans chaque section). Memory Claude (`MEMORY.md` + entries) contient l'historique des décisions empiriques validées.









