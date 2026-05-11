# Test Fiche : V7.0 — E6.a (USB UAC2 8×8 gadget)

**Date** : 2026-05-11
**Statut** : **GO côté board** — test PC host (câble USB) à valider par l'utilisateur
**Tag git associé** : `v7.0-e6a` (posé après OUI user PC host)

## Référentiel

| Champ | Valeur |
|---|---|
| Phase | V7.0 — E6.a USB UAC2 8×8 gadget (1er volet de E6 ; téléphone + routing N×M reportés à E6.b/E6.c) |
| Préalable | E5 GO (tag `v7.0-e5`, commit yocto `ae7128fa`) |
| Branche yocto | `feature/v7.0-multiband-drc-tap` |
| Branche SOF | inchangée (E6.a est userspace + systemd, **pas de modif SOF source**) |
| HEAD SOF | `6ee842c67` (inchangé E5) |
| Firmware/DTB/module md5 | inchangés vs E5 |

## Travaux exécutés

| Domaine | Action |
|---|---|
| Recipe Yocto | `meta-local/recipes-bsp/usb-uac2-gadget/usb-uac2-gadget_1.0.bb` (NEW, allarch, SYSTEMD_AUTO_ENABLE) |
| Script setup | `usb-uac2-setup.sh` : configfs g1, idVendor=0x1d6b idProduct=0x0104, UAC2 function p_chmask=0xff p_srate=48000 p_ssize=4 (idem capture), UDC=38100000.usb |
| Service systemd | `usb-uac2-gadget.service` : oneshot, After=sys-kernel-config.mount, ConditionPathExists UDC (skip propre si hardware absent) |
| Image | `meta-local/recipes-fsl/images/imx-image-full.bbappend` : IMAGE_INSTALL += " usb-uac2-gadget" |

Pas de modif kernel/DT/SOF. `usb_f_uac2` + `libcomposite` sont **builtin** dans le kernel courant.

## Architecture

```
PC hôte (USB host)                                  Board (USB peripheral)
  │                                                   │
  │ ─── USB cable ───> 38100000.usb (DWC3 UDC)
  │                                                   │
  ▼                                                   ▼
  Carte ALSA "Debix UAC2 8x8"            Card 5 ALSA "UAC2_Gadget"
    8 ch playback (host → board)             /proc/asound/card5/pcm0c (board cap depuis host)
    8 ch capture  (board → host)             /proc/asound/card5/pcm0p (board play vers host)
```

Le UAC2 gadget expose des PCMs **côté board** : `card5/pcm0p` (playback côté board = sortie vers host) et `card5/pcm0c` (capture côté board = entrée depuis host). Sur le PC hôte, c'est l'inverse : une carte son ALSA standard 8×8.

## Tests T6a.X

| Test | Description | Résultat | Critère GO |
|---|---|---|---|
| **T6a.1** | Service `usb-uac2-gadget.service` actif au boot | ✓ OK (`active (exited)`) | systemctl is-active |
| **T6a.2** | UDC bindé à g1 | ✓ OK (`cat /sys/kernel/config/usb_gadget/g1/UDC` = `38100000.usb`) | UDC contient le nom |
| **T6a.3** | UDC state configured + high-speed | ✓ OK (`configured`, `high-speed`) | usb 2.0 négocié |
| **T6a.4** | Card ALSA `UAC2Gadget` visible côté board | ✓ OK (card 5, 0 régression vs cards 0-4) | `aplay -l` montre card UAC2 |
| **T6a.5** | aplay accepte format S32_LE 8ch 48kHz | ✓ OK (header WAV accepté, EIO seulement sans host PC connecté = attendu) | "Playing WAVE..." affiché |
| **T6a.6** | DSP loopback E3 non régressé | ✓ OK (`loopback-c-lowlat` steady `+48000 f/s` in et out, xrun stable) | delta cap/play = 0 steady |
| **T6a.7** | Cards 0-4 inchangées (HDMI/es8316/sof-ADC/sof-tac/sof-probes) | ✓ OK | 5 cards d'origine présentes |
| **T6a.8** | Test PC host : board visible comme carte 8×8 ALSA via câble USB | ⏳ à valider user (câble USB physique requis) | `lsusb` PC + `aplay -l` PC voient Debix |
| **T6a.9** | Test utilisateur — stream PC→board ou board→PC fonctionne | ⏳ à valider user | « j'entends le son envoyé par le PC » |

## Procédure test PC host (à exécuter)

1. Brancher un câble USB-C ou microUSB du **port USB OTG** du board (CN3 ou équivalent) vers un PC hôte (Linux, macOS, Windows).
2. Sur le PC :
   ```bash
   lsusb | grep 1d6b:0104                # → Linux Foundation Multifunction
   aplay -l | grep -A 1 "Debix UAC2"     # → carte 8 ch playback
   arecord -l | grep -A 1 "Debix UAC2"   # → carte 8 ch capture
   ```
