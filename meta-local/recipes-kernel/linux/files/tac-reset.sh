#!/bin/sh
# TAC5212 SW reset script - run WHILE audio stream is active (BCLK present)
# Usage: tac-reset.sh [pdm|analog] [tac_addr]
#   pdm    - reset for PDM microphone input on CH1+CH2
#   analog - reset for analog ADC input (default)
#   tac_addr - I2C address (default: 0x50 = TAC0)
#
# Example:
#   tac-reset.sh pdm          # Reset TAC0 for PDM
#   tac-reset.sh analog 0x51  # Reset TAC1 for analog
#   tac-reset.sh all          # Reset all TACs for analog

MODE=${1:-analog}
BUS=3

reset_tac() {
    local ADDR=$1
    local CFG4=$2

    echo "Resetting TAC @ $ADDR (INTF_CFG4=$CFG4)..."
    i2cset -f -y $BUS $ADDR 0x01 0x01  # SW_RESET
    usleep 100000
    i2cset -f -y $BUS $ADDR 0x02 0x09  # Exit sleep
    usleep 100000
    i2cset -f -y $BUS $ADDR 0x10 0x53  # INTF_CFG1
    i2cset -f -y $BUS $ADDR 0x11 0x80  # INTF_CFG2
    i2cset -f -y $BUS $ADDR 0x18 0x40  # ASI_CFG0
    i2cset -f -y $BUS $ADDR 0x04 0x40  # MISC_CFG
    i2cset -f -y $BUS $ADDR 0x1b 0x68  # PASI_TX_CFG0
    i2cset -f -y $BUS $ADDR 0x1c 0x01  # TX_OFFSET=1
    i2cset -f -y $BUS $ADDR 0x26 0x01  # RX_OFFSET=1
    i2cset -f -y $BUS $ADDR 0x0c 0x41  # GPO1=PDMCLK
    i2cset -f -y $BUS $ADDR 0x0d 0x02  # GPI1 enable
    i2cset -f -y $BUS $ADDR 0x13 $CFG4 # INTF_CFG4
    i2cset -f -y $BUS $ADDR 0x1a 0x30  # PASI_CFG0: TDM 32bit

    # Slot assignments based on I2C address
    local BASE=$(( ($ADDR - 0x50) * 2 ))
    local SLOT1=$(( 0x20 | $BASE ))
    local SLOT2=$(( 0x20 | $BASE + 1 ))
    i2cset -f -y $BUS $ADDR 0x1e $SLOT1  # TX_CH1
    i2cset -f -y $BUS $ADDR 0x1f $SLOT2  # TX_CH2
    i2cset -f -y $BUS $ADDR 0x28 $SLOT1  # RX_CH1
    i2cset -f -y $BUS $ADDR 0x29 $SLOT2  # RX_CH2

    i2cset -f -y $BUS $ADDR 0x34 0x40  # CLK_CFG2
    i2cset -f -y $BUS $ADDR 0x76 0xCC  # CH_EN
    i2cset -f -y $BUS $ADDR 0x78 0xE0  # PWR_CFG (ADC+DAC+MICBIAS)
    echo "TAC @ $ADDR reset done."
}

# Fix SAI7 RX direction: force consumer mode (BCD=0, FSD=0)
# SOF firmware incorrectly sets RX as master, conflicting with TX on shared pins
fix_sai7_rx() {
    python3 -c "
import mmap, struct, os
fd = os.open('/dev/mem', os.O_RDWR | os.O_SYNC)
m = mmap.mmap(fd, 0x100, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE, offset=0x30c80000)
rcr2 = struct.unpack('<I', m[0x90:0x94])[0]
rcr2 &= ~(1 << 24)  # BCD=0 (consumer)
m[0x90:0x94] = struct.pack('<I', rcr2)
rcr4 = struct.unpack('<I', m[0x98:0x9C])[0]
rcr4 &= ~1  # FSD=0 (consumer)
m[0x98:0x9C] = struct.pack('<I', rcr4)
m.close(); os.close(fd)
print('SAI7 RX fixed: BCD=0 FSD=0')
" 2>/dev/null
}

case "$MODE" in
    pdm)
        ADDR=${2:-0x50}
        fix_sai7_rx
        reset_tac $ADDR 0x8C
        ;;
    analog)
        ADDR=${2:-0x50}
        fix_sai7_rx
        reset_tac $ADDR 0x0C
        ;;
    all)
        echo "Resetting all TACs for analog..."
        fix_sai7_rx
        for addr in 0x50 0x51 0x52 0x53; do
            reset_tac $addr 0x0C
        done
        ;;
    allpdm)
        echo "Resetting all TACs for PDM..."
        fix_sai7_rx
        for addr in 0x50 0x51 0x52 0x53; do
            reset_tac $addr 0x8C
        done
        ;;
    *)
        echo "Usage: $0 [pdm|analog|all|allpdm] [i2c_addr]"
        exit 1
        ;;
esac
