#!/bin/sh
# TAC5212 daisy-chain configuration script
# Topology: TAC0(DOUT)->GPIO2 TAC1(DOUT)->GPIO2 TAC2(DOUT)->GPIO2 TAC3(DOUT)->DIN SAI7
# 8 channels TDM 32-bit, 48kHz, BCLK=12.288MHz
# SAI7 provides BCLK+FSYNC (all TACs in target mode)
#
# Usage: tac-daisy.sh [analog|pdm]

MODE=${1:-analog}
BUS=3

TAC0=0x50
TAC1=0x51
TAC2=0x52
TAC3=0x53
ADDRS="$TAC0 $TAC1 $TAC2 $TAC3"

case "$MODE" in
    pdm)    CFG4=0x8C ;;  # PDM_CH1_SEL + PDM_DIN1_SEL=GPI1
    analog) CFG4=0x0C ;;  # analog mode, PDM_DIN1_SEL=GPI1
    *)      echo "Usage: $0 [analog|pdm]"; exit 1 ;;
esac

echo "Configuring 4x TAC5212 daisy-chain ($MODE mode)..."

# -----------------------------------------------
# 1. SW_RESET all TACs
# -----------------------------------------------
for addr in $ADDRS; do
    i2cset -f -y $BUS $addr 0x01 0x01
done
usleep 100000

# -----------------------------------------------
# 2. Exit sleep mode (DREG + VREF enabled)
#    DEV_MISC_CFG (0x02) = 0x09
#    SLEEP_ENZ=1, SLEEP_EXIT_VREF_EN=1
# -----------------------------------------------
for addr in $ADDRS; do
    i2cset -f -y $BUS $addr 0x02 0x09
done
usleep 100000

# -----------------------------------------------
# 3. DOUT drive: full strength (active low + active high)
#    INTF_CFG1 (0x10) = 0x51
#    DOUT_SEL=0101 (primary ASI DOUT), DOUT_DRV=001
# -----------------------------------------------
for addr in $ADDRS; do
    i2cset -f -y $BUS $addr 0x10 0x51
done

# -----------------------------------------------
# 4. INTF_CFG2 (0x11) = 0x80 — PASI DIN enabled
# -----------------------------------------------
for addr in $ADDRS; do
    i2cset -f -y $BUS $addr 0x11 0x80
done

# -----------------------------------------------
# 5. ASI_CFG0 (0x18) — Daisy chain config
#    TAC0: no daisy input (first in chain)
#      0x40 = PASI enabled, SASI disabled, no daisy
#    TAC1/2/3: daisy input from GPIO2
#      0x4A = PASI enabled, SASI disabled,
#             DAISY_EN=01 (PASI daisy), DAISY_IN_SEL=010 (GPIO2)
# -----------------------------------------------
i2cset -f -y $BUS $TAC0 0x18 0x40
i2cset -f -y $BUS $TAC1 0x18 0x4A
i2cset -f -y $BUS $TAC2 0x18 0x4A
i2cset -f -y $BUS $TAC3 0x18 0x4A

# -----------------------------------------------
# 6. GPIO2 as GPI on TAC1/2/3 (daisy input pin)
#    GPIO2_CFG0 (0x0B) = 0x10
#    GPIO2_CFG[7:4]=0001 (GPI), GPIO2_DRV=000
# -----------------------------------------------
i2cset -f -y $BUS $TAC1 0x0B 0x10
i2cset -f -y $BUS $TAC2 0x0B 0x10
i2cset -f -y $BUS $TAC3 0x0B 0x10

# -----------------------------------------------
# 7. MISC_CFG (0x04) = 0x40 — IGNORE_CLK_ERR
# -----------------------------------------------
for addr in $ADDRS; do
    i2cset -f -y $BUS $addr 0x04 0x40
done

# -----------------------------------------------
# 8. TDM format: TDM mode, 32-bit word length
#    PASI_CFG0 (0x1A) = 0x30
#    PASI_FORMAT=00 (TDM), PASI_WLEN=11 (32-bit)
# -----------------------------------------------
for addr in $ADDRS; do
    i2cset -f -y $BUS $addr 0x1A 0x30
done

# -----------------------------------------------
# 9. TX config — drive 0 on unused slots (not Hi-Z, point-to-point)
#    PASI_TX_CFG0 (0x1B) = 0x00
#    TX_EDGE=0, TX_FILL=0, TX_LSB=0, KEEPER=00
# -----------------------------------------------
for addr in $ADDRS; do
    i2cset -f -y $BUS $addr 0x1B 0x00
done

# -----------------------------------------------
# 10. TX_OFFSET = 1 (recommended for TDM)
#     PASI_TX_CFG1 (0x1C) = 0x01
# -----------------------------------------------
for addr in $ADDRS; do
    i2cset -f -y $BUS $addr 0x1C 0x01