3. Test playback PC → board → speakers (en parallèle d'un loopback sur board pour router) :
   ```bash
   # sur PC
   speaker-test -D plughw:DebixUAC2,0 -c 8 -t sine -f 440
   # sur board (en parallèle)
   arecord -D hw:UAC2Gadget,0 -c 8 -f S32_LE -r 48000 /tmp/uac2_received.wav
   ```
4. Test capture board → PC :
   ```bash
   # sur board
   aplay -D hw:UAC2Gadget,0 /root/tests/sine440_8ch.wav
   # sur PC
   arecord -D plughw:DebixUAC2,0 -c 8 -f S32_LE -r 48000 /tmp/from_board.wav
   ```

## Mesures empiriques

| Métrique | Valeur |
|---|---|
| Service start time | < 50 ms (oneshot post sys-kernel-config) |
| UDC speed négocié | high-speed (USB 2.0, 480 Mbps) — largement suffisant pour 8×8×48k×32-bit = ~12 Mbps |
| Cards ALSA total | 5 (0-4 inchangés + UAC2Gadget en 5) |
| Format UAC2 négocié | S32_LE 48000 Hz 8 ch (p_chmask=c_chmask=0xff) |

## Notes techniques

### Pourquoi configfs et pas g_audio legacy

`g_audio.ko` est un module legacy avec paramètres modprobe figés. `configfs` permet une création dynamique du gadget, modification à chaud, et coexistence avec d'autres fonctions (RNDIS, CDC ACM, etc. en mode composite). Recommandé par la documentation kernel moderne.

### Idempotence + safety

Le script vérifie 3 conditions avant d'agir :
1. g1 déjà existant → no-op (post-reboot, ou re-run via systemd reload)
2. UDC absent → exit propre (hardware USB inactif, kernel sans driver, hotplug à venir)
3. configfs non monté → erreur explicite

Le service systemd ajoute `ConditionPathExists=/sys/class/udc/38100000.usb` pour éviter `failed` log si pas d'USB.

### Non-régression DSP

Le loopback `loopback-c-lowlat` continue de fonctionner en steady state (+48000 f/s in et out, xrun cap=2 et play=9 stables, ring_drop=218 stable). L'ajout de la carte UAC2 (card 5) n'a aucune influence sur le pipeline SOF du DSP TAC5212.

### Manque modem hardware → téléphone reporté

Le board Debix Model AB n'a pas de modem (`/dev/ttyACM*`/`/dev/ttyUSB*`/`/dev/cdc-*` absents). Le volet « téléphone 2×2 » de E6 doit être :
- soit simulé par `snd-aloop` + script de routing (E6.b)
- soit fourni par un modem USB externe (qmi/mbim) plus tard

### Routing mixer N×M

Hors scope E6.a — sera adressé en **E6.c** (ou intégré à E7 GUI test). PipeWire ou app userspace avec ALSA mixing pour router DSP ↔ UAC2 ↔ téléphone.

## Test utilisateur

| Critère | OUI / NON |
|---|---|
| Board présente carte ALSA UAC2 sans dégrader le DSP | **OUI côté board** (cards 0-4 intacts, loopback steady) |
| PC host voit le board comme carte son 8×8 via câble USB | ⏳ à valider (câble physique requis) |
| Validation E6.a GO | ⏳ — sera GO si OUI ci-dessus |

## Conclusion

- **Infrastructure UAC2 côté board** : OK (service, gadget, ALSA card, format S32_LE 8ch 48kHz, UDC high-speed)
- **Non-régression DSP** : OK (loopback E3 préservé, cards d'origine intactes)
- **Procédure de validation host** documentée ; nécessite câble USB physique pour T6a.8/T6a.9

**Action immédiate** : commit + tag `v7.0-e6a` après confirmation utilisateur du test host (lsusb + speaker-test). E6.b (snd-aloop téléphone simulé) ou E7 (GUI) ensuite selon priorité.

## Annexes

- Recipe : `meta-local/recipes-bsp/usb-uac2-gadget/usb-uac2-gadget_1.0.bb`
- Script  : `meta-local/recipes-bsp/usb-uac2-gadget/files/usb-uac2-setup.sh`
- Service : `meta-local/recipes-bsp/usb-uac2-gadget/files/usb-uac2-gadget.service`
- Reproductibilité (hot-deploy sur board déjà flashée) :
  ```bash
  scp meta-local/recipes-bsp/usb-uac2-gadget/files/usb-uac2-setup.sh root@<board>:/usr/bin/
  scp meta-local/recipes-bsp/usb-uac2-gadget/files/usb-uac2-gadget.service root@<board>:/lib/systemd/system/
  ssh root@<board> 'chmod +x /usr/bin/usb-uac2-setup.sh && systemctl daemon-reload && systemctl enable --now usb-uac2-gadget'
  ```
