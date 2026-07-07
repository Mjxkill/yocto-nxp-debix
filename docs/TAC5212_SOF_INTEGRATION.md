# Intégration 4x TAC5212 + SOF HiFi4 DSP sur i.MX8MP

## Document technique complet

**Plateforme** : Debix Model AB (NXP i.MX8MP)
**Kernel** : 6.6.36
**SOF Firmware** : 2.10.0 (sof-imx8m.ri)
**Codecs** : 4x TI TAC5212 (I2C 0x50-0x53)
**Interface** : SAI7 TDM-8, 48kHz, 32-bit, DSP_A

---

## 1. Architecture Matérielle

```
                    ┌──────────────────────────────┐
                    │        i.MX8MP SoC            │
                    │                                │
                    │  ┌──────────┐  ┌──────────┐   │
                    │  │ AudioMix │  │ DSP      │   │
   4x TAC5212 ◄────┼──┤  SAI7    ├──┤ HiFi4    │   │
   I2C 0x50-53 ────┼──┤  TDM-8   │  │ (SOF)    │   │
                    │  │  SDMA3   │  │ 800MHz   │   │
                    │  └──────────┘  └──────────┘   │
                    └──────────────────────────────┘
```

### Bus TDM
- **Format** : DSP_A (Frame Sync Early = 1 BCLK avant les données)
- **Slots** : 8 × 32 bits = 256 BCLK par frame
- **BCLK** : 12.288 MHz (256 × 48kHz)
- **FSYNC** : 48 kHz
- **SAI7** : TX = master (génère BCLK + FSYNC), RX = consumer (sync interne)

### Allocation des slots TDM

| TAC | I2C | ADC→DOUT (capture) | DIN→DAC (playback) |
|-----|-----|--------------------|--------------------|
| TAC0 | 0x50 | slot 0, 1 | slot 0, 1 |
| TAC1 | 0x51 | slot 2, 3 | slot 2, 3 |
| TAC2 | 0x52 | slot 4, 5 | slot 4, 5 |
| TAC3 | 0x53 | slot 6, 7 | slot 6, 7 |

Calcul automatique dans le driver : `base_slot = (i2c_addr - 0x50) * 2`

### Câblage pins SAI7

| Signal | Pin i.MX8MP | Mux Mode | SION |
|--------|-------------|----------|------|
| TX_SYNC (FSYNC) | ECSPI1_SS0 | 5 | Non |
| TX_BCLK | ECSPI2_SCLK | 3 | Non |
| TX_DATA | ECSPI2_MOSI | 3 | Non |
| RX_SYNC | ECSPI1_SCLK | 3 | Oui (0x13) |
| RX_BCLK | ECSPI1_MOSI | 3 | Oui (0x13) |
| RX_DATA | ECSPI1_MISO | 3 | Oui (0x13) |

Les pins TX et RX pour BCLK/FSYNC sont reliées physiquement sur le PCB.
SION (Software Input ON) sur les pins RX route le signal TX vers l'entrée RX.

---

## 2. Registres SAI7 — Configuration finale

### Transmission (TX) — Master, génère les clocks

| Registre | Adresse | Valeur | Description |
|----------|---------|--------|-------------|
| TCSR | 0x30C80008 | 0x90100001 | TERE=1, FRDE=1 (DMA actif) |
| TCR1 | 0x30C8000C | 0x00000040 | Watermark FIFO = 64 |
| TCR2 | 0x30C80010 | 0x07800000 | BCD=1 (master), BCP=1, BYP=1, MSEL=1 |
| TCR3 | 0x30C80014 | 0x00010000 | TRCE=1 (canal 0 actif) |
| TCR4 | 0x30C80018 | 0x10070039 | FSD=1, FSE=1 (dsp_a), MF=1, FRSZ=7, **FCONT=1** |
| TCR5 | 0x30C8001C | 0x1F1F1F00 | FBT=31, W0W=31, WNW=31 (32-bit) |
| TMR | 0x30C80060 | 0xFFFFFF00 | Mask: slots 0-7 actifs |

### Réception (RX) — Consumer, sync avec TX

