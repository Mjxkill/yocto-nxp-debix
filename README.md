# New features
A summary of the main new features is as follows.
New features added for all supported boards:
- Upgraded the kernel to 6.12.3 with consolidated Linux Factory Kernel.
- Upgraded the U-Boot to v2024.04 with consolidated Linux Factory U-Boot.
- Updated EULA to v58 November 2024.
- Upgraded the Yocto Project to version 5.1 Styhead.
- Supports the GCC 14.2 toolchain.
- Supports Glitch Detection (GDET) on i.MX 93.
- Added Flutter and Dart SDK support via meta-flutter layer.
- Cortex-M33 update for 8ULP and i.MX 93, Cortex-M7 updates for i.MX 8M Nano, i.MX 8M Plus, and i.MX 95,
and Cortex-M4 update for i.MX 7ULP, i.MX 8M Mini, and i.MX 8M Quad.
- Security
  1. OP-TEE upgraded to 4.4.0.
- Multimedia updates:
  1. Enabled the "V4L2VD" video decoder in Chromium 129.0.6668.100.
  2. Camera preview: Supports community capture driver for i.MX 8M Nano, i.MX 8M Mini, i.MX 8M Quad, i.MX8ULP, i.MX 8QuadXPlus, i.MX 8QuadMax, and i.MX 93. This feature is not working for i.MX 8DualX, support for it will be added in the next release.
- Graphics updates:
  1. i.MX 9 GPU driver upgraded to r53p0.
  2. i.MX 6/7/8 GPU driver upgraded to 6.4.11.p3.0 with Vulkan enablement, bug fixes, and performance optimizations.
  3. GPU SDK upgraded to 6.2.4.
  4. Chromium upgraded to 129.0.6668.100.
> **Note:** Chromium is not supported on i.MX 6 and i.MX 7 any longer due to the DRM/KMS display driver limitation, but is still supported on i.MX 8 and i.MX 9.
- Machine Learning updates:
  1. OpenCV upgraded to 4.10.0.
  2. TensorFlow Lite upgraded to 2.16.2 with GPU acceleration.
  3. i.MX 93 Vela upgraded to 3.12.
- i.MX 8M Plus
  1. Updates for ISP 4.2.2.25.1.
- i.MX 91
  1. Introduction for i.MX 91 11x11 as GA quality.
  2. Introduction for i.MX 91 9x9 as GA quality.
- Arm SystemReady-IR (SR-IR) certification
  1. i.MX 8M Mini EVK board has passed the Arm SR-IR certification.
  2. i.MX 8M Plus EVK board has passed the Arm SR-IR certification.
  3. i.MX 8M Quad EVK board has passed the Arm SR-IR certification.
  4. i.MX 8M Nano EVK board has passed the Arm SR-IR certification.
- Userspace Ethernet DPDK Driver
  1. Supports TSN-QBV on i.MX 95.
  2. Supports OpenSSL-based applications on i.MX 95.
- The following boards are not supported in this release:
  1. i.MX 8QuadXPlus B0 MEK
  2. i.MX 8DXL A1 DDR3L EVK
  3. i.MX 6QuadPlus SABRE-AI
  4. i.MX 6Quad/Dual SABRE-AI
  5. i.MX 6DualLite SABRE-AI


# Fork-specific changes
This fork extends the NXP i.MX BSP with additional features and fixes:

- Integrated the `meta-flutter` layer to provide Dart and Flutter SDK support.
- Added custom kernel configuration and a USB Audio Class 2 patch for improved MIDI and audio functionality on Debix boards.
- Disabled the CAAM cryptographic accelerator to avoid initialization issues during boot.
- Extended the default image with audio and AI tools such as Jack, Ardour, TensorFlow Lite and ONNX Runtime.
- Patched hostapd to apply a fresh defconfig directly from the source tree.
- Updated the voice UI player demo to use a valid model revision.
- Improved ONNX Runtime packaging and allowed building with GCC 13 by ignoring interference-size warnings.
- Included Debix-specific rootfs scripts and services (ADB enablement, mass storage) along with Broadcom Wi-Fi firmware.
- Limited build parallelism to eight threads and made the gstreamer ugly plugin optional.
- Configured NTP to use `ntp.aliyun.com` as the default server.

## Branch and Patch Review

### Branches
- `L6.12.3-debix_model_ab`: baseline of the NXP BSP for Debix boards.
- `work`: current integration branch that aggregates all customizations listed above.

### Patch status
**Working patches**
- `meta-flutter` layer with Dart and Flutter SDK recipes.
- Custom kernel configuration and USB Audio Class 2 fixes for better MIDI/audio support.
- U-Boot SPL legacy image support for i.MX8MP (legacy boot enabled).
- CAAM driver disabled to avoid hardware initialization errors.
- Ardour recipe and packaging cleanup for ONNX Runtime, hostapd and build thread limitation.

**Removed or non-working patches**
- `gstreamer1.0-plugins-ugly` made optional because the package is not always buildable.
- `lv2`, `ladspa-sdk` and `jack-utils` dropped from images due to missing dependencies.

### Future work
- Reintroduce multimedia packages when upstream becomes stable.
- Add camera preview support for i.MX 8DualX and improve Jack integration.

### Upstreaming strategy
- Submit kernel and U-Boot changes to NXP's `meta-imx` and `linux-imx` projects.
- Contribute recipe updates to upstream layers such as `meta-openembedded` and `meta-flutter`.
- Follow the Yocto Project contribution workflow (sign-off, mailing lists or GitHub pull requests) to merge patches into the official distribution.
# Host Setup
To get the Yocto Project expected behavior in a Linux Host Machine, the packages and utilities described
below must be installed. An important consideration is the hard disk space required in the host machine. For
example, when building on a machine running Ubuntu, the minimum hard disk space required is about 50 GB.
It is recommended that at least 120 GB is provided, which is enough to compile all backends together. For
building machine learning components, at least 250 GB is recommended.

The recommended minimum Ubuntu version is 20.04 or later.

### 1. Host packages
```
$ sudo apt install gawk wget git diffstat unzip texinfo gcc build-essential
 chrpath socat cpio python3 python3-pip python3-pexpect xz-utils debianutils
 iputils-ping python3-git python3-jinja2 python3-subunit zstd liblz4-tool file locales libacl1
```

### 2. Build configurations
```
$ DISTRO=<distro name> MACHINE=<machine name> source imx-setup-release.sh -b
 <build dir>

eg.
build debix model ab
$ EULA=1 DISTRO=fsl-imx-xwayland MACHINE=imx8mp-lpddr4-evk source imx-setup-release.sh -b Model_AB_Infinity
$ bitbake imx-image-full
```

 