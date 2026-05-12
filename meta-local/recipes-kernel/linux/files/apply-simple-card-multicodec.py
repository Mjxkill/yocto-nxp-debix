#!/usr/bin/env python3
# V7.0-E7.4.b — patch sound/soc/generic/simple-card.c : multi-codec support
#
# Upstream `simple_count_noml()` hardcodes `li->num[li->link].codecs = 1`
# which prevents simple-audio-card DT links from binding more than one
# codec, even when the DT declares `sound-dai = <&a>, <&b>, <&c>, ...`.
#
# This patch replaces that single line with a phandle count, so a DAI
# link with N codec phandles binds all N. Existing single-codec DTs are
# unaffected (count returns 1).
#
# Reference : datasheet of `sound-dai` phandle, kernel API
# of_count_phandle_with_args(codec_node, "sound-dai", "#sound-dai-cells").

import sys
import os

if len(sys.argv) != 2:
    sys.stderr.write("usage: apply-simple-card-multicodec.py <kernel-source-root>\n")
    sys.exit(1)

src = os.path.join(sys.argv[1], "sound/soc/generic/simple-card.c")
if not os.path.exists(src):
    sys.stderr.write(f"missing : {src}\n")
    sys.exit(1)

with open(src, "r") as f:
    text = f.read()

# Sentinel : already patched
if "V7.0-E7.4.b multi-codec" in text:
    sys.stderr.write("simple-card.c already patched for multi-codec, skipping\n")
    sys.exit(0)

# Target block inside simple_count_noml() : keep CPU + platforms = 1,
# but make codecs count match the actual phandle count in DT.
old = """\tli->num[li->link].cpus\t\t= 1;
\tli->num[li->link].platforms\t= 1;

\tli->num[li->link].codecs\t= 1;

\tli->link += 1;

\treturn 0;
}

static int simple_count_dpcm("""

new = """\tli->num[li->link].cpus\t\t= 1;
\tli->num[li->link].platforms\t= 1;

\t/* V7.0-E7.4.b multi-codec : honour every codec phandle in DT */
\tli->num[li->link].codecs\t= 1;
\tif (codec) {
\t\tint n = of_count_phandle_with_args(codec, \"sound-dai\",
\t\t\t\t\t\t   \"#sound-dai-cells\");
\t\tif (n > 1)
\t\t\tli->num[li->link].codecs = n;
\t}

\tli->link += 1;

\treturn 0;
}

static int simple_count_dpcm("""

if old not in text:
    sys.stderr.write("expected simple_count_noml block not found in simple-card.c — "
                     "kernel source layout may have changed\n")
    sys.exit(1)

text = text.replace(old, new)

with open(src, "w") as f:
    f.write(text)

print("simple-card.c : multi-codec support installed (V7.0-E7.4.b)")
