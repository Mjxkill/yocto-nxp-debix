# REPRODUCIBILITY — V7.0 Debix Audio Platform

How to rebuild the **complete** Debix Model AB audio platform from a
clean git clone — board firmware + kernel + DSP firmware + topology +
userspace daemons + GUI — as it exists today on the deployed board.

> **Source de vérité** : ce document. Si une étape diverge de ce qui
> est sur la board, c'est ici qu'il faut la corriger en premier (et
> commit).

---

## 0. État cible

À la fin de la procédure tu auras une carte Debix Model AB qui boot avec :

| Composant | Origine | Path sur la carte |
|---|---|---|
| Kernel Linux 6.6.36 + patches V7 | `meta-local/recipes-kernel/linux/` | `/boot/Image` |
| Module `snd-soc-tac5212.ko` | idem (built-in via tac5212.c) | `/lib/modules/$(uname -r)/kernel/sound/soc/codecs/` |
| DT carves NPU tap (in+out) | `apply-npu-tap-dt.py` | `/boot/<board>.dtb` |
| SOF firmware custom (HiFi4 DSP) | recipe `sof-firmware-custom` | `/lib/firmware/imx/sof-zephyr-xcc/sof-imx8m.ri` |
| Topology TAC5212 | recipe `sof-firmware-custom` | `/lib/firmware/imx/sof-tplg/sof-imx8mp-tac5212.tplg` |
| `tac-reset` script + service | recipe `tac5212-service` | `/usr/bin/tac-reset` + `/etc/systemd/system/...` |
| `mixer-pro` daemon | recipe `mixer-pro` | `/usr/bin/mixer-pro` |
| `mixer-gui-http` daemon + GUI | recipe `mixer-gui-http` | `/usr/bin/mixer-gui-http` + `/var/www/mixer-gui/index.html` |
| systemd units mixer-pro + mixer-gui-http | recipes idem | `/lib/systemd/system/` |

Audio : 4 TAC5212 (0x50-0x53) sur SAI7 TDM 8ch × 32 bit @ 48 kHz, latence
~8-14 ms one-way mixer.

---

## 1. Prérequis machine de build

- Linux Ubuntu 22.04+ ou équivalent
- ≥ 120 GB libres
- ≥ 32 GB RAM (16 GB OK mais lent)
- `git`, `python3`, `chrpath`, `diffstat`, `gawk`, `wget`, `cpio`, `texinfo` etc. (cf. Yocto Project Quick Start)
- Pour PDF docs : `google-chrome` (`--headless --print-to-pdf`)

---

## 2. Clone du repo principal + submodules

```bash
git clone git@github.com:Mjxkill/yocto-nxp-debix.git
cd yocto-nxp-debix
git checkout feature/v7.0-multiband-drc-tap
git submodule update --init --recursive
```

Submodules tirés : `sources/meta-imx`, `sources/poky`, `sources/meta-openembedded`,
`sources/meta-musicians`, `sources/meta-freescale`, etc. (~25 layers Yocto).

---

## 3. Clone du fork SOF (firmware DSP)

Le firmware DSP HiFi4 n'est PAS automatiquement buildé par Yocto — il
est buildé manuellement puis vendoré dans le recipe `sof-firmware-custom`.
Si tu changes le code SOF, regénère les artifacts.

```bash
cd /home/michael/yocto-nxp-debix
git clone git@github.com:Mjxkill/sof.git
cd sof
git checkout feature/v7.0-multiband-drc-tap
# HEAD attendu = 4bea8e59d (V7.0-E7.2 SAI TX FIFO alignment fix)
```

Procédure de **régénération** des artifacts SOF (uniquement si tu
modifies le code SOF) :

```bash
cd /home/michael/yocto-nxp-debix/sof
./scripts/xtensa-build-zephyr.py imx8mp     # nécessite Zephyr SDK + Xtensa toolchain
# IMPORTANT : concatène .ri.xman + .ri (sinon IPC 108/20 au boot)
cat build-imx8mp-xcc/sof.ri.xman build-imx8mp-xcc/sof.ri \
    > /tmp/sof-imx8m.ri

# Topology (alsatplg côté host)
cd tools/topology/topology1
m4 sof-imx8mp-tac5212.m4 | alsatplg -c /dev/stdin -o /tmp/sof-imx8mp-tac5212.tplg

# Drop dans le recipe
cp /tmp/sof-imx8m.ri \
   meta-local/recipes-bsp/sof-firmware-custom/files/sof-imx8m.ri
cp /tmp/sof-imx8mp-tac5212.tplg \
   meta-local/recipes-bsp/sof-firmware-custom/files/sof-imx8mp-tac5212.tplg
git commit -am "sof-firmware-custom: refresh from sof <commit>"
```

Sinon, les artifacts vendorés correspondant à `sof@4bea8e59d` sont
suffisants — passer directement à l'étape 4.

---

## 4. Setup environnement Yocto

```bash
cd /home/michael/yocto-nxp-debix
EULA=1 DISTRO=fsl-imx-xwayland MACHINE=imx8mpevk \
    source imx-setup-release.sh -b Model_AB_Infinity
```

Le sous-dossier `Model_AB_Infinity/` est créé avec `conf/local.conf` et
`conf/bblayers.conf`.

---

