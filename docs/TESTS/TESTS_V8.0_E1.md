# Test Fiche : V8.0 — E1 (USB UAC2 8×8 gadget activé dans mixer-pro)

**Date** : 2026-05-14
**Statut** : **PENDING USER VALIDATION** — code en place, validation board à faire
**Commits** :
- (à committer) `meta-local: V8.0-E1 — retire --no-uac2 + graceful-degrade si gadget absent`

## Référentiel

| Champ | Valeur |
|---|---|
| Phase | V8.0 — E1 USB UAC2 8×8 fonctionnel |
| Préalable | V7.0-E7.7b GO (reproductibilité comblée) |
| mixer-pro version | `v8.0-e1` |
| Recipe USB gadget | `meta-local/recipes-bsp/usb-uac2-gadget/` (déjà présent depuis V7.0-E6.a) |
| DT USB mode | `dr_mode = "otg"` sur usb_dwc3_0 (38100000.usb) — câble USB-C à brancher en device |
| UDC attendu | `/sys/class/udc/38100000.usb` |
| PCM attendu | `hw:UAC2Gadget,0` (8 ch in + 8 ch out S32_LE 48 kHz) |

## Travaux exécutés (code)

### 1. `mixer-pro.service` — retire `--no-uac2`

```diff
- Description=V7.0-E6.d mixer-pro daemon (console DAW SW, 26 in / 4 bus FX / 18 out)
+ Description=V8.0-E1 mixer-pro daemon (console DAW SW, 26 in / 4 bus FX / 18 out)
  After=sound.target snd-aloop-phone.service usb-uac2-gadget.service
+ Wants=usb-uac2-gadget.service
  ...
- ExecStart=/usr/bin/mixer-pro --no-uac2 --no-phone
+ ExecStart=/usr/bin/mixer-pro --no-phone
```

Justification : le recipe `usb-uac2-gadget` est dans `IMAGE_INSTALL` depuis V7.0-E6.a
et son service systemd est `SYSTEMD_AUTO_ENABLE = "enable"`. Le ConfigFS bind l'UDC
au boot, ce qui enregistre `hw:UAC2Gadget,0` côté ALSA. Le flag `--no-uac2` était
un workaround historique pour démarrer mixer-pro sans dépendance au gadget — il
n'a plus lieu d'être maintenant que le gadget est setup côté board. `Wants=` est
ajouté pour s'assurer que le gadget est démarré (et non juste "attendu") avant
mixer-pro.

`--no-phone` est conservé car le recipe `snd-aloop-phone` est encore en validation
(à activer dans un sprint séparé).

### 2. `mixer-pro.c` — graceful-degrade UAC2 + Phone

Avant : si `pcm_open(hw:UAC2Gadget,0)` échouait (gadget pas bindé, câble USB
absent au boot, kernel sans driver), mixer-pro die avec `goto err`. Avec
`After=usb-uac2-gadget.service` qui a `ConditionPathExists=/sys/class/udc/...`,
le service gadget est SKIP success si pas d'UDC → mixer-pro die quand même.

Après : si le `pcm_open` UAC2 (cap OU play) échoue, on bascule en mode
`g_skip_uac2 = 1` dynamiquement avec un log warning. Le daemon continue avec
les 8 mics DSP fonctionnels. Idem pour Phone.

```c
if (!g_skip_uac2) {
    if (pcm_open(cap_uac2, PCM_UAC2_CAP, ...) < 0 ||
        pcm_open(play_uac2, PCM_UAC2_PLAY, ...) < 0) {
        mlog("UAC2Gadget PCM unavailable — running without UAC2");
        g_skip_uac2 = 1;
    }
}
```

Comportement existant des threads `audio_thread`/`play_thread` (qui checkent
`if (g_skip_uac2)` dynamiquement à chaque period) est préservé bit-exact.

### 3. `MIXER_VERSION` bumpée

`v7.0-e7.5a` → `v8.0-e1`.

## Tests T8.1.X — à exécuter sur board

| Test | Commande | Résultat attendu |
|---|---|---|
| T8.1.1 service gadget démarré | `systemctl status usb-uac2-gadget` | `active (exited)` |
| T8.1.2 UDC bindé | `cat /sys/kernel/config/usb_gadget/g1/UDC` | `38100000.usb` |
| T8.1.3 PCM enuméré | `aplay -l \| grep -i UAC2Gadget` | 1 carte listée |
| T8.1.4 mixer-pro démarre sans --no-uac2 | `systemctl status mixer-pro` | `active (running)` + log absence "running without UAC2" |
| T8.1.5 host PC voit Debix USB | `lsusb` côté PC | `Linux Foundation Multifunction Composite Gadget` + descripteur "Debix UAC2 8x8" |
| T8.1.6 host PC ouvre device audio | `arecord -D plughw:Debix -c 8 -r 48000 -f S32_LE -d 3 /tmp/usb-cap.wav` côté PC | wav 8 ch 3 s sans erreur |
| T8.1.7 host PC joue vers Debix | `aplay -D plughw:Debix sample-8ch.wav` côté PC | son routé via mixer-pro → master matrix → DSP play |
| T8.1.8 mixer-pro routing test | Dans la GUI, ouvrir strip U1 (USB in 1) → router vers S1 (DSP play 1) à 0 dB | Audio host PC sort sur speaker 1 |
| T8.1.9 reverse routing test | Dans la GUI, router M1 (mic DSP 1) → U1 (USB out 1) | Audio mic 1 enregistrable côté PC via `arecord` |
| T8.1.10 graceful degrade test | Débrancher câble USB-C, redémarrer board, démarrer mixer-pro | Log `UAC2Gadget PCM unavailable...`, daemon reste UP, DSP-only fonctionnel |

## Risques connus à valider

1. **DT dr_mode = "otg"** : si l'ID pin du connecteur USB-C est mal-câblée ou
   si le PC host ne fournit pas un signal "device" clair, l'UDC peut ne pas
   apparaître. Workaround si problème : forcer `dr_mode = "peripheral"` via
   un `apply-usb-peripheral-dt.py` à ajouter au `linux-imx_%.bbappend`.
2. **Latence USB ajoutée** : l'UAC2 ALSA buffer minimal côté gadget est
   typiquement 4-8 ms. À mesurer empiriquement avec un test loopback host→board→host.
3. **CPU cost** : 2 PCMs additionnels (UAC2 cap+play 8 ch) ajoutent ≈ 5%
   CPU sur audio_thread/play_thread. À surveiller avec E2 (loads).

## Validation utilisateur attendue

> "OK / KO — précisions"

→ **À renseigner après reboot board et tests T8.1.1 à T8.1.10**

## Suite envisageable (sprint E2)

E2 = loads CPU 4-core + DSP affichés temps réel dans la GUI.
- mixer-pro `loads_thread` lit `/proc/stat` toutes 500 ms
- Patch SOF firmware : compteur idle/busy via `k_cycle_get_32()` wrappé
  autour de `dma_task_run()` dans `zephyr_dma_domain.c`, publication
  mailbox SW REG
- Kernel reader debugfs `/sys/kernel/debug/sof/mbox`
- GUI widget topbar : 4 mini-bars CPU + 1 bar DSP, code couleur
