# TAC5212 TDM Integration - Recap complet

## Architecture hardware

- 4x TI TAC5212 sur I2C bus /dev/i2c-3 (i2c4, i2c@30a50000)
  - TAC0: 0x50, TAC1: 0x51, TAC2: 0x52, TAC3: 0x53
- SAI7 du i.MX8MP (30c80000) en mode TDM 8 slots x 32 bits
- SAI7 = bus master (genere BCLK + FSYNC)
- Tous les TAC = targets (PLL auto-lock sur BCLK)
- Bus TDM partage : BCLK, FSYNC, DIN (SAI→TAC), DOUT (TAC→SAI)

## Cablage pins (connecteur Debix)

| Signal | Pin 1 (TX) | Pin 2 (RX) |
|--------|-----------|-----------|
| FSYNC | ECSPI1_SS0 → SAI7_TX_SYNC | ECSPI1_SCLK → SAI7_RX_SYNC |
| BCLK | ECSPI2_SCLK → SAI7_TX_BCLK | ECSPI1_MOSI → SAI7_RX_BCLK |
| DIN (SAI→TAC) | ECSPI2_MOSI → SAI7_TX_DATA00 | |
| DOUT (TAC→SAI) | ECSPI1_MISO → SAI7_RX_DATA00 | |

Les pins TX et RX pour FSYNC/BCLK sont reliees sur le PCB (meme fil physique).

## Ce qui fonctionne

### Playback 8 canaux
- OK a 44.1, 48, 96 kHz
- Format dsp_a ou dsp_b, les deux marchent pour le playback

### Capture 8 canaux (DMA)
- Fonctionne en mode **async** uniquement (pas sync)
- SAI7 TX = provider (BCD=1, FSD=1), genere les clocks
- SAI7 RX = consumer (BCD=0, FSD=0), recoit les clocks depuis les pins RX
- SION (Software Input ON) necessaire sur les 3 pins RX
- Patch fsl_sai : "async RX consumer fix" force RX en consumer
- Patch fsl_sai : "async capture TX start" demarre TX quand seul le capture s'ouvre
- Le mode sync ne fonctionne PAS pour le capture (FIFO RX reste vide)

### Dual PLL (44.1k + 48k familles)
- pll8k (AUDIO_PLL1_OUT = 393216000 Hz) pour 48/96 kHz
- pll11k (AUDIO_PLL2_OUT = 361267200 Hz) pour 44.1 kHz
- Ajout des clocks pll8k/pll11k dans le DT SAI7
- Le driver SAI reparente automatiquement

### PDM microphones
- 8 micros PDM fonctionnels (2 par TAC, front montant + descendant PDMCLK)
- GPO1 = PDMCLK output (0x41)
- GPI1 = PDM data input (0x02)
- PDM_DIN1_SEL = GPI1 (0x03 dans INTF_CFG4)
- Mux DAPM "CH1 Input Mux" / "CH2 Input Mux" : Analog ou PDM

### 192 kHz
- NON SUPPORTE : BCLK = 49.152 MHz depasse la limite TAC5212 (max 24.576 MHz)
- Retire des rates supportes dans le driver

## Registres TAC5212 cles (configuration qui fonctionne)

