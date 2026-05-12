#!/usr/bin/env python3
# V7.0-E7.4.b — patch sound/soc/fsl/imx-card.c : drop link->id override
#
# imx-card.c sets `link->id = args.args[0]` where `args.args[0]` is the DSP
# DAI index from the DT (e.g. <&dsp 1> → 1). But SOF topology files use
# `link_id` as a sequential ASoC link identifier independent of the DSP
# DAI index (sof-imx8mp-tac5212.tplg has `DAI_CONFIG(SAI, 7, 0, ...)` →
# expects link_id=0). The result is `snd_soc_find_dai_link()` failing to
# match the topology link, with the error :
#   "ASoC: physical link tac5212-hifi (id 0) not exist"
#   "ASoC: topology: could not load header: -22"
#   "imx-card: probe of sof-sound-tac5212 failed with error -22"
#
# simple-card.c does NOT touch link->id (left at the devm_kzalloc-cleared
# default of 0), which is the convention SOF topology files rely on.
#
# This patch comments out the override so link->id stays sequential per
# dai-link (0,1,2... as simple-card does). The DSP DAI is still selected
# by the cpu sound-dai phandle args.args[0] stored elsewhere in
# link->cpus[0].of_node — `link->id` is only used for ASoC link matching.

import sys
import os

if len(sys.argv) != 2:
    sys.stderr.write("usage: apply-imx-card-linkid.py <kernel-source-root>\n")
    sys.exit(1)

src = os.path.join(sys.argv[1], "sound/soc/fsl/imx-card.c")
if not os.path.exists(src):
    sys.stderr.write(f"missing : {src}\n")
    sys.exit(1)

with open(src, "r") as f:
    text = f.read()

# Sentinel : already patched
if "V7.0-E7.4.b imx-card link_id" in text:
    print("imx-card.c already patched, skipping")
    sys.exit(0)

# The exact line we want to neutralise. Context-anchored to avoid touching
# anything else that legitimately sets link->id.
old = """\t\tlink->platforms->of_node = link->cpus->of_node;
\t\tlink->id = args.args[0];

\t\tcodec = of_get_child_by_name(np, "codec");"""

new = """\t\tlink->platforms->of_node = link->cpus->of_node;
\t\t/* V7.0-E7.4.b imx-card link_id : do NOT use args.args[0] as link->id.
\t\t * SOF topology link_id is a sequential ASoC index (0,1,2...),
\t\t * independent of the DSP DAI index from cpu sound-dai. Leaving
\t\t * link->id at its default 0/sequential allocation matches the
\t\t * convention used by simple-card and by SOF tplg files like
\t\t * sof-imx8mp-tac5212.tplg (DAI_CONFIG(SAI, 7, 0, tac5212-hifi)).
\t\t */
\t\t/* link->id = args.args[0]; */

\t\tcodec = of_get_child_by_name(np, "codec");"""

if old not in text:
    sys.stderr.write("expected `link->id = args.args[0]` block not found in "
                     "imx-card.c — kernel source layout may have changed\n")
    sys.exit(1)

text = text.replace(old, new)

with open(src, "w") as f:
    f.write(text)

print("imx-card.c : link_id override neutralised (V7.0-E7.4.b)")