done

# -----------------------------------------------
# 11. RX config
#     PASI_RX_CFG0 (0x26) = 0x01 (RX_OFFSET=1)
# -----------------------------------------------
for addr in $ADDRS; do
    i2cset -f -y $BUS $addr 0x26 0x01
done

# -----------------------------------------------
# 12. TX slot assignments — 2 channels per TAC
#     PASI_TX_CH1_CFG (0x1E): bit5=enable, bits[4:0]=slot
#     PASI_TX_CH2_CFG (0x1F): bit5=enable, bits[4:0]=slot
#
#     TAC0: ch1=slot0, ch2=slot1
#     TAC1: ch1=slot2, ch2=slot3
#     TAC2: ch1=slot4, ch2=slot5
#     TAC3: ch1=slot6, ch2=slot7
# -----------------------------------------------
i2cset -f -y $BUS $TAC0 0x1E 0x20  # ch1 -> slot 0
i2cset -f -y $BUS $TAC0 0x1F 0x21  # ch2 -> slot 1

i2cset -f -y $BUS $TAC1 0x1E 0x22  # ch1 -> slot 2
i2cset -f -y $BUS $TAC1 0x1F 0x23  # ch2 -> slot 3

i2cset -f -y $BUS $TAC2 0x1E 0x24  # ch1 -> slot 4
i2cset -f -y $BUS $TAC2 0x1F 0x25  # ch2 -> slot 5

i2cset -f -y $BUS $TAC3 0x1E 0x26  # ch1 -> slot 6
i2cset -f -y $BUS $TAC3 0x1F 0x27  # ch2 -> slot 7

# -----------------------------------------------
# 13. RX slot assignments (for DAC playback)
#     Same slot layout as TX
# -----------------------------------------------
i2cset -f -y $BUS $TAC0 0x28 0x20  # ch1 -> slot 0
i2cset -f -y $BUS $TAC0 0x29 0x21  # ch2 -> slot 1

i2cset -f -y $BUS $TAC1 0x28 0x22
i2cset -f -y $BUS $TAC1 0x29 0x23

i2cset -f -y $BUS $TAC2 0x28 0x24
i2cset -f -y $BUS $TAC2 0x29 0x25

i2cset -f -y $BUS $TAC3 0x28 0x26
i2cset -f -y $BUS $TAC3 0x29 0x27

# -----------------------------------------------
# 14. GPIO/PDM config
#     GPO1_CFG0 (0x0C) = 0x41 — PDMCLK on GPO1
#     GPI_CFG   (0x0D) = 0x02 — GPI1 enabled
#     INTF_CFG4 (0x13) = mode-dependent
# -----------------------------------------------
for addr in $ADDRS; do
    i2cset -f -y $BUS $addr 0x0C 0x41
    i2cset -f -y $BUS $addr 0x0D 0x02
    i2cset -f -y $BUS $addr 0x13 $CFG4
done

# -----------------------------------------------
# 15. Clock config
#     CLK_CFG2 (0x34) = 0x40 — PLL enabled, fractional allowed
# -----------------------------------------------
for addr in $ADDRS; do
    i2cset -f -y $BUS $addr 0x34 0x40
done

# -----------------------------------------------
# 16. ADC input config — differential AC-coupled, 5k impedance
#     ADC_CH1_CFG0 (0x50) = 0x00
#     ADC_CH2_CFG0 (0x55) = 0x00
# -----------------------------------------------
for addr in $ADDRS; do
    i2cset -f -y $BUS $addr 0x50 0x00
    i2cset -f -y $BUS $addr 0x55 0x00
done

# -----------------------------------------------
# 17. Channel enable
#     CH_EN (0x76) = 0xCC — ADC ch1+2, DAC ch1+2
# -----------------------------------------------
for addr in $ADDRS; do
    i2cset -f -y $BUS $addr 0x76 0xCC
done

# -----------------------------------------------
# 18. Power up — ADC + DAC + MICBIAS
#     PWR_CFG (0x78) = 0xE0
# -----------------------------------------------
for addr in $ADDRS; do
    i2cset -f -y $BUS $addr 0x78 0xE0
done

# -----------------------------------------------
# 19. Wait PLL lock, clear error status
# -----------------------------------------------
sleep 1
for addr in $ADDRS; do
    i2cget -f -y $BUS $addr 0x3C > /dev/null 2>&1
    i2cget -f -y $BUS $addr 0x3D > /dev/null 2>&1
done

echo "Daisy-chain config done ($MODE). 8ch TDM 32-bit @ 48kHz"
echo "  TAC0(0x50) slots 0-1 -> TAC1(0x51) slots 2-3 -> TAC2(0x52) slots 4-5 -> TAC3(0x53) slots 6-7 -> SAI7"