| Registre | Addr | Valeur | Description |
|----------|------|--------|-------------|
| DEV_MISC_CFG | 0x02 | 0x09 | SLEEP_ENZ=1, SLEEP_EXIT_VREF_EN=1 |
| MISC_CFG | 0x04 | 0x40 | IGNORE_CLK_ERR=1 |
| INTF_CFG1 | 0x10 | 0x53 | DOUT_SEL=5 (PASI DOUT), DOUT_DRV=3 (Hi-Z inactif) |
| INTF_CFG2 | 0x11 | 0x80 | PASI_DIN_EN=1 |
| INTF_CFG4 | 0x13 | 0xC3 | PDM_CH1_SEL=1, PDM_CH2_SEL=1, PDM_DIN1_SEL=3 (GPI1) |
| ASI_CFG0 | 0x18 | 0x40 | SASI_DIS=1 |
| PASI_CFG0 | 0x1A | 0x30 | TDM mode, 32-bit, BCLK_POL=0, FSYNC_POL=0 |
| PASI_TX_CFG0 | 0x1B | 0x40 | TX_FILL=1 (Hi-Z unused slots) |
| PASI_TX_CFG1 | 0x1C | 0x00 ou 0x01 | TX_OFFSET (voir probleme ci-dessous) |
| TX_CH1_CFG | 0x1E | 0x20+slot | CH_EN=1, slot=base_slot |
| TX_CH2_CFG | 0x1F | 0x21+slot | CH_EN=1, slot=base_slot+1 |
| PASI_RX_CFG0 | 0x26 | 0x00 ou 0x01 | RX_OFFSET |
| RX_CH1_CFG | 0x28 | 0x20+slot | CH_EN=1, slot=base_slot |
| RX_CH2_CFG | 0x29 | 0x21+slot | CH_EN=1, slot=base_slot+1 |
| CLK_CFG0 | 0x32 | 0x00 | Auto-detect sample rate |
| CLK_CFG2 | 0x34 | 0x40 | PLL enabled, AUTO_PLL_FR_ALLOW=1 |
| CNT_CLK_CFG2 | 0x37 | 0x20 | Target mode |
| GPO1_CFG0 | 0x0C | 0x41 | PDMCLK output |
| GPI_CFG | 0x0D | 0x02 | GPI1 enabled |
| CH_EN | 0x76 | 0xCC | IN_CH1, IN_CH2, OUT_CH1, OUT_CH2 |
| PWR_CFG | 0x78 | 0xC0 | ADC_PDZ=1, DAC_PDZ=1 |

Slots par TAC : TAC0=0,1 / TAC1=2,3 / TAC2=4,5 / TAC3=6,7
Calcul : base_slot = (i2c_addr - 0x50) * 2

## Registres SAI7 cles

| Registre | Addr+8 | Valeur | Description |
|----------|--------|--------|-------------|
| TCSR | 0x08 | 0x90100C01 | TERE=1, FRDE=1 |
| TCR2 | 0x10 | 0x07800000 | MSEL=1, BYP=1, BCI=1, BCD=1 |
| TCR3 | 0x14 | 0x00010000 | TRCE=1 (channel 0) |
| TCR4 | 0x18 | 0x10070039 | FSD=1, FSE=1(dsp_a)/0(dsp_b), MF=1, FRSZ=7, FCONT=1 |
| TCR5 | 0x1C | 0x1F1F1F00 | FBT=31, W0W=31, WNW=31 |
| TMR | 0x60 | 0xFFFFFF00 | Mask slots (channel 0 only active for TX) |
| RCR1 | 0x8C | 0x00000005 | RX watermark = 5 |
| RCR2 | 0x90 | 0x06000000 | BCI=1, BCP=1, BCD=0 (consumer) |
| RCR4 | 0x98 | 0x10070018 | FSD=0, FSE=1, MF=1, FRSZ=7, FCONT=1 |
| RCR5 | 0x9C | 0x1F1F1F00 | FBT=31, W0W=31, WNW=31 |

## PROBLEME PRINCIPAL : CH1 (slot 0) et decalage de bits

### Le probleme du slot 0

Le datasheet TAC5212 (page 32, section 7.3.1.2.1) dit :

> "In TDM mode, the rising edge of FSYNC starts the data transfer with the slot 0 data first.
> FSYNC and each data bit **(except the MSB of slot 0 when TX_OFFSET equals 0)**
> is transmitted on the rising edge of BCLK."

Le MSB du slot 0 avec TX_OFFSET=0 est special : il coincide avec le front montant FSYNC
sur le meme BCLK. Le SAI rate ce premier bit car il utilise ce front pour detecter le FSYNC.

### Tableau des configurations testees