| Registre | Adresse | Valeur | Description |
|----------|---------|--------|-------------|
| RCSR | 0x30C80088 | 0x90100001 | TERE=1, FRDE=1 |
| RCR1 | 0x30C8008C | 0x00000040 | Watermark FIFO = 64 |
| RCR2 | 0x30C80090 | 0x06000000 | **BCD=0** (consumer), BCP=1, SYNC=1 |
| RCR3 | 0x30C80094 | 0x00010000 | TRCE=1 (canal 0 actif) |
| RCR4 | 0x30C80098 | 0x10070038 | **FSD=0** (consumer), FSE=1, FRSZ=7, **FCONT=1** |
| RCR5 | 0x30C8009C | 0x1F1F1F00 | FBT=31, W0W=31, WNW=31 |
| RMR | 0x30C800E0 | 0xFFFFFF00 | Mask: slots 0-7 actifs |

### Points critiques SAI7

1. **FCONT (bit 28 de TCR4/RCR4)** : OBLIGATOIRE. Sans FCONT, le bruit de fond
   passe de -121 dB à -32 dB. FCONT maintient la synchronisation frame même
   en cas d'erreur transitoire.

2. **RCR2 BCD=0** : Le firmware SOF configure RCR2 avec BCD=1 (master).
   Comme les pins TX et RX sont reliées physiquement, deux masters se battent
   sur le bus → signal corrompu → PLL TAC ne lock pas.
   **Fix** : forcer BCD=0 dans RCR2 après la configuration SOF.

3. **RCR4 FSD=0** : Même logique que BCD — le RX ne doit pas driver FSYNC.

4. **SYNC=1 dans RCR2** : Le RX utilise les clocks TX internes. Nécessaire
   avec SOF car le firmware ne supporte pas le mode async sur l'i.MX8MP.

---

## 3. Registres TAC5212 — Configuration complète

### Séquence d'initialisation (probe)

| # | Registre | Adresse | Valeur | Description |
|---|----------|---------|--------|-------------|
| 1 | SW_RESET | 0x01 | 0x01 | Reset logiciel |
| 2 | DEV_MISC_CFG | 0x02 | 0x09 | SLEEP_ENZ + SLEEP_EXIT_VREF_EN |
| 3 | INTF_CFG1 | 0x10 | 0x53 | DOUT=PASI DOUT, Hi-Z inactif (DOUT_DRV=3) |
| 4 | INTF_CFG2 | 0x11 | 0x80 | PASI_DIN_EN=1 |
| 5 | ASI_CFG0 | 0x18 | 0x40 | SASI_DIS=1 (Secondary ASI désactivé) |
| 6 | MISC_CFG | 0x04 | 0x40 | IGNORE_CLK_ERR=1 |
| 7 | PASI_TX_CFG0 | 0x1B | 0x68 | TX_FILL=1, TX_KEEPER=1, TX_LSB=1 (AN sbaa383c) |
| 8 | PASI_TX_CFG1 | 0x1C | 0x01 | **TX_OFFSET=1** (alignement dsp_a) |
| 9 | PASI_RX_CFG0 | 0x26 | 0x01 | **RX_OFFSET=1** (alignement DAC dsp_a) |
| 10 | GPO1_CFG0 | 0x0C | 0x41 | PDMCLK output, active drive |
| 11 | GPI_CFG | 0x0D | 0x02 | GPI1 enabled (PDM data input) |
| 12 | INTF_CFG4 | 0x13 | 0x0C | PDM_DIN1_SEL=GPI1 (défaut analog) |
| 13 | PASI_CFG0 | 0x1A | 0x30 | TDM mode, 32-bit word length |
| 14 | TX_CH1_CFG | 0x1E | 0x20+slot | CH_EN=1, slot=base_slot |
| 15 | TX_CH2_CFG | 0x1F | 0x21+slot | CH_EN=1, slot=base_slot+1 |
| 16 | RX_CH1_CFG | 0x28 | 0x20+slot | CH_EN=1, slot=base_slot |
| 17 | RX_CH2_CFG | 0x29 | 0x21+slot | CH_EN=1, slot=base_slot+1 |
| 18 | CLK_CFG2 | 0x34 | 0x40 | PLL enabled, AUTO_PLL_FR_ALLOW |
| 19 | CH_EN | 0x76 | 0xCC | IN_CH1+IN_CH2+OUT_CH1+OUT_CH2 |
| 20 | PWR_CFG | 0x78 | 0xE0 | ADC_PDZ + DAC_PDZ + MICBIAS_PDZ |

### TX_OFFSET et RX_OFFSET — Pourquoi 1 ?

En mode TDM DSP_A, le SAI7 envoie les données avec FSE=1 (Frame Sync Early) :
le FSYNC pulse 1 BCLK avant le premier bit de données. Le TAC5212 doit
compenser ce décalage :

