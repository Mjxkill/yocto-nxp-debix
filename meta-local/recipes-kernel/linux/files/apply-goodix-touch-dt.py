#!/usr/bin/env python3
"""V10-P4c — Ajoute le tactile Goodix GT911 de la dalle DSI 8" (800x1280)
au DT imx8mp-evk.dts.

Noeud repris tel quel de imx8mp-debix-core-TD080B.dts (meme dalle) :
  - i2c2 @ 0x5d (le GT911 repond en 0x14 ou 0x5d selon le niveau d'INT au
    reset ; le driver goodix pilote RST/INT et impose l'adresse du reg)
  - CTP-INT = GPIO1_IO09, CTP-RST = GPIO1_IO14 (pads libres : les groupes
    synaptics_dsx_io et l'ancien usage usb1_vbus sont commentes dans notre DTS)
Idempotent : ne fait rien si goodix,gt911 est deja present.
"""
import re
import sys

TOUCH_NODE = """
\ttouchscreen@5d {
\t\tcompatible = "goodix,gt911";
\t\treg = <0x5d>;
\t\tpinctrl-names = "default";
\t\tpinctrl-0 = <&pinctrl_mipi_tp>;
\t\tinterrupt-parent = <&gpio1>;
\t\tinterrupts = <9 IRQ_TYPE_EDGE_RISING>;
\t\tirq-gpios = <&gpio1 9 GPIO_ACTIVE_HIGH>;\t/* CTP-INT */
\t\treset-gpios = <&gpio1 14 GPIO_ACTIVE_HIGH>;\t/* CTP-RST */
\t\ttouchscreen-size-x = <800>;
\t\ttouchscreen-size-y = <1280>;
\t\tstatus = "okay";
\t};
"""

PINCTRL_GROUP = """
\tpinctrl_mipi_tp: mipitpgrp {
\t\tfsl,pins = <
\t\t\tMX8MP_IOMUXC_GPIO1_IO14__GPIO1_IO14\t0x16
\t\t\tMX8MP_IOMUXC_GPIO1_IO09__GPIO1_IO09\t0x1c4
\t\t>;
\t};
"""


def main(dts_path):
    with open(dts_path) as f:
        src = f.read()

    if "goodix,gt911" in src:
        print("goodix-touch-dt: deja applique, rien a faire")
        return

    # 1. noeud tactile en tete du bloc &i2c2 (avant le #if 0 existant)
    m = re.search(r"&i2c2 \{\n(\tclock-frequency[^\n]*\n"
                  r"\tpinctrl-names[^\n]*\n"
                  r"\tpinctrl-0[^\n]*\n"
                  r"\tstatus = \"okay\";\n)", src)
    if not m:
        sys.exit("goodix-touch-dt: bloc &i2c2 introuvable")
    insert_at = m.end(1)
    src = src[:insert_at] + TOUCH_NODE + src[insert_at:]

    # 2. groupe pinctrl dans &iomuxc — DTC exige les proprietes AVANT les
    # sous-noeuds : on s'ancre sur le premier sous-noeud (pinctrl_hog),
    # jamais sur l'ouverture du bloc (pinctrl-names/pinctrl-0 la suivent).
    anchor = "\tpinctrl_hog: hoggrp {"
    if anchor not in src:
        sys.exit("goodix-touch-dt: pinctrl_hog introuvable")
    src = src.replace(anchor, PINCTRL_GROUP.lstrip("\n") + "\n" + anchor, 1)

    with open(dts_path, "w") as f:
        f.write(src)
    print("goodix-touch-dt: noeud GT911 + pinctrl_mipi_tp ajoutes")


if __name__ == "__main__":
    main(sys.argv[1])