| Format SAI | TX_OFFSET | Resultat Ch1 | Resultat Ch2-Ch8 | Bits alignes? |
|------------|-----------|-------------|-------------------|---------------|
| dsp_b | 0 | MORT (0xFFFFFFFF) | OK (-67 a -109 dBFS) | OUI |
| dsp_b | 1 | OK (signal) | OK (signal) | NON (-3 dBFS, decalage 1 bit) |
| dsp_a | 0 | MORT (0xFFFFFFFF) | OK (-67 a -93 dBFS) | OUI |
| dsp_a | 1 | Tout zero | Tout zero | - |

### Analyse du decalage avec TX_OFFSET=1 + dsp_b

Avec TX_OFFSET=1 en dsp_b, les donnees sont decalees de 1 bit :
- Un echantillon ADC negatif proche de zero (ex: 0xFFFBDF00) est lu comme 0x7FFBDFD9
- Un 0 est insere au MSB (bit 31) car le SAI lit le bus 1 BCLK avant que le TAC commence
- Les 8 canaux ont du signal mais le bruit analogique est a -3 dBFS au lieu de -100 dBFS

### Analyse avec TX_OFFSET=0 + dsp_b ou dsp_a

- Ch2-Ch8 : bits correctement alignes, bruit a -67 a -111 dBFS
- Ch1 (slot 0) : bloque a 0xFFFFFFFF (ou 0x0000FFFE selon RX_OFFSET)
- Le MSB du slot 0 est perdu a cause du timing FSYNC/data simultane

### Pistes non resolues

1. **BCI (Bit Clock Invert)** : BCI=1 est necessaire pour que les donnees soient correctes.
   BCI=0 donne des donnees figees.

2. **BCLK_POL** : PASI_BCLK_POL=1 sur le TAC (reg 0x1A bit 2) fige les donnees.
   Le datasheet Figure 7-5 montre cette option mais ne resout pas le slot 0.

3. **RX_OFFSET** : PASI_RX_OFFSET (reg 0x26) controle le DIN du TAC (playback).
   Avec RX_OFFSET=1, Ch1 passe de 0xFFFFFFFF a 0x0000FFFE.

4. **96 kHz** : BCLK = 24.576 MHz (limite TAC5212). CLK_ERR_STS1 = 0x90
   (BCLK_FS_RATIO_ERR). Fonctionnait en PDM mais bruit eleve en analogique.

## Fichiers du driver

- `meta-local/recipes-kernel/linux/files/tac5212.c` : driver ASoC codec
- `meta-local/recipes-kernel/linux/files/tac5212.h` : defines registres
- `meta-local/recipes-kernel/linux/files/apply-tac5212-dt.py` : modifications DT
- `meta-local/recipes-kernel/linux/linux-imx_%.bbappend` : integration Yocto + patches fsl_sai
- `meta-local/recipes-kernel/linux/files/tac5212.cfg` : CONFIG_SND_SOC_TAC5212=m

## Patches fsl_sai.c

### 1. Async RX consumer fix
Force RX en consumer (BCD=0, FSD=0) en mode async pour que le RX recoit les clocks
depuis les pins physiques au lieu de les driver.

### 2. Async capture TX start
Quand un capture demarre en mode async, active aussi le TX (TERE + TRCE) pour
generer les clocks BCLK/FSYNC.

## Commits git

```
5795a2d8 meta-local: revert to dsp_b, remove 192kHz, finalize TAC5212
4dc42f65 meta-local: fix TDM slot alignment and finalize TAC5212 config
77cdb084 meta-local: integrate TAC5212 TDM fixes for full-duplex capture
307a72d8 meta-local: add TAC5212 TDM codec driver and SAI7 configuration
```

## TODO

- Resoudre le probleme du Ch1 (slot 0) sans decaler les bits
- Valider le 96 kHz (limite BCLK du TAC5212)
- Integrer SION correctement dans le DT (actuellement valeurs brutes qui posent probleme)
- Tester le playback DAC (sortie audio)
- Build full image finale et tests Ardour full-duplex
