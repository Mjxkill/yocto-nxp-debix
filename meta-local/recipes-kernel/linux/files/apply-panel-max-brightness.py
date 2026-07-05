#!/usr/bin/env python3
"""V10-N6b — Le panneau HC080IY28026 redémarre avec sa luminosité DCS
interne à 12/255 : la dalle est sombre du kernel jusqu'au tmpfiles.d.
Le driver (ajouté par 0001-imx8mp-evk-audio-mipi.patch) active le
backlight mais ne POUSSE jamais de valeur au panneau à l'allumage.
Ce script insère l'écriture DCS brightness=255 juste après le
set_display_on, dans le driver installé par le patch. Idempotent.
"""
import sys

ANCHOR = """	msleep(100);

	backlight_enable(panel->backlight);
	backlight_enable(panel->backlight2);
"""

INSERT = """	msleep(100);

	/* V10-N6b : le panneau reset sa luminosite DCS a 12/255 -> plein
	 * feux des l'allumage (l'userspace peut ensuite la moduler). */
	ret = mipi_dsi_dcs_set_display_brightness(dsi, 255);
	if (ret < 0)
		dev_warn(dev, "set_display_brightness failed (%d)\\n", ret);

	backlight_enable(panel->backlight);
	backlight_enable(panel->backlight2);
"""


def main(src_path):
    path = src_path + "/drivers/gpu/drm/panel/panel-HC080IY28026-D60.c"
    with open(path) as f:
        src = f.read()
    if "V10-N6b" in src:
        print("panel-max-brightness: deja applique")
        return
    if src.count(ANCHOR) != 1:
        sys.exit("panel-max-brightness: ancre introuvable ou multiple")
    src = src.replace(ANCHOR, INSERT)
    with open(path, "w") as f:
        f.write(src)
    print("panel-max-brightness: DCS 255 insere apres display_on")


if __name__ == "__main__":
    main(sys.argv[1])