- **TX_OFFSET=1** : le TAC envoie ses données ADC 1 BCLK après FSYNC,
  aligné avec le timing DSP_A du SAI7.
- **RX_OFFSET=1** : le TAC lit les données DAC 1 BCLK après FSYNC.

Sans ces offsets, le slot 0 est corrompu (Ch1 bloqué à 0xFFFFFFFF).

### INTF_CFG4 — Sélection Analog/PDM

```
Bit 7: PDM_CH1_SEL    — 0=Analog, 1=PDM pour CH1
Bit 6: PDM_CH2_SEL    — 0=Analog, 1=PDM pour CH2
Bit 5: PDMDIN1_EDGE   — 0=CH1 neg edge/CH2 pos edge
Bit 4: PDMDIN2_EDGE
Bit 3-2: PDM_DIN1_SEL — 00=disabled, 11=GPI1
Bit 1-0: PDM_DIN2_SEL
```

**Bug corrigé** : les defines PDM_DIN1_SEL et PDM_DIN2_SEL étaient inversés
dans le header (shift 0 au lieu de 2). Résultat : le PDM était routé vers
les canaux 3-4 au lieu de 1-2.

### Contrôles ALSA exposés (par TAC, avec sound-name-prefix)

- ADC1/ADC2 Digital Volume (-80 à +47 dB)
- DAC1A/1B/2A/2B Digital Volume
- ADC HPF Cutoff (1Hz, 12Hz, 96Hz)
- ADC Biquad Config (1-3 biquads/canal)
- ADC Decimation Filter
- Noise Gate, Distortion Limiter
- Output Drive (Line/Headphone)
- VREF, MICBIAS
- CH1/CH2 PDM Select (0=Analog, 1=PDM)

---

## 4. Problème PLL Lock et solution

### Le problème

Le TAC5212 utilise une PLL interne qui se verrouille sur le BCLK externe.
Au démarrage (probe du driver), le BCLK n'est pas encore actif car le SAI7
n'a pas démarré. La PLL ne peut pas locker → erreur 0x90 (BCLK_FS_RATIO_ERR).

### Symptômes
- `DEV_STS1` (0x7A) bit 4 = 0 (PLL non verrouillée)
- `CLK_ERR_STS1` (0x3D) = 0x10 (BCLK_FS_RATIO_ERR)
- Bruit blanc sur les canaux PDM
- Données ADC incorrectes ou nulles

### Solution : SW Reset avec BCLK présent

Le driver fait un SW reset au premier `hw_params` (flag `needs_reset`).
À ce moment, le SAI7 est configuré et BCLK est actif. Le TAC5212 peut
détecter le BCLK et locker sa PLL.

Avec SOF, le `hw_params` du codec ne s'exécute pas directement. Le script
`tac-reset` fait le SW reset manuellement pendant qu'un stream SOF tourne.

### Temps de stabilisation PLL

Après le SW reset avec BCLK présent :
- 0-5 secondes : bruit de fond élevé (-32 à -86 dB)
- Après 5 secondes : bruit de fond nominal (-94 à -122 dB)

---

## 5. SOF (Sound Open Firmware) — Intégration

### Architecture SOF

```
Application Linux → ALSA PCM "sof-tac5212-tdm"
                         ↓ (mémoire partagée)
                    DSP HiFi4 (firmware SOF 2.10.0)
                         ↓ (SDMA3)
                    SAI7 FIFO TX/RX
                         ↓ (bus TDM physique)
                    4× TAC5212
```

### Device Tree — Noeud DSP

Le DSP remplace le driver fsl_sai pour le contrôle du SAI7 :

