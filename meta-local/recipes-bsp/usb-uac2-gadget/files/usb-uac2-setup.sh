#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# V7.0-E6.a — Setup USB UAC2 8×8 gadget via configfs.
#
# Expose le board comme carte son USB 8 ch in + 8 ch out vers le PC hôte.
# Stream à 48 kHz, S32_LE (4 octets/sample). UDC = 38100000.usb (1er DWC3).
#
# Idempotent : si g1 existe déjà (script déjà appliqué), no-op.
# Si l'UDC n'est pas présent (kernel sans driver USB device), exit 0 propre.

set -eu

GADGET=/sys/kernel/config/usb_gadget/g1
UDC_NAME=38100000.usb
UDC_PATH=/sys/class/udc/$UDC_NAME

# 1) Skip si déjà configuré (post-reboot, ou re-run du service)
if [ -d "$GADGET" ]; then
    echo "usb-uac2-setup: $GADGET already exists, no-op"
    exit 0
fi

# 2) Skip si l'UDC n'est pas présent (hardware/kernel non-supporté)
if [ ! -e "$UDC_PATH" ]; then
    echo "usb-uac2-setup: UDC $UDC_NAME absent, skipping (USB hardware not available)"
    exit 0
fi

# 3) configfs déjà monté (oneshot Linux : sys-kernel-config.mount)
if ! mount | grep -q "configfs.*sys/kernel/config"; then
    echo "usb-uac2-setup: ERROR - configfs not mounted" >&2
    exit 1
fi

modprobe libcomposite 2>/dev/null || true

# 4) Création du gadget root
mkdir -p "$GADGET"
echo 0x1d6b > "$GADGET/idVendor"   # Linux Foundation
echo 0x0104 > "$GADGET/idProduct"  # Multifunction Composite Gadget
echo 0x0100 > "$GADGET/bcdDevice"
echo 0x0200 > "$GADGET/bcdUSB"     # USB 2.0

# 5) Strings descriptors
mkdir -p "$GADGET/strings/0x409"
echo "Electrosens"                  > "$GADGET/strings/0x409/manufacturer"
echo "Debix UAC2 8x8"               > "$GADGET/strings/0x409/product"
echo "v7.0-e6a"                     > "$GADGET/strings/0x409/serialnumber"

# 6) Configuration container
mkdir -p "$GADGET/configs/c.1/strings/0x409"
echo "UAC2 8x8 S32_LE 48kHz"        > "$GADGET/configs/c.1/strings/0x409/configuration"
echo 250                            > "$GADGET/configs/c.1/MaxPower"  # mA

# 7) UAC2 function (8 ch in + 8 ch out)
mkdir -p "$GADGET/functions/uac2.0"
echo 0xff   > "$GADGET/functions/uac2.0/p_chmask"  # 8 ch playback (host → board)
echo 48000  > "$GADGET/functions/uac2.0/p_srate"
echo 4      > "$GADGET/functions/uac2.0/p_ssize"   # S32_LE = 4 bytes
echo 0xff   > "$GADGET/functions/uac2.0/c_chmask"  # 8 ch capture (board → host)
echo 48000  > "$GADGET/functions/uac2.0/c_srate"
echo 4      > "$GADGET/functions/uac2.0/c_ssize"

# 8) V12-MIDIX — MIDI function (expandeur : le PC voit un port MIDI in/out
#    sur le même câble). CONFIG_USB_F_MIDI=y (built-in). 1 in + 1 out.
mkdir -p "$GADGET/functions/midi.0"
echo "A.L.A. MIDI" > "$GADGET/functions/midi.0/id"
echo 1             > "$GADGET/functions/midi.0/in_ports"
echo 1             > "$GADGET/functions/midi.0/out_ports"

# 9) Link functions → config
ln -sf "$GADGET/functions/uac2.0" "$GADGET/configs/c.1/"
ln -sf "$GADGET/functions/midi.0" "$GADGET/configs/c.1/"

# 10) Bind to UDC (active le gadget côté USB host)
echo "$UDC_NAME" > "$GADGET/UDC"

echo "usb-uac2-setup: UAC2 8x8 + MIDI gadget bound to $UDC_NAME"
exit 0
