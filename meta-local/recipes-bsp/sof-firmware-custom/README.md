# sof-firmware-custom — V7.0 SOF artifacts

This recipe vendors the SOF artifacts produced from the project's local
`sof/` source tree (branch `feature/v7.0-multiband-drc-tap`, github
`github.com/Mjxkill/sof`). It exists because the upstream `meta-imx`
recipe `sof-zephyr_2.10.0.bb` ships a vendor binary that does NOT
include :

- the TAC5212 codec driver topology component
- the V7.0 `multiband_drc` multi-config blob detection patch
- the V5.4.1 D3 DRC per-channel patch
- the V4 NPU dual-tap firmware hook
- the V7.0-E7.2 SAI TX FIFO alignment fix

Without this recipe a fresh `bitbake imx-image-full` would install the
stock vendor firmware and the kernel would not register the
`softac5212tdm` ALSA card, blocking mixer-pro startup.

## Layout

| File | md5 (current) | Built from |
|---|---|---|
| `files/sof-imx8m.ri` | `743e2e249cc3ea6a291b975a614514f0` | sof commit `4bea8e59d` (V7.0-E7.2 TX FIFO align) |
| `files/sof-imx8mp-tac5212.tplg` | `0363fd2806b9d2adfbd4f4cf994dc6e9` | same |

## Build procedure (refreshing the artifacts)

When the local `sof/` source tree changes (audio components, topology
M4, IPC patches, etc.), the vendored binaries need to be regenerated.

```bash
# 1. Build the firmware
cd /home/michael/yocto-nxp-debix/sof
./scripts/xtensa-build-zephyr.py imx8mp
# → sof/build-imx8mp-xcc/sof.ri (or similar)

# IMPORTANT (per memory sof_firmware_build.md) : the .ri loaded by the
# kernel must be the CONCATENATION of .ri.xman + .ri (xman header +
# payload). If you skip this you get IPC 108/20 errors at boot.
cat build-imx8mp-xcc/sof.ri.xman build-imx8mp-xcc/sof.ri \
    > /tmp/sof-imx8m.ri.full

# 2. Build the topology
cd sof/tools/topology/topology1
m4 sof-imx8mp-tac5212.m4 | alsatplg -c /dev/stdin -o /tmp/sof-imx8mp-tac5212.tplg

# 3. Drop them in this recipe and bump the md5 + commit
cp /tmp/sof-imx8m.ri.full \
   meta-local/recipes-bsp/sof-firmware-custom/files/sof-imx8m.ri
cp /tmp/sof-imx8mp-tac5212.tplg \
   meta-local/recipes-bsp/sof-firmware-custom/files/sof-imx8mp-tac5212.tplg

# 4. Bitbake + flash
bitbake sof-firmware-custom -c cleansstate
bitbake imx-image-full
```

## Install paths

The kernel SOF loader resolves `/lib/firmware/imx/sof/sof-imx8m.ri`
through the symlink `/lib/firmware/imx/sof -> sof-zephyr-xcc/` set up
by the vendor `sof-zephyr` recipe. To guarantee the kernel picks our
file regardless of variant active, the recipe installs the same `.ri`
into both `sof-zephyr-xcc/` and `sof-zephyr-gcc/`.

The vendor recipe's same-named `.ri` files are evicted at install time
by the companion bbappend at
`meta-local/recipes-kernel/linux-firmware/sof-zephyr_%.bbappend`.

The topology lives in the shared `/lib/firmware/imx/sof-tplg/` which
is unaffected by the variant symlink.