```dts
&dsp {
    #sound-dai-cells = <1>;
    compatible = "fsl,imx8mp-dsp";
    reg = <0x0 0x3B6E8000 0x0 0x88000>;
    pinctrl-names = "default";
    pinctrl-0 = <&pinctrl_sai7>;    /* Pins muxées par le DSP */
    power-domains = <&audiomix_pd>;
    assigned-clocks = <&clk IMX8MP_CLK_SAI7>;
    assigned-clock-parents = <&clk IMX8MP_AUDIO_PLL1_OUT>;
    assigned-clock-rates = <12288000>;
    clocks = <&audio_blk_ctrl IMX8MP_CLK_AUDIOMIX_OCRAMA_IPG>,
             <&audio_blk_ctrl IMX8MP_CLK_AUDIOMIX_DSP_ROOT>,
             <&audio_blk_ctrl IMX8MP_CLK_AUDIOMIX_DSPDBG_ROOT>,
             <&audio_blk_ctrl IMX8MP_CLK_AUDIOMIX_SAI7_IPG>,
             <&audio_blk_ctrl IMX8MP_CLK_AUDIOMIX_SAI7_MCLK1>,
             <&audio_blk_ctrl IMX8MP_CLK_AUDIOMIX_SDMA3_ROOT>;
    clock-names = "ipg", "ocram", "core",
                  "sai7_bus", "sai7_mclk", "sdma3_root";
    mbox-names = "txdb0", "txdb1", "rxdb0", "rxdb1";
    mboxes = <&mu2 2 0>, <&mu2 2 1>,
             <&mu2 3 0>, <&mu2 3 1>;
    memory-region = <&dsp_reserved>;
    /delete-property/ firmware-name;
    tplg-name = "sof-imx8mp-tac5212.tplg";
    machine-drv-name = "asoc-simple-card";
    syscon = <&audio_blk_ctrl>;
    status = "okay";
};
```

### Device Tree — Carte son SOF

```dts
sof-sound-tac5212 {
    compatible = "simple-audio-card";
    label = "tac5212-tdm";
    simple-audio-card,dai-link@0 {
        link-name = "tac5212-hifi";
        format = "dsp_a";
        dai-tdm-slot-num = <8>;
        dai-tdm-slot-width = <32>;
        bitclock-master = <&sndcpu>;   /* DSP est master */
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

### Noeuds désactivés

```dts
&sai7 { status = "disabled"; };  /* SAI7 contrôlé par DSP, pas fsl_sai */
&sdma3 { status = "disabled"; }; /* SDMA3 contrôlé par DSP, pas Linux */
```

### Clock Gates — Problème et solution

Quand SAI7 est `status = "disabled"`, Linux ne lui assigne pas de clocks.
Les clock gates AudioMix pour SAI7 restent fermées → le BCLK ne sort pas
physiquement même si TERE=1.

**Solution** : Le noeud TAC0 dans le DT déclare les clocks SAI7 comme
consumer. Le driver `tac5212.c` les active au probe via
`devm_clk_bulk_get_all` + `clk_bulk_prepare_enable`. Les clock gates
restent ouvertes en permanence.

```dts
tac0: audio-codec@50 {
    compatible = "ti,tac5212";
    reg = <0x50>;
    clocks = <&audio_blk_ctrl IMX8MP_CLK_AUDIOMIX_SAI7_IPG>,
             <&audio_blk_ctrl IMX8MP_CLK_AUDIOMIX_SAI7_MCLK1>;
    clock-names = "sai-ipg", "sai-mclk";
};
```

### Topologie SOF (.tplg)

Fichier source M4 : `sof-imx8mp-tac5212.m4`
Compilé avec : `m4 ... | alsatplg -c - -o sof-imx8mp-tac5212.tplg`

Pipeline :
```
Host PCM 0 (capture) ← Volume ← SAI7 RX (8ch, s32le)
Host PCM 1 (playback) → Volume → SAI7 TX (8ch, s32le)
```

Configuration DAI :
```
DAI_CONFIG(SAI, 7, 0, tac5212-hifi,
    SAI_CONFIG(DSP_A,
        SAI_CLOCK(mclk, 12288000, codec_mclk_in),
        SAI_CLOCK(bclk, 12288000, codec_consumer),
        SAI_CLOCK(fsync, 48000, codec_consumer),
        SAI_TDM(8, 32, 255, 255),
        SAI_CONFIG_DATA(SAI, 7, 0)))
