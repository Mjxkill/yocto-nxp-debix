# SOF ASYNC Audio Platform — Guide Technique Complet

## Table des matières

1. [Contexte et objectif](#1-contexte-et-objectif)
2. [Architecture matérielle](#2-architecture-matérielle)
3. [Historique du problème](#3-historique-du-problème)
4. [Diagnostic et root cause](#4-diagnostic-et-root-cause)
5. [Solution : firmware SOF custom](#5-solution--firmware-sof-custom)
6. [Configuration des TAC5212](#6-configuration-des-tac5212)
7. [Configuration du Device Tree](#7-configuration-du-device-tree)
8. [Build du firmware SOF](#8-build-du-firmware-sof)
9. [Procédure de déploiement](#9-procédure-de-déploiement)
10. [Résultats et validation](#10-résultats-et-validation)
11. [Problèmes rencontrés et solutions](#11-problèmes-rencontrés-et-solutions)
12. [Registres de référence](#12-registres-de-référence)
13. [Fichiers modifiés](#13-fichiers-modifiés)

---

## 1. Contexte et objectif

### Plateforme

- **Board** : Debix Model AB (NXP i.MX 8M Plus)
- **SoC** : i.MX8MP (Cortex-A53 + HiFi4 DSP + Cortex-M7)
- **Audio** : 4x TI TAC5212 stereo ADC/DAC (8 canaux capture, 8 canaux playback)
- **Interface** : TDM 8 slots x 32 bits sur SAI7, piloté par le DSP via SOF (Sound Open Firmware)

### Objectif

Obtenir un plancher de bruit < -100 dBFS sur les 8 canaux de capture en mode SOF/DSP, équivalent à ce qui était obtenu en mode ALSA direct (< -100 dBFS).

### Résultat obtenu

**-118 à -122 dBFS** sur tous les canaux (entrées non connectées), stable sur 10 recordings consécutifs de 30 secondes. Correspond au plancher de bruit théorique du TAC5212 (119 dB de plage dynamique).

---

## 2. Architecture matérielle

### Bus audio TDM

```
                    BCLK (12.288 MHz)
                    FSYNC (48 kHz)
    i.MX8MP         DOUT ──────────────────────────┐
    SAI7 ──────────  DIN ──────────────────────────┐│
    (DSP)           RX_BCLK ◄─────────────────┐   ││
                    RX_FSYNC ◄────────────────┐│   ││
                                              ││   ││
                    ┌──────┐ ┌──────┐ ┌──────┐││┌──────┐
                    │ TAC0 │ │ TAC1 │ │ TAC2 ││││ TAC3 │
                    │ 0x50 │ │ 0x51 │ │ 0x52 ││││ 0x53 │
                    │ CH0-1│ │ CH2-3│ │ CH4-5││││ CH6-7│
                    └──┬───┘ └──┬───┘ └──┬───┘│└──┬───┘
                       │        │        │    │   │
                       └────────┴────────┴────┴───┘
                            Bus partagé (BCLK, FSYNC, DOUT, DIN)
```

### Pins SAI7 sur i.MX8MP

| Signal | Pad i.MX8MP | IOMUXC offset | ALT | SION | Input Select |
|--------|-------------|---------------|-----|------|-------------|
| TX_BCLK | ECSPI2_SCLK | 0x1F0 | 3 | 0 | — |
| TX_DATA | ECSPI2_MOSI | 0x1F4 | 3 | 0 | — |
| TX_SYNC | ECSPI1_SS0 | 0x1EC | 3 | 0 | — (hog group) |
| RX_DATA | ECSPI1_MISO | 0x1E8 | 3 | 1 | 0x534 = 1 |
| RX_BCLK | ECSPI1_MOSI | 0x1E4 | 3 | 1 | 0x530 = 1 |
| RX_SYNC | ECSPI1_SCLK | 0x1E0 | 3 | 1 | 0x538 = 1 |

**Attention** : les noms sont contre-intuitifs !
- ECSPI1_**SCLK** = SAI7_RX_**SYNC** (pas RX_BCLK !)
- ECSPI1_**MOSI** = SAI7_RX_**BCLK** (pas RX_SYNC !)

Source : `imx8mp-pinfunc.h` :
```c
MX8MP_IOMUXC_ECSPI1_SCLK__AUDIOMIX_SAI7_RX_SYNC   0x1E0 0x440 0x538 0x3 0x1
MX8MP_IOMUXC_ECSPI1_MOSI__AUDIOMIX_SAI7_RX_BCLK   0x1E4 0x444 0x530 0x3 0x1
```

### Câblage PCB

Les pins TX_BCLK/TX_FSYNC du SAI7 sont câblées aux pins BCLK/FSYNC des 4 TAC. Les mêmes signaux reviennent sur les pins RX_BCLK/RX_FSYNC via le PCB. Ce câblage est **essentiel** pour le mode ASYNC.

### TAC5212 sur bus I2C

| TAC | Adresse I2C | Bus I2C | Slots TDM TX | Slots TDM RX |
|-----|-------------|---------|-------------|-------------|
| TAC0 | 0x50 | 3 | 0-1 | 0-1 |
| TAC1 | 0x51 | 3 | 2-3 | 2-3 |
| TAC2 | 0x52 | 3 | 4-5 | 4-5 |
| TAC3 | 0x53 | 3 | 6-7 | 6-7 |

TAC0 est le plus proche du SAI7 sur le bus (bus keeper activé).

---

## 3. Historique du problème

### Phase 1 : ALSA fonctionnel (avant SOF)

En mode ALSA, le SAI7 était contrôlé directement par le driver Linux `fsl_sai`. Configuration :
- **Mode ASYNC** : TX et RX ont des domaines d'horloge séparés
- TX = master (génère BCLK/FSYNC)
- RX = slave (reçoit BCLK/FSYNC via les pins RX physiques)
- 3 patches sur `fsl_sai.c` :
  1. Force RX en consumer (BCD=0, FSD=0)
  2. Démarre TX quand capture commence (TRCE + TERE)
  3. Configure les registres TX TDM pour la génération d'horloge
- Résultat : **< -100 dBFS**, fonctionnel avec `tac-reset` après chaque démarrage de stream

### Phase 2 : Migration vers SOF/DSP

Objectif : utiliser le DSP HiFi4 pour le traitement audio (EQ, DRC, volume). Le firmware SOF prend le contrôle de SAI7. Le driver Linux `fsl_sai` est désactivé (`status = "disabled"` sur le noeud SAI7).

**Problème immédiat** : bruit blanc à -58 dBFS au lieu de < -100 dBFS.

### Phase 3 : Diagnostic (2 jours de debug)

Tests systématiques effectués sans amélioration significative :

| Test | Résultat | Conclusion |
|------|----------|------------|
| BCLK_POL=0 vs 1 (TAC) | -55 vs -59 dBFS | Non significatif |
| BCP=0 vs 1 (SAI) | -58 dBFS dans les deux cas | Pas la cause |
| FCONT activé | -58 dBFS | Pas la cause |
| TX_OFFSET=0 vs 1 | -58 dBFS | Pas la cause |
| TX_LSB=0 vs 1 | -58 dBFS | Pas la cause |
| RATIO_CLK_EDGE=0 vs 1 | -55 dBFS | Pas la cause |
| Phase calibration (0-63) | -55 dBFS | Pas la cause |
| CH3-8 slot mapping | -58 dBFS | Pas la cause |
| Pin mux vérifié | Tout correct | Pas la cause |
| Registres TAC (4x dump complet) | Tous corrects | Pas la cause |
| PLL status, clock errors | PLL locked, 0 erreur | Pas la cause |

**Observation clé** : le bruit à -58 dBFS est constant quoi qu'on change dans la configuration TAC ou SAI. Il est identique sur les 8 canaux (sauf Ch0 avec micro).

### Phase 4 : Découverte de la cause racine

L'insight vient de la comparaison ALSA vs SOF :

- **ALSA** : mode ASYNC → RX_BCLK voyage à travers le PCB vers les TAC et revient → même retard de propagation que DOUT → échantillonnage propre
- **SOF** : mode SYNC → RX utilise le clock TX interne (zéro retard) → DOUT a un retard de propagation → le SAI échantillonne pendant la transition des données

---

## 4. Diagnostic et root cause

### Le problème de timing

```
Mode SYNC (SOF original) :

    TX_BCLK ──→ [PCB trace ~18ns] ──→ TAC BCLK
                                      TAC DOUT ──→ [PCB trace ~18ns] ──→ RX_DATA
    SAI RX clock = TX interne (0ns de retard)
    
    ╔══════════════════════════════════════════════╗
    ║ Le SAI échantillonne RX_DATA avec le clock   ║
    ║ TX INTERNE (pas de retard), mais RX_DATA     ║
    ║ arrive ~18ns+ en retard → échantillonnage    ║
    ║ pendant la transition → BRUIT à -58 dBFS    ║
    ╚══════════════════════════════════════════════╝

Mode ASYNC (solution) :

    TX_BCLK ──→ [PCB trace] ──→ TAC BCLK ──→ [PCB trace] ──→ RX_BCLK
                                TAC DOUT  ──→ [PCB trace] ──→ RX_DATA
    SAI RX clock = RX_BCLK (même retard que RX_DATA)
    
    ╔══════════════════════════════════════════════╗
    ║ RX_BCLK et RX_DATA ont le MÊME retard de     ║
    ║ propagation → échantillonnage au bon moment  ║
    ║ → BRUIT à -121 dBFS (plancher théorique)     ║
    ╚══════════════════════════════════════════════╝
```

### Chiffres du datasheet TAC5212

- `t_d(DOUT-BCLK)` : 14ns (IOVDD=3.3V), 18ns (IOVDD=1.8V) — délai intrinsèque
- BCLK = 12.288 MHz (256 x 48 kHz) → demi-période = 40.7 ns
- Marge avec 4 TAC en bus partagé (capacité cumulée) : **quasi nulle en mode SYNC**

### Pourquoi le bruit est constant à -58 dBFS

-58 dBFS correspond à environ 10-11 bits d'incertitude. Quand le SAI échantillonne pendant la transition des données :
- Les MSBs (bits de poids fort) se stabilisent rapidement → corrects
- Les LSBs (bits de poids faible) sont encore en transition → aléatoires
- Résultat : ~10 bits de données valides + ~14 bits de bruit → plancher à -58 dBFS

---

## 5. Solution : firmware SOF custom

### Modifications dans `src/drivers/imx/sai.c`

Le firmware SOF est compilé depuis les sources v2.10 avec 3 modifications :

#### 5.1 Mode ASYNC (sai_set_config)

```c
// AVANT (SYNC mode) :
val_cr2 |= REG_SAI_CR2_SYNC;
mask_cr2 |= REG_SAI_CR2_SYNC_MASK;
dai_update_bits(dai, REG_SAI_XCR2(REG_RX_DIR), mask_cr2, val_cr2);
dai_update_bits(dai, REG_SAI_XCR4(REG_RX_DIR), mask_cr4, val_cr4);

// APRES (ASYNC mode) :
mask_cr2 |= REG_SAI_CR2_SYNC_MASK;     // SYNC dans le mask mais PAS dans val → cleared
val_cr2 &= ~REG_SAI_CR2_BCD_MSTR;      // RX est consumer (reçoit BCLK externe)
val_cr4 &= ~REG_SAI_CR4_FSD_MSTR;      // RX FSYNC depuis externe
dai_update_bits(dai, REG_SAI_XCR2(REG_RX_DIR), mask_cr2, val_cr2);
dai_update_bits(dai, REG_SAI_XCR4(REG_RX_DIR), mask_cr4, val_cr4);
```

Registres SAI7 résultants :
- **TCR2** = `0x07800000` : BCD=1 (master), BCP=1, MSEL=MCLK1, BYP=1
- **RCR2** = `0x04800000` : SYNC=0, BCD=0 (consumer), BCP=1
- **TCR4** = `0x10070039` : FCONT=1, FSD=1 (master), FSE=1, FRSZ=8, CHMOD=1
- **RCR4** = `0x10070038` : FCONT=1, FSD=0 (consumer)

#### 5.2 FCONT (sai_set_config)

```c
val_cr4 |= REG_SAI_CR4_FCONT;   // FIFO Continue on Error

// Et dans le mask :
mask_cr4 |= REG_SAI_CR4_FCONT;
```

FCONT empêche le SAI de s'arrêter sur une erreur FIFO (overrun/underrun). Sans FCONT, une erreur FIFO décale les slots TDM.

#### 5.3 Clock continue (sai_set_config + sai_start + sai_stop)

**sai_set_config** — démarrage du clock TX au boot :
```c
#if defined(CONFIG_IMX8M)
    // Enable TX TRCE + TERE at boot for continuous clock
    dai_update_bits(dai, REG_SAI_XCR3(REG_TX_DIR),
            REG_SAI_CR3_TRCE_MASK, REG_SAI_CR3_TRCE(1));
    dai_write(dai, REG_SAI_TDR0, 0x0);  // prime FIFO
    dai_update_bits(dai, REG_SAI_XCSR(DAI_DIR_PLAYBACK),
            REG_SAI_CSR_TERE, REG_SAI_CSR_TERE);
    dai_update_bits(dai, REG_SAI_MCTL, REG_SAI_MCTL_MCLK_EN,
            REG_SAI_MCTL_MCLK_EN);
#endif
```

**sai_start** — ne jamais reset TX :
```c
if (direction == DAI_DIR_CAPTURE) {
    // Reset RX only — never touch TX
    dai_update_bits(dai, REG_SAI_XCSR(DAI_DIR_CAPTURE),
            REG_SAI_CSR_SR, REG_SAI_CSR_SR);
    dai_update_bits(dai, REG_SAI_XCSR(DAI_DIR_CAPTURE),
            REG_SAI_CSR_SR, 0U);
    // TX clock stays running (continuous clock for async mode)
}
```

**sai_stop** — ne jamais arrêter TX :
```c
if (direction == DAI_DIR_CAPTURE) {
    dai_update_bits(dai, REG_SAI_XCSR(DAI_DIR_CAPTURE),
            REG_SAI_CSR_TERE, 0);
    // TX stays running — continuous clock for TAC PLL
}
```

### Pourquoi la clock continue ?

Les TAC5212 utilisent une PLL interne qui se verrouille sur BCLK. Si BCLK s'arrête :
1. La PLL perd le verrouillage
2. Au redémarrage, la PLL doit re-verrouiller → transitoire de ~1s
3. Pendant la re-verrouillage, les données ADC sont du bruit

Avec la clock continue, la PLL reste verrouillée en permanence. Plus besoin de `tac-reset` entre les recordings.

---

## 6. Configuration des TAC5212

### Script tac-reset.sh

Le script `tac-reset` effectue un reset séquentiel des 4 TAC (un à la fois pour éviter les conflits DOUT sur bus partagé avec clock continue).

### Registres configurés par tac-reset

| Registre | Adresse | Valeur | Description |
|----------|---------|--------|-------------|
| SW_RESET | 0x01 | 0x01 | Reset logiciel (auto-clear) |
| DEV_MISC_CFG | 0x02 | 0x09 | SLEEP_ENZ + SLEEP_EXIT_VREF_EN |
| INTF_CFG1 | 0x10 | 0x51 | DOUT=PASI TX, **DRV=001 (push-push)** |
| INTF_CFG2 | 0x11 | 0x80 | PASI_DIN_EN=1 |
| ASI_CFG0 | 0x18 | 0x40 | SASI disabled |
| MISC_CFG | 0x04 | 0x40 | IGNORE_CLK_ERR=1 |
| PASI_TX_CFG0 | 0x1B | 0x48 (TAC0) / 0x40 (autres) | TX_FILL=1, TX_LSB=0, KEEPER=01/00 |
| PASI_TX_CFG1 | 0x1C | 0x01 | **TX_OFFSET=1** (pour DSP_A avec FSE=1) |
| PASI_RX_CFG0 | 0x26 | 0x01 | **RX_OFFSET=1** |
| GPO1_CFG0 | 0x0C | 0x41 | PDMCLK output |
| GPI_CFG | 0x0D | 0x02 | GPI1 enabled |
| INTF_CFG4 | 0x13 | 0x0C (analog) / 0x8C (PDM) | PDM_DIN1_SEL=GPI1 |
| PASI_CFG0 | 0x1A | 0x30 | TDM, 32-bit, BCLK_POL=0, FSYNC_POL=0 |
| TX_CH1_CFG | 0x1E | 0x20\|slot | CH_EN + slot assignment |
| TX_CH2_CFG | 0x1F | 0x20\|slot+1 | CH_EN + slot assignment |
| RX_CH1_CFG | 0x28 | 0x20\|slot | CH_EN + slot assignment |
| RX_CH2_CFG | 0x29 | 0x20\|slot+1 | CH_EN + slot assignment |
| CLK_CFG2 | 0x34 | 0x40 | AUTO_PLL_FR_ALLOW, CLK_SRC=BCLK |
| CH_EN | 0x76 | 0xCC | IN_CH1+2 + OUT_CH1+2 enabled |
| PWR_CFG | 0x78 | 0xE0 | ADC + DAC + MICBIAS powered |

### Points critiques de configuration

#### TX_OFFSET et RX_OFFSET = 1

Le SAI7 utilise le format DSP_A avec FSE=1 (Frame Sync Early). Le FSYNC est 1 BCLK avant les données. Les TAC doivent décaler leur sortie/entrée de 1 BCLK pour s'aligner.

- TX_OFFSET=0 → les données TAC commencent au FSYNC → décalé de 1 slot par rapport au SAI
- TX_OFFSET=1 → les données TAC commencent 1 BCLK après FSYNC → aligné avec le SAI

#### TX_FILL = Hi-Z (bit 6 de PASI_TX_CFG0)

Avec 4 TAC sur un bus DOUT partagé, chaque TAC ne doit driver que ses 2 slots. Pendant les 6 autres slots, le TAC doit passer en Hi-Z (haute impédance) pour ne pas écraser les données des autres TAC.

- TX_FILL=0 (défaut) : drive LOW pendant les slots inactifs → **conflit de bus !**
- TX_FILL=1 : Hi-Z pendant les slots inactifs → **correct**

#### DOUT_DRV = push-push (bits 2:0 de INTF_CFG1)

Avec 4 TAC, la capacité parasite cumulée ralentit les transitions. Le driver doit être assez fort pour charger rapidement la ligne.

- DRV=010 (défaut) : active low + weak high → front montant lent
- DRV=001 : active low + active high (push-push) → fronts rapides

#### TX_KEEPER = 01 sur TAC0 uniquement

Le bus keeper maintient le dernier état du bus entre les transitions de slots (quand tous les TAC sont en Hi-Z). Seul le TAC le plus proche du SAI (TAC0) doit avoir le keeper activé.

#### TX_LSB = 0 à 48 kHz

TX_LSB ajoute un demi-cycle de retard sur le dernier bit de chaque slot. Utile uniquement pour BCLK > 18.5 MHz (96 kHz+). À 48 kHz (BCLK = 12.288 MHz), ce retard n'est pas nécessaire et peut causer des problèmes de timing.

#### Reset séquentiel

Avec la clock continue (TX ne s'arrête jamais), un reset simultané des 4 TAC cause un conflit DOUT : après SW_RESET, tous les TAC ont CH1=slot 0 et CH2=slot 1 avec TX_FILL=0. Ils se battent tous sur les mêmes slots.

Solution : reset un TAC à la fois. Pendant qu'un TAC se reconfigure, les 3 autres continuent de fonctionner normalement.

---

## 7. Configuration du Device Tree

### Script apply-tac5212-dt.py

Ce script modifie `imx8mp-evk.dts` pendant le build du kernel. Modifications principales :

#### 7.1 Sound card SOF

```dts
sof-sound-tac5212 {
    compatible = "simple-audio-card";
    label = "tac5212-tdm";
    simple-audio-card,dai-link@0 {
        link-name = "tac5212-hifi";
        format = "dsp_a";
        dai-tdm-slot-num = <8>;
        dai-tdm-slot-width = <32>;
        bitclock-master = <&sndcpu>;
        frame-master = <&sndcpu>;
        sndcpu: cpu {
            sound-dai = <&dsp 1>;
        };
        codec {
            sound-dai = <&tac0>, <&tac1>, <&tac2>, <&tac3>;
        };
    };
};
```

#### 7.2 DSP node

```dts
&dsp {
    #sound-dai-cells = <1>;
    compatible = "fsl,imx8mp-dsp";
    reg = <0x0 0x3B6E8000 0x0 0x88000>;
    pinctrl-names = "default";
    pinctrl-0 = <&pinctrl_sai7>;
    power-domains = <&audiomix_pd>;
    assigned-clocks = <&clk IMX8MP_CLK_SAI7>;
    assigned-clock-parents = <&clk IMX8MP_AUDIO_PLL1_OUT>;
    assigned-clock-rates = <12288000>;
    /* clocks: ipg, ocram, core, sai7_bus, sai7_mclk, sdma3_root */
    tplg-name = "sof-imx8mp-tac5212.tplg";
    machine-drv-name = "asoc-simple-card";
    status = "okay";
};
```

#### 7.3 SAI7 désactivé (SOF le contrôle)

```dts
&sai7 {
    status = "disabled";
};
```

#### 7.4 pinctrl_sai7 avec pins RX

```dts
pinctrl_sai7: sai7grp {
    fsl,pins = <
        MX8MP_IOMUXC_ECSPI2_SCLK__AUDIOMIX_SAI7_TX_BCLK00   0x1c4
        MX8MP_IOMUXC_ECSPI2_MOSI__AUDIOMIX_SAI7_TX_DATA00    0x1c4
        /* RX_DATA */
        0x1E8 0x448 0x534 0x13 0x1                            0x1c4
        /* ECSPI1_SCLK = SAI7_RX_SYNC (ALT3+SION, input_sel 0x538=1) */
        0x1E0 0x440 0x538 0x13 0x1                            0x1c4
        /* ECSPI1_MOSI = SAI7_RX_BCLK (ALT3+SION, input_sel 0x530=1) */
        0x1E4 0x444 0x530 0x13 0x1                            0x1c4
    >;
};
```

#### 7.5 TX_SYNC dans le hog group

TX_SYNC est dans le hog group (appliqué au boot) plutôt que dans pinctrl_sai7, car le pinctrl DSP ne l'applique pas assez tôt.

#### 7.6 ECSPI1 pinctrl retiré

Les pads ECSPI1 (SCLK, MOSI, MISO) sont utilisés pour SAI7 RX. La référence pinctrl de ECSPI1 est supprimée pour éviter les conflits.

---

## 8. Build du firmware SOF

### Prérequis

- Python 3.8+ avec modules `west`, `anytree`, `pyelftools`
- Zephyr SDK 0.17.2 avec toolchain `xtensa-nxp_imx8m_adsp_zephyr-elf`
- cmake, ninja

### Installation du toolchain Xtensa

```bash
# Si le toolchain n'est pas dans le SDK :
SDKVER="0.17.2"
wget -O /tmp/xtensa.tar.xz \
  "https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v${SDKVER}/toolchain_linux-x86_64_xtensa-nxp_imx8m_adsp_zephyr-elf.tar.xz"
cd ~/zephyr-sdk-${SDKVER}
tar xf /tmp/xtensa.tar.xz
```

### Clone et build

```bash
# Clone depuis le mirror local Yocto
git clone --branch v2.10 \
  downloads/git2/github.com.thesofproject.sof.git /tmp/sof

# Appliquer les modifications à src/drivers/imx/sai.c
# (voir section 5 pour les changements)

# Initialiser west
cd /tmp
west init -l sof
west update

# Build
python3 sof/scripts/xtensa-build-zephyr.py imx8m
```

### Sortie

```
/tmp/build-sof-staging/sof/imx/sof/community/sof-imx8m.ri
```

---

## 9. Procédure de déploiement

### Depuis la machine de build

```bash
DEPLOY=Model_AB_Infinity/tmp/deploy/images/imx8mpevk
BOARD=root@192.168.0.9

# 1. Kernel + DTB
scp $DEPLOY/Image $BOARD:/boot/Image
scp $DEPLOY/imx8mp-evk.dtb $BOARD:/boot/imx8mp-evk.dtb

# 2. Module TAC5212
DEB=$(ls $DEPLOY/../deb/imx8mpevk/kernel-module-snd-soc-tac5212-*.deb | head -1)
scp "$DEB" $BOARD:/tmp/tac5212.deb
ssh $BOARD "dpkg -i /tmp/tac5212.deb"

# 3. Firmware SOF
scp /tmp/build-sof-staging/sof/imx/sof/community/sof-imx8m.ri \
    $BOARD:/lib/firmware/imx/sof/sof-imx8m.ri

# 4. tac-reset
scp meta-local/recipes-kernel/linux/files/tac-reset.sh $BOARD:/usr/bin/tac-reset
ssh $BOARD "chmod +x /usr/bin/tac-reset"

# 5. Reboot
ssh $BOARD "sync && reboot"
```

### Après le reboot

```bash
# Vérifier que SOF a chargé
dmesg | grep sof
# → sof-audio-of-imx8m 3b6e8000.dsp: Firmware info: version 2:10:0-b15f1

# Vérifier la carte son
cat /proc/asound/cards
# → 2 [softac5212tdm]: simple-card - sof-tac5212-tdm

# Premier tac-reset (une seule fois après le boot)
arecord -D hw:2,0 -c 8 -f S32_LE -r 48000 -d 8 /tmp/init.wav &
sleep 1; tac-reset; wait

# Enregistrer
arecord -D hw:2,0 -c 8 -f S32_LE -r 48000 -d 30 /tmp/recording.wav
```

---

## 10. Résultats et validation

### Mesures de bruit (10 runs x 30 secondes, entrées non connectées)

```
Run     Ch0      Ch1      Ch2      Ch3      Ch4      Ch5      Ch6      Ch7
----------------------------------------------------------------------------
  1   -68.3  -107.5  -121.1  -121.8  -121.7  -121.5  -120.8  -118.4
  2   -68.6  -107.9  -121.3  -121.7  -122.0  -121.7  -120.7  -118.2
  3   -68.6  -107.8  -121.1  -121.7  -121.9  -121.5  -120.8  -118.4
  4   -68.4  -107.5  -121.2  -121.7  -121.9  -121.7  -120.8  -118.1
  5   -68.4  -107.6  -121.2  -121.6  -122.1  -121.7  -120.8  -118.0
  6   -68.3  -107.5  -121.0  -121.7  -121.9  -121.5  -120.7  -118.2
  7   -68.3  -107.6  -121.3  -121.7  -121.8  -121.7  -120.7  -118.3
  8   -68.3  -107.5  -121.3  -121.7  -122.0  -121.6  -120.7  -118.3
  9   -68.4  -107.8  -121.3  -121.7  -121.8  -121.5  -120.7  -118.2
 10   -68.6  -107.7  -121.3  -121.6  -121.9  -121.5  -120.7  -118.2
----------------------------------------------------------------------------
 AVG  -68.4  -107.6  -121.2  -121.7  -121.9  -121.6  -120.7  -118.2
```

### Interprétation

- **Ch0** (-68.4 dBFS) : micro électret connecté sans préamplificateur → signal attendu
- **Ch1** (-107.6 dBFS) : micro sans préamp, niveau plus bas → normal
- **Ch2-Ch5** (-121 à -122 dBFS) : plancher de bruit théorique du TAC5212 (119 dB)
- **Ch6-Ch7** (-118 à -120 dBFS) : légèrement plus haut, possiblement dû à la position de TAC3 en bout de bus
- **Écart-type** : < 0.3 dB entre les runs → **parfaitement stable**

### Comparaison avant/après

| | SYNC (avant) | ASYNC (après) | Amélioration |
|--|-------------|--------------|--------------|
| Ch2-Ch7 | -58 dBFS | -121 dBFS | **+63 dB** |
| Stabilité | aléatoire | < 0.3 dB | **stable** |
| Warmup | 2-3 tentatives | aucun | **immédiat** |
| tac-reset | à chaque record | une fois au boot | **simplifié** |

---

## 11. Problèmes rencontrés et solutions

### 11.1 I/O error au démarrage du recording

**Symptôme** : `arecord: pcm_read:2272: read error: Input/output error` sur les 2-3 premières tentatives après boot.

**Cause** : Le pipeline SOF met du temps à s'initialiser. Le premier PCM open/start peut échouer.

**Solution** : Avec le firmware ASYNC + clock continue, ce problème a disparu. Le premier recording fonctionne directement.

### 11.2 Crash de la carte pendant les tests

**Symptôme** : La carte reboot spontanément pendant les opérations mmap sur SAI7.

**Cause** : Écriture concurrente sur les registres SAI7 (Linux via mmap + DSP) pendant un stream actif.

**Solution** : Ne jamais écrire dans les registres SAI7 via mmap pendant un stream. Toutes les modifications SAI sont dans le firmware SOF. Cause secondaire identifiée : problème d'alimentation résolu par l'utilisateur.

### 11.3 Bruit aléatoire entre les recordings (-24 dBFS sur certains canaux)

**Symptôme** : Après tac-reset, certains canaux montrent -120 dBFS et d'autres -24 dBFS, de façon aléatoire.

**Cause** : Deux facteurs combinés :
1. Le driver kernel tac5212.c sur la carte avait les anciennes valeurs (TX_CFG0=0x68 avec TX_LSB=1). Le hw_params du driver écrasait les valeurs du tac-reset.
2. Les input select registers (0x530, 0x538) pour RX_BCLK et RX_FSYNC n'étaient pas configurés dans le DT. Le SAI7 RX ne recevait pas toujours le bon signal d'horloge.

**Solution** : Recompiler le kernel avec le driver tac5212.c corrigé + ajouter les input select dans apply-tac5212-dt.py.

### 11.4 DOUT bus contention avec clock continue

**Symptôme** : Après un tac-reset simultané des 4 TAC avec clock continue, tous les TAC se battent sur les slots 0-1 (valeurs par défaut après SW_RESET).

**Cause** : Après SW_RESET, les registres reviennent aux défauts : CH1=slot 0, CH2=slot 1, TX_FILL=0 (drive LOW pendant les slots inactifs). Avec la clock continue, les 4 TAC transmettent immédiatement sur les mêmes slots.

**Solution** : Reset séquentiel — un TAC à la fois. Pendant qu'un TAC se reconfigure, les 3 autres continuent de fonctionner normalement.

### 11.5 Firmware SOF pre-built (binaire NXP)

**Symptôme** : Impossible de modifier le comportement SYNC/ASYNC du SAI7 sans toucher au firmware.

**Cause** : La recette Yocto `sof-zephyr_2.10.0.bb` installe un binaire pré-compilé par NXP. Pas de compilation depuis les sources.

**Solution** : Build custom depuis les sources SOF v2.10 avec le Zephyr SDK + toolchain Xtensa. Le firmware résultant est déployé dans `/lib/firmware/imx/sof/sof-imx8m.ri`.

### 11.6 Noms de pins inversés dans le header i.MX8MP

**Symptôme** : Les pins RX_BCLK et RX_FSYNC étaient inversées dans le DT, causant une capture instable.

**Cause** : Les macros du header `imx8mp-pinfunc.h` ont des noms contre-intuitifs :
- `ECSPI1_SCLK` → `SAI7_RX_SYNC` (on s'attend à RX_BCLK)
- `ECSPI1_MOSI` → `SAI7_RX_BCLK` (on s'attend à RX_SYNC)

**Solution** : Toujours vérifier les définitions exactes dans le header, ne pas supposer d'après les noms des pads.

### 11.7 Patches fsl_sai inutiles avec SOF

**Symptôme** : 3 patches sed sur fsl_sai.c dans linux-imx_%.bbappend pour le mode ASYNC ALSA.

**Cause** : En mode SOF, le driver fsl_sai Linux est désactivé (SAI7 `status = "disabled"`). Les patches ne servent plus et modifient le code de fsl_sai pour d'autres SAI instances (risque de bugs).

**Solution** : Suppression des 3 patches sed. Le mode ASYNC est maintenant géré par le firmware SOF custom.

---

## 12. Registres de référence

### SAI7 (base 0x30C80000, avec SAI_OFS=8 pour i.MX8MP)

| Registre | Offset | Description |
|----------|--------|-------------|
| TCSR | 0x08 | TX Control/Status |
| TCR1 | 0x0C | TX Config 1 (watermark) |
| TCR2 | 0x10 | TX Config 2 (BCP, BCD, MSEL, DIV) |
| TCR3 | 0x14 | TX Config 3 (TRCE) |
| TCR4 | 0x18 | TX Config 4 (FCONT, FRSZ, SYWD, FSE, FSD) |
| TCR5 | 0x1C | TX Config 5 (WNW, W0W, FBT) |
| TDR0 | 0x20 | TX Data Register |
| TMR | 0x60 | TX Mask |
| RCSR | 0x88 | RX Control/Status |
| RCR2 | 0x90 | RX Config 2 (SYNC, BCP, BCD) |
| RCR3 | 0x94 | RX Config 3 (TRCE) |
| RCR4 | 0x98 | RX Config 4 (FCONT, FSD) |
| RCR5 | 0x9C | RX Config 5 |
| RDR0 | 0xA0 | RX Data Register |
| RMR | 0xE0 | RX Mask |
| MCTL | 0x100 | MCLK Control |

### TAC5212 (page 0)

Voir section 6 pour la liste complète des registres configurés.

### CLK_DET_STS (status, read-only)

| Registre | Adresse | Contenu attendu |
|----------|---------|-----------------|
| CLK_DET_STS0 | 0x3E | 0x50 = 48 kHz détecté |
| CLK_DET_STS2/3 | 0x40/0x41 | 0x81/0x00 = ratio 256 (8x32) |
| DEV_STS1 | 0x7A | 0xF8 = PLL verrouillé |
| CLK_ERR_STS0/1 | 0x3C/0x3D | 0x00/0x00 = pas d'erreur |

---

## 13. Fichiers modifiés

### Dans le repo Yocto (meta-local/)

| Fichier | Modifications |
|---------|--------------|
| `recipes-kernel/linux/files/tac5212.c` | TX_CFG0: 0x68→0x48, 0x60→0x40 (TX_LSB=0) |
| `recipes-kernel/linux/files/tac5212.h` | Pas de changement fonctionnel |
| `recipes-kernel/linux/files/tac-reset.sh` | Reset séquentiel, TX_FILL=1, DOUT_DRV=push-push, TX_OFFSET=1 |
| `recipes-kernel/linux/files/apply-tac5212-dt.py` | Ajout RX_BCLK + RX_FSYNC pins avec input select |
| `recipes-kernel/linux/linux-imx_%.bbappend` | Suppression des 3 patches fsl_sai |

### Firmware SOF custom (hors repo)

| Fichier | Emplacement |
|---------|-------------|
| Source modifié | `/tmp/sof/src/drivers/imx/sai.c` |
| Firmware compilé | `/tmp/build-sof-staging/sof/imx/sof/community/sof-imx8m.ri` |
| Installé sur cible | `/lib/firmware/imx/sof/sof-imx8m.ri` |
| Backup original | `/lib/firmware/imx/sof/sof-imx8m.ri.bak` |

---

*Document généré le 2026-04-04. Commit: 25d11fea sur feature/audio-platform-v2.*
