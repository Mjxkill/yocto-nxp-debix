#!/usr/bin/env python3
"""
Patch sound/soc/sof/imx/ to register a SOF probes client for NXP i.MX.

Idempotent: if already applied, exits cleanly.

Modifies:
  - sound/soc/sof/imx/Kconfig   (adds SND_SOC_SOF_IMX_PROBES entry)
  - sound/soc/sof/imx/Makefile  (adds snd-sof-imx-probes module)
  - sound/soc/sof/imx/imx8m.c   (calls imx_probes_register/unregister)

Expects ${WORKDIR}/imx-probes.c to be already copied to
${S}/sound/soc/sof/imx/imx-probes.c by the bbappend.
"""

import sys
import os
import re

KCONFIG_ENTRY = """
config SND_SOC_SOF_IMX_PROBES
	tristate "SOF probes support for NXP i.MX"
	depends on SND_SOC_SOF_IMX_COMMON
	select SND_SOC_SOF_DEBUG_PROBES
	help
	  This adds support for SOF probes (audio buffer extraction) on
	  NXP i.MX platforms. Enables /dev/snd/comprC?D? for tapping
	  audio data from arbitrary pipeline buffers.

"""

MAKEFILE_ADDITIONS = """
snd-sof-imx-probes-objs := imx-probes.o
obj-$(CONFIG_SND_SOC_SOF_IMX_PROBES) += snd-sof-imx-probes.o
"""

IMX8M_FORWARD_DECLS = """
#if IS_ENABLED(CONFIG_SND_SOC_SOF_IMX_PROBES)
int imx_probes_register(struct snd_sof_dev *sdev);
void imx_probes_unregister(struct snd_sof_dev *sdev);
#endif

"""

IMX8M_PROBE_HOOK = """
#if IS_ENABLED(CONFIG_SND_SOC_SOF_IMX_PROBES)
	{
		int pr = imx_probes_register(sdev);
		if (pr)
			dev_warn(sdev->dev,
				 "imx-probes register failed: %d\\n", pr);
	}
#endif
"""

IMX8M_REMOVE_HOOK = """
#if IS_ENABLED(CONFIG_SND_SOC_SOF_IMX_PROBES)
	imx_probes_unregister(sdev);
#endif
"""

MARKER = "SND_SOC_SOF_IMX_PROBES"


def patch_kconfig(path):
    with open(path, "r") as f:
        content = f.read()
    if MARKER in content:
        print(f"[imx-probes] Kconfig already patched: {path}")
        return
    # Insert before the final 'endif ## SND_SOC_SOF_IMX_TOPLEVEL'
    new = re.sub(
        r"(endif ## SND_SOC_SOF_IMX_TOPLEVEL)",
        KCONFIG_ENTRY + r"\1",
        content,
        count=1,
    )
    if new == content:
        sys.exit(f"[imx-probes] ERROR: could not find endif marker in {path}")
    with open(path, "w") as f:
        f.write(new)
    print(f"[imx-probes] Kconfig patched: {path}")


def patch_makefile(path):
    with open(path, "r") as f:
        content = f.read()
    if "snd-sof-imx-probes" in content:
        print(f"[imx-probes] Makefile already patched: {path}")
        return
    with open(path, "a") as f:
        f.write(MAKEFILE_ADDITIONS)
    print(f"[imx-probes] Makefile patched: {path}")


def patch_imx8m(path):
    """Use str.replace (not re.sub) to avoid mangling C backslash escapes
    such as \\n inside the inserted hook string."""
    with open(path, "r") as f:
        content = f.read()
    if MARKER in content:
        print(f"[imx-probes] imx8m.c already patched: {path}")
        return

    # 1. Forward declarations BEFORE imx8m_probe signature.
    anchor1 = "static int imx8m_probe(struct snd_sof_dev *sdev)"
    if anchor1 not in content:
        sys.exit(f"[imx-probes] ERROR: imx8m_probe signature not found in {path}")
    content = content.replace(anchor1, IMX8M_FORWARD_DECLS + anchor1, 1)

    # 2. Probe register hook just BEFORE 'exit_pdev_unregister:' label.
    #    The unique anchor is "\n\nexit_pdev_unregister:".
    anchor2 = "\treturn 0;\n\nexit_pdev_unregister:"
    if anchor2 not in content:
        sys.exit(
            f"[imx-probes] ERROR: could not find exit_pdev_unregister "
            f"anchor in {path}"
        )
    content = content.replace(
        anchor2,
        IMX8M_PROBE_HOOK + "\treturn 0;\n\nexit_pdev_unregister:",
        1,
    )

    # 3. Unregister hook at the start of imx8m_remove body.
    anchor3 = (
        "static int imx8m_remove(struct snd_sof_dev *sdev)\n{\n"
        "\tstruct imx8m_priv *priv = sdev->pdata->hw_pdata;\n"
    )
    if anchor3 not in content:
        sys.exit(
            f"[imx-probes] ERROR: could not find imx8m_remove anchor in {path}"
        )
    content = content.replace(anchor3, anchor3 + IMX8M_REMOVE_HOOK, 1)

    with open(path, "w") as f:
        f.write(content)
    print(f"[imx-probes] imx8m.c patched: {path}")


def patch_sof_client_probes(path):
    """Add 'snd_sof.imx-probes' to the auxiliary_device_id table so the
    upstream sof-client-probes driver binds against our imx-probes
    auxiliary device. Additive: appends one entry, no behaviour change
    for existing entries."""
    with open(path, "r") as f:
        content = f.read()
    if "snd_sof.imx-probes" in content:
        print(f"[imx-probes] sof-client-probes.c already patched: {path}")
        return
    # Anchor on the existing table closing terminator. Replace the
    # acp-probes entry + sentinel with acp-probes + imx-probes + sentinel.
    anchor = (
        '\t{ .name = "snd_sof.acp-probes", },\n'
        '\t{},'
    )
    if anchor not in content:
        sys.exit(
            f"[imx-probes] ERROR: probe id_table anchor not found in {path}"
        )
    new_block = (
        '\t{ .name = "snd_sof.acp-probes", },\n'
        '\t{ .name = "snd_sof.imx-probes", },\n'
        '\t{},'
    )
    content = content.replace(anchor, new_block, 1)
    with open(path, "w") as f:
        f.write(content)
    print(f"[imx-probes] sof-client-probes.c patched: {path}")


def main():
    if len(sys.argv) != 2:
        sys.exit("Usage: apply-imx-probes.py <kernel-source-dir>")
    s = sys.argv[1]
    base = os.path.join(s, "sound", "soc", "sof", "imx")
    if not os.path.isdir(base):
        sys.exit(f"[imx-probes] ERROR: directory not found: {base}")

    patch_kconfig(os.path.join(base, "Kconfig"))
    patch_makefile(os.path.join(base, "Makefile"))
    patch_imx8m(os.path.join(base, "imx8m.c"))
    patch_sof_client_probes(
        os.path.join(s, "sound", "soc", "sof", "sof-client-probes.c")
    )
    print("[imx-probes] all patches applied successfully")


if __name__ == "__main__":
    main()
