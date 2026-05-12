#!/usr/bin/env python3
# V7.0-E7.4.b — DISABLED, switched approach.
#
# Initial attempt patched `simple_count_noml()` to dynamically count codec
# phandles. The count succeeded but `simple_parse_node()` then only parsed
# codec[0], leaving codec[1..N-1] with empty of_node/dai_name. The result
# was a fatal probe error : `Neither Component name/of_node are set for
# sai3-tac5212-hifi`, killing the sound card entirely (TAC0 included).
#
# Switched to `compatible = "fsl,imx-audio-card"` (NXP imx-card.c machine
# driver) which uses `snd_soc_of_get_dai_link_codecs()` — the standard
# ASoC helper that properly fills every codec entry. See apply-tac5212-dt.py
# for the DT compatible change.
#
# This script is kept as no-op so existing `linux-imx_%.bbappend` references
# remain valid; remove the SRC_URI + do_patch:append line in a future cleanup.

import sys

if len(sys.argv) != 2:
    sys.stderr.write("usage: apply-simple-card-multicodec.py <kernel-source-root>\n")
    sys.exit(1)

print("simple-card multi-codec patch : DISABLED (see fsl,imx-audio-card)")