```

---

## 6. Problème Mode SYNC vs ASYNC

### Le problème

Le firmware SOF configure SAI7 RX en mode **SYNC** (RCR2 bit 30 = 1) avec
**BCD=1** (master). Comme les pins TX et RX sont physiquement reliées sur
le PCB, les deux drivers (TX et RX) se battent sur le bus BCLK/FSYNC :

```
TX BCLK (output, BCD=1) ──┬── fil physique
RX BCLK (output, BCD=1) ──┘   ← CONFLIT !
```

Résultat : signal corrompu, PLL TAC ne lock pas.

### La solution

Forcer RCR2 BCD=0 et RCR4 FSD=0 après la configuration SOF :

```
TX BCLK (output, BCD=1) ──┬── fil physique
RX BCLK (input, BCD=0)  ──┘   ← OK, RX écoute
```

Le mode SYNC reste actif (RCR2 bit 30 = 1) : le RX utilise les clocks
TX internes pour le sampling, pas les pins physiques.

### Impact FCONT

Sans FCONT (bit 28 de TCR4/RCR4), le bruit de fond est de -32 à -55 dB.
Avec FCONT : -118 à -121 dB. FCONT maintient la synchronisation frame
même en cas de glitch transitoire.

---

## 7. PDM Microphones

### Configuration

- 2 micros PDM par TAC (8 total)
- GPO1 → PDMCLK (3.072 MHz)
- GPI1 → PDM data
- Micro L/R=GND → CH1 (falling edge)
- Micro L/R=VDD → CH2 (rising edge)

### Bug corrigé

Les defines `PDM_DIN1_SEL_SHIFT` et `PDM_DIN2_SEL_SHIFT` étaient inversés
dans `tac5212.h`. DIN1 (bits 3:2) et DIN2 (bits 1:0) étaient swappés,
routant le PDM vers les canaux 3-4 au lieu de 1-2.

### DC Offset PDM

Le décodeur PDM du TAC5212 produit un signal avec un DC offset négatif
important. Le HPF interne (12Hz ou 96Hz) le corrige en temps réel.

---

## 8. Script tac-reset

Usage : `tac-reset [pdm|analog|all|allpdm] [i2c_addr]`

Le script :
1. Fixe SAI7 RCR2 (BCD=0) et RCR4 (FSD=0)
2. Fait un SW reset du TAC5212
3. Reconfigure tous les registres
4. Le PLL se verrouille sur le BCLK actif

À exécuter après le démarrage d'un stream SOF (BCLK présent).

---

## 9. Performances mesurées

### Bruit de fond (entrées non connectées, après stabilisation PLL 5s)

| Canal | 48 kHz | 44.1 kHz | 96 kHz |
|-------|--------|----------|--------|
| Ch1 (TAC0) | -116 dB | -116 dB | -116 dB |
| Ch2 (TAC0) | -116 dB | -116 dB | -116 dB |
| Ch3 (TAC1) | -121 dB | -99 dB | -121 dB |
| Ch4 (TAC1) | -121 dB | -99 dB | -122 dB |
| Ch5 (TAC2) | -98 dB | -98 dB | -122 dB |
| Ch6 (TAC2) | -99 dB | -99 dB | -122 dB |
| Ch7 (TAC3) | -120 dB | -94 dB | -121 dB |
| Ch8 (TAC3) | -118 dB | -95 dB | -118 dB |

### Via SOF DSP (48 kHz, après FCONT fix)

| Canal | RMS (dB) |
|-------|----------|
| Ch1 | -70 (mic sans préamp) |
| Ch2 | -109 |
| Ch3 | -121 |
| Ch4 | -121 |
| Ch5 | -121 |
| Ch6 | -121 |
| Ch7 | -121 |
| Ch8 | -118 |

---

## 10. Fichiers du projet

| Fichier | Rôle |
|---------|------|
| `meta-local/recipes-kernel/linux/files/tac5212.c` | Driver ASoC codec |
| `meta-local/recipes-kernel/linux/files/tac5212.h` | Defines registres |
| `meta-local/recipes-kernel/linux/files/apply-tac5212-dt.py` | Modifications DT |
| `meta-local/recipes-kernel/linux/linux-imx_%.bbappend` | Intégration Yocto + patches fsl_sai |
| `meta-local/recipes-kernel/linux/files/sof-imx8mp-tac5212.m4` | Topologie SOF |
| `meta-local/recipes-kernel/linux/files/tac-reset.sh` | Script reset PLL + fix SAI7 |
| `meta-local/recipes-fsl/images/imx-image-full.bbappend` | Paquets SOF dans l'image |

---

## 11. Patches fsl_sai (kernel Linux)

### Patch 1 : Async RX Consumer Fix
Force RX en consumer (BCD=0, FSD=0) en mode async pour que le RX
reçoive les clocks depuis les pins physiques au lieu de les driver.

### Patch 2 : Async Capture TX Start
Quand un capture démarre en mode async, active aussi le TX (TERE + TRCE)
pour générer les clocks BCLK/FSYNC.

### Patch 3 : Async Capture TX Config
Configure les registres TX (TCR4/TCR5/TMR) et active TX MCLK dans
`hw_params` quand on fait un capture en mode async. Sans ce patch,
le TX a TERE actif mais TCR4 n'a pas le bon FRSZ → pas de TDM correct.

**Note** : Ces patches sont pour le driver fsl_sai standard (sans SOF).
Avec SOF, le firmware DSP configure le SAI7 directement.
