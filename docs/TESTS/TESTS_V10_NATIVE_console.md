# TESTS V10-NATIVE — Console A.L.A. native Qt6/eglfs (N0 → N6)

Dates : 2026-07-05 · Décision utilisateur : interface écran native (chromium
trop gourmand), sans démarrer wayland.

## Versions finales

| Artefact | Référence |
|---|---|
| mixer-console | 0.1 (Qt6/QML eglfs), service systemd enable, commit `141be4c2` |
| mixer-pro | contrôle MULTI-CLIENT poll 8 slots (`f3900867`) + get_meters_lite (`9edc6f75`) |
| Kernel | + tactile GT911 DT (`9c538a12`), gadget-debug off (`246b1992`), panel brightness v2 |
| flash.bin | PATCH BINAIRE A.L.A. (logo 160x90 + bandeau 21=21 chars, `8472cb1d`) |
| beta.html (web) | P4f — sync bidirectionnelle, fix codec DRC attack partagé |
| Calibration tactile | écran 5 mires intégré (LSQ+composition), règle udev en recette |

## Chaîne de boot validée (reboot à froid sans intervention)

logo A.L.A. U-Boot → kernel → mixer-pro/gui-http/ml-inference (auto) →
mixer-console (auto) → dalle à 255 (in-app post-1re-frame) → intro animée
A.L.A. (métal + reflet balayant, GPU) → console. TDM aligné ce boot.

## Mesures clés

| Métrique | Chromium kiosk | Native N6 |
|---|---|---|
| CPU (lecture, page MIXER animée) | 85-93 % × 2 cores | 35 % (tick 22 Hz) / 60 % (vsync 44 fps, mode retenu) |
| RAM | 360-373 MB | ~69 MB |
| Compositeur | weston requis | aucun (DRM/KMS direct) |
| Xruns 3×60 s lecture | 0 (avec priorités) | 0 |
| Dalle | 44 Hz natif (44 FPS = vsync parfait) | idem |

## Leçons techniques durables (voir aussi memory)

1. Canvas QML repeint par frame + shadowBlur = poison CPU (117 %→) ;
   aiguilles = Rectangle rotation GPU, cadran statique.
2. Ballistique par frame vsync (FrameAnimation) = LA solution anti-beat ;
   un QTimer n'est jamais en phase avec le vsync (battement 2/3 vsync).
3. QVariantList lu dans un binding = conversion complète par lecture →
   distribution impérative 1×/tick.
4. perf : 42 % des cycles dans libGAL/GLESv2 (driver Vivante, coût par
   frame rendue) — la cadence de rendu EST le levier CPU.
5. MouseArea ne suit que le 1er touchpoint (point fantôme → taps sourds) ;
   TapHandler par-point pour toute navigation tactile.
6. Le modeset de l'app RE-INITIALISE le panneau (brightness→12) après
   toutes les écritures udev/tmpfiles → luminosité in-app post-frame.
7. Autotest round-trip du codec DRC → a attrapé un bug LATENT du web
   (clamp attack 1 ms altérait les blobs réels < 1 ms) ; verrou APPLIQUER.
8. flash.bin : patch binaire à tailles égales OK (FIT sans hash vérifié) ;
   ne JAMAIS recompiler u-boot sur cette board (ne boote pas).

## N6e/N6f — Boot 100 % A.L.A. (logo kernel + persistance pendant tac-reset)

Le « gros logo debix » restant au boot était le logo fbcon du KERNEL :
Polyhex substitue `drivers/video/logo/logo_linux_clut224.ppm` (DEBIX 770×192,
CONFIG_LOGO_LINUX_CLUT224=y). Ni U-Boot, ni boot.scr (vérifié : texte
uniquement, aucune commande `bmp display`).

| Changement | Détail |
|---|---|
| Logo kernel | PPM A.L.A. PLEIN ÉCRAN 800×1280 (dessiné paysage 1280×800 puis rotate 90° CCW — fbcon n'oriente pas), réplique la géométrie de l'IntroOverlay (mêmes textes/tailles/couleurs, SANS barre de progression — demande utilisateur, emplacement réservé pour position identique). ASCII P3 obligatoire (pnmtologo refuse P6 binaire). Générateur archivé : `files/gen-ala-kernel-logo.py` |
| Persistance logo | getty@tty1 désactivé (c'était LUI qui effaçait le fb pendant le tac-reset) — board + ROOTFS_POSTPROCESS_COMMAND image. `vt.global_cursor_default=0` dans bootargs (curseur clignotant). Login local : série + ssh |
| boot.scr | Rebrandé A.L.A. (echo + bootargs `ALA_Electrosens V1.0`), recette boot-script-rt alignée (mkimage -n, LIC_FILES_CHKSUM) |
| Intro app | Reste affichée tant que `tac-reset.service` tourne (oneshot+RemainAfterExit : `activating`→`active`, sondé 500 ms via BootStatus/QProcess puis arrêt). Barre 3 étapes : 25 % démarrage → 65 % « INITIALISATION DES CONVERTISSEURS… » → 100 % connecté → fondu. Avant : intro fondue à ~23 s, tac-reset fini à 25,7 s → transitoires visibles sur les VU-mètres |

Chaîne complète validée à froid : logo A.L.A. U-Boot → logo A.L.A. kernel
plein écran (identique à l'intro, reste affiché en continu) → intro animée →
console. **Test utilisateur : « c'est parfait » — VALIDÉ 2026-07-05.**

## Écarts / reste à faire

- Sprint SOF « priming SAI RX » (voies décalées aléatoirement au boot,
  workaround restart mixer-pro ; idée validée : mire tone-generator TAC)
- Persistance des formes RBJ biquads (session seulement, web = localStorage)
- Retrait chromium/weston de l'IMAGE (encore installés en fallback)
- CPU 60 % en vsync plein : levier = réduction du coût/frame Vivante
- fw_setenv absent (reliquat debix_version dans bootargs, invisible)

## Test utilisateur

- Tactile : calibration 5 mires « parfaite » — VALIDÉ
- Pages + panneau effets piste : en cours d'usage
- Boot complet A.L.A. : séquence vue, VALIDÉ (luminosité OK au dernier reboot)