## 5. Build de l'image complète

```bash
bitbake imx-image-full
```

Durée première fois : 6-12 heures selon machine + réseau. Builds
incrémentaux : 5-30 min.

L'image `.wic` finale se trouve dans
`Model_AB_Infinity/tmp/deploy/images/imx8mpevk/`.

> **Dette RÉSOLUE (revue code 2026-07-28, F1)** : l'échec `do_rootfs` sur
> `kernel-module-imx-audio-tap` venait de l'installation directe du paquet
> versionné ; l'image installe désormais le méta-paquet `imx-audio-tap`
> (qui RDEPENDS sur `kernel-module-imx-audio-tap-${KV}` via Provides deb).
> Validé par build image complet 2026-07-28. Autres points reproductibilité
> réglés à la même date : chromium/mixer-kiosk retirés de l'image (la layer
> meta-chromium était de toute façon ignorée sous scarthgap — compat ≤
> nanbield) ; `flash.bin` versionné dans
> `meta-local/recipes-bsp/imx-mkimage/files/` avec garde-fou md5 au build.

---

## 6. Flash de la SD card

```bash
sudo bmap-tools copy \
    Model_AB_Infinity/tmp/deploy/images/imx8mpevk/imx-image-full-imx8mpevk.rootfs.wic.zst \
    /dev/sdX        # ⚠ vérifier /dev/sdX avec lsblk avant !
```

Ou via Etcher / `dd` selon préférence.

---

## 7. Premier boot sur la board

- Insérer la SD
- Booter
- Réseau : la board prend une IP par DHCP. Récupérer-la (ex `arp -a` côté PC, ou via UART console).
- ssh root@<board-ip> (pas de password en config dev — sécuriser pour prod)

Vérifications :

```bash
# Versions
mixer-pro --help                        # → v7.0-e7.5a ou plus récent
curl -s http://localhost:8080/api/state | jq .version

# ALSA cards
aplay -l | grep -i tac5212              # → softac5212tdm card visible
amixer -c softac5212tdm controls | wc -l # → ~339 kcontrols

# Audio test
arecord -D hw:softac5212tdm,0 -c 8 -r 48000 -f S32_LE -d 3 /tmp/test.wav

# GUI
firefox http://<board-ip>:8080/
```

---

## 8. Procédure de mise à jour incrémentale (pas full re-flash)

Quand tu changes le code mixer-pro / mixer-gui-http / kernel
patches / DSP firmware, tu peux pousser uniquement les binaires :

```bash
# Code userspace
bitbake mixer-pro mixer-gui-http
IMG=Model_AB_Infinity/tmp/work/armv8a-poky-linux/mixer-gui-http/1.0/image
scp $IMG/usr/bin/mixer-gui-http root@board:/usr/bin/
scp $IMG/var/www/mixer-gui/index.html root@board:/var/www/mixer-gui/
ssh root@board "systemctl restart mixer-gui-http"

# DSP firmware (après refresh sof/ → cf §3)
bitbake sof-firmware-custom -c cleansstate
bitbake sof-firmware-custom
IMG=Model_AB_Infinity/tmp/work/all-poky-linux/sof-firmware-custom/1.0/image
scp $IMG/usr/lib/firmware/imx/sof-zephyr-xcc/sof-imx8m.ri \
    root@board:/lib/firmware/imx/sof/
scp $IMG/usr/lib/firmware/imx/sof-tplg/sof-imx8mp-tac5212.tplg \
    root@board:/lib/firmware/imx/sof-tplg/
ssh root@board "reboot"
```

---

## 9. Branches et tags de référence

| Repo | Branche prod | HEAD attendu |
|---|---|---|
| `Mjxkill/yocto-nxp-debix` | `feature/v7.0-multiband-drc-tap` | `6511f273` ou plus récent |
| `Mjxkill/sof` | `feature/v7.0-multiband-drc-tap` | `4bea8e59d` ou plus récent |

Avant tout build qui produira un firmware DSP, vérifier que ces deux
branches sont sync et que le hash de `4bea8e59d` correspond à ce qui a
généré les artifacts vendorés.

---

## 10. Documentation associée

- `ARCHI/ARCHI_V7.0.md` — architecture détaillée V7.0 (md/html/pdf)
- `TESTS/TESTS_V7.0_E*.md` — fiches de test par étape (E0 → E7.7)
- `meta-local/recipes-bsp/sof-firmware-custom/README.md` — procédure
  rebuild firmware DSP
- `PROJECT_STATE.md` — état global du projet (vision, architecture,
  branches actives)
- `REGLES.md` — protocole de travail (critic_analyze obligatoire, etc.)

---

## 11. Gaps reproductibilité connus (à clôturer)

| Item | Sévérité | Status |
|---|---|---|
| `kernel-module-imx-audio-tap` packaging deb manquant | bloque `bitbake imx-image-full -c rootfs` | TODO |
| `mixerctl` binaire pas dans `imx-image-full` | non bloquant | TODO |
| systemd units actuellement en `/etc/systemd/system/` (scp manuel historique) au lieu de `/usr/lib/systemd/system/` (recipe) | non bloquant, dual-install OK | acceptable |
| Persistance state mixer (gains, sends, DRC, taps) | sprint dédié | TODO E7.8 ? |
