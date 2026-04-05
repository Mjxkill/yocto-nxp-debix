#!/bin/sh
# TAC5212 reset script — sequential per-device reset for shared DOUT bus
# With continuous BCLK (SOF ASYNC mode), resetting all TACs simultaneously
# causes DOUT bus contention. Reset one TAC at a time instead.
# Usage: tac-reset [pdm|analog]

MODE=${1:-analog}
BUS=3
ADDRS="0x50 0x51 0x52 0x53"

case "$MODE" in
    pdm)    CFG4=0x8C ;;
    analog) CFG4=0x0C ;;
    *)      echo "Usage: $0 [pdm|analog]"; exit 1 ;;
esac

echo "Resetting TACs sequentially ($MODE mode)..."

for addr in $ADDRS; do
    BASE=$(( ($addr - 0x50) * 2 ))
    SLOT1=$(( 0x20 | $BASE ))
    SLOT2=$(( 0x20 | $BASE + 1 ))
    KEEPER=0x40
    [ "$addr" = "0x50" ] && KEEPER=0x48

    # Reset THIS TAC only
    i2cset -f -y $BUS $addr 0x01 0x01
    usleep 50000

    # Exit sleep
    i2cset -f -y $BUS $addr 0x02 0x09
    usleep 50000

    # Interface config
    i2cset -f -y $BUS $addr 0x10 0x51
    i2cset -f -y $BUS $addr 0x11 0x80
    i2cset -f -y $BUS $addr 0x18 0x40
    i2cset -f -y $BUS $addr 0x04 0x40

    # TX config: TX_FILL=1 BEFORE setting slots
    i2cset -f -y $BUS $addr 0x1b $KEEPER
    i2cset -f -y $BUS $addr 0x1c 0x01

    # RX config
    i2cset -f -y $BUS $addr 0x26 0x01

    # GPIO/PDM
    i2cset -f -y $BUS $addr 0x0c 0x41
    i2cset -f -y $BUS $addr 0x0d 0x02
    i2cset -f -y $BUS $addr 0x13 $CFG4

    # TDM format
    i2cset -f -y $BUS $addr 0x1a 0x30

    # Slot assignments
    i2cset -f -y $BUS $addr 0x1e $SLOT1
    i2cset -f -y $BUS $addr 0x1f $SLOT2
    i2cset -f -y $BUS $addr 0x28 $SLOT1
    i2cset -f -y $BUS $addr 0x29 $SLOT2

    # Clock config
    i2cset -f -y $BUS $addr 0x34 0x40

    # Channel enable + power up
    i2cset -f -y $BUS $addr 0x76 0xCC
    i2cset -f -y $BUS $addr 0x78 0xE0

    # Wait PLL lock for THIS TAC
    usleep 500000
    i2cget -f -y $BUS $addr 0x3c > /dev/null 2>&1
    i2cget -f -y $BUS $addr 0x3d > /dev/null 2>&1
done

echo "All TACs reset done ($MODE)."
