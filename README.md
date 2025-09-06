# New features
A summary of the main new features is as follows.
New features added for all supported boards:
- Upgraded the kernel to 6.6.36 with consolidated Linux Factory Kernel.
- Upgraded the U-Boot to v2024.04 with consolidated Linux Factory U-Boot.
- Updated EULA to v57 July 2024.
- Upgraded the Yocto Project to version 5.0 Scarthgap.
- Supports the GCC 13.3 toolchain.
- Supports Glitch Detection (GDET) on i.MX 93.
- Provides Flutter SDK, Dart runtime, and a Flutter embedder for embedded application development.
- Cortex-M33 update for 8ULP and i.MX 93, Cortex-M7 updates for i.MX 8M Nano, i.MX 8M Plus, and i.MX 95,
and Cortex-M4 update for i.MX 7ULP, i.MX 8M Mini, and i.MX 8M Quad.
- Since LF6.6.3_1.0.0, Pipewire has become the default audio service, see the i.MX Linux User's Guide
(UG10163) to enable it by default. PulseAudio has been removed from this release.
- Security
  1. OP-TEE upgraded to 4.2.0.
- Multimedia updates:
  1. Gstreamer upgraded to 1.24.0.
  2. Switched to playbin3 as the default A/V playback backend.
  3. Supports Hantro VPU V4L2 codec interfaces, used in Gstreamer by default.
  4. Upgraded Sound Open Firmware to v2.10.0
- Graphics updates:
  1. GPU driver upgraded to 6.4.11.p2.8 with Vulkan enablement, bug fixes, and performance optimizations.
  2. GPU SDK upgraded to 6.2.4.
  3. Chromium upgraded to 117.0.5938.132.
- Machine Learning updates:
  1. OpenCV upgraded to 4.10.0.
  2. TensorFlow Lite upgraded to 2.16.2 with GPU acceleration.
  3. i.MX 93 Vela upgraded to 3.12.
- i.MX 8M Plus
  1. Updates for ISP 4.2.2.24.3.
- i.MX 91
  1. Introduction for i.MX 91 11x11 as GA quality.
  2. Introduction for i.MX 91 9x9 as GA quality.
- i.MX 95
  1. Supports the 15x15 EVK board.
  2. Supports the 19x19 Verdin board.
  3. Supports the Audio board2 and Audio HAT extension board.
  4. Supports the CPUIdle low power function.
  5. Supports the 4K MIPI-HDMI converter card.
  6. Supports the NETC: RSC, XDP
- Arm SystemReady-IR (SR-IR) certification
  1. i.MX 8M Mini EVK board has passed the Arm SR-IR certification.
  2. i.MX 8M Plus EVK board has passed the Arm SR-IR certification.
  3. i.MX 8M Quad EVK board has passed the Arm SR-IR certification.
  4. i.MX 8M Nano EVK board has passed the Arm SR-IR certification.
- Userspace Ethernet DPDK Driver
  1. Supported on i.MX 8DXL and i.MX 91
  2. Supported features on i.MX 95
    – Supported multiqueues
    – Supported VLAN and MAC filtering
    – Supported Link status interrupt status
    – Supported 1G hugepages
    – Supported dpdk-fpr application
    – Supported dpdk-ip_fragmentation and dpdk-ip_reassembly applications
- The following boards are not supported in this release:
  1. i.MX 8QuadXPlus B0 MEK
  2. i.MX 8DXL A1 DDR3L EVK
  3. i.MX 6QuadPlus SABRE-AI
  4. i.MX 6Quad/Dual SABRE-AI
  5. i.MX 6DualLite SABRE-AI  


# Host Setup
To get the Yocto Project expected behavior in a Linux Host Machine, the packages and utilities described below must be installed. An important consideration is the hard disk space required in the host machine. For example, when building on a machine running Ubuntu, the minimum hard disk space required is about 50 GB. It is recommended that at least 120 GB is provided, which is enough to compile all backends together. For building machine learning components, at least 250 GB is recommended.

The recommended minimum Ubuntu version is 20.04 or later. The latest release supports Chromium v91, which requires an increase to the ulimit (number of open files) to 4098.

### 1. Host packages
```bash
$ sudo apt install gawk wget git diffstat unzip texinfo gcc build-essential chrpath socat cpio python3 python3-pip python3-pexpect xz-utils debianutils iputils-ping python3-git python3-jinja2 python3-subunit zstd liblz4-tool file locales libacl1
```

### 2. Build configurations
```
$ DISTRO=<distro name> MACHINE=<machine name> source imx-setup-release.sh -b
 <build dir>

eg.
build debix model ab
$ EULA=1 DISTRO=fsl-imx-xwayland MACHINE=imx8mpevk source imx-setup-release.sh -b Model_AB_Infinity
$ bitbake imx-image-full
```

### Flutter development workflow
After building the image, an SDK with host `flutter` and `dart` tools can be generated:

```
$ bitbake imx-image-full -c populate_sdk
```

Install and source the resulting SDK on your PC to build Flutter applications and
deploy them to the i.MX8MP board over the network (SSH) or via USB networking.
`imx-image-full` already includes machine learning, DSP and audio libraries, so no
additional packages are required for these features.

 