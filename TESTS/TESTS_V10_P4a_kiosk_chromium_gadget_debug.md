# TESTS V10-P4a — Kiosk Chromium écran DSI + fix flood printk gadget UAC2

Date : 2026-07-04

## Versions

| Artefact | Référence |
|---|---|
| Kernel | Image 6.6.36-rt35, md5 `4ecfa2ae5b24418e94f1e879bf5be923` (CONFIG_USB_GADGET_DEBUG désactivé), backup `/boot/Image.bak-pre-v10p4a` |
| Chromium | chromium-ozone-wayland 117.0.5938.132 (.deb + libcxx/nspr/nss/upower) |
| beta.html | GUI P2l + mode PANEL (30 fps, waterfall off, upscale 1.25) |
| mixer-gui-http | pool MHD 16, buffers par requête, /api/client-log |
| Commits | `246b1992` (kernel), `f9538b26` (rotation+upscale), série P2d-P2m (panneaux) |
| weston.ini board | `[output] name=DSI-1 transform=rotate-90` (backup .bak-v10) — **à persister en recette** |

## Découverte majeure : flood printk gadget UAC2

`CONFIG_USB_GADGET_DEBUG=y` (defconfig NXP) compilait les `pr_debug` du
gadget : **2 printk par paquet USB (~4000 lignes/s) pendant toute lecture**
(`u_audio.c:208 p_srate/p_residue_mil`). Conséquences mesurées :
- journald jusqu'à 53 % + syslogd 18 % du core 0 — en permanence pendant
  la lecture, depuis toujours (suspect des glitchs résiduels V8.33)
- rotation du journal en minutes (perte des traces de diagnostic)
- contention printk dans le chemin de complétion USB

## Mesures (delta xruns sur 60 s de lecture bruit rose USB)

| Condition | xruns/60 s | cpu0 / cpu1 | RSS kiosk |
|---|---|---|---|
| AVANT fix, sans kiosk | 0 | ~50 % (dont journald 31 %) | — |
| AVANT fix, kiosk 60 fps plein rendu | 13 | 99 / 99 % | 176 MB |
| AVANT fix, kiosk mode panel 30 fps | 7-8 | 99 / 99 % | 160 MB |
| APRÈS fix, sans kiosk | 0 | — (journald 0 %) | — |
| **APRÈS fix, kiosk mode panel ×3** | **0 / 0 / 0** | 93 / 85 % | 359-373 MB |

Flood kernel : 11 253 lignes/3 s avant → 10 lignes/3 s après.

## Config kiosk validée (GO)

```
systemd-run --unit=mixer-kiosk \
  CPUAffinity=0-1  MemoryMax=400M  Nice=19  CPUWeight=30 \
  StandardOutput=null StandardError=null \
  chromium --ozone-platform=wayland --kiosk --no-sandbox --noerrdialogs \
           --disable-infobars --log-level=3 http://localhost:8080/panel
```
Page /panel : mode PANEL auto (30 fps, waterfall off, upscale plein écran).
Écran réel : panneau DSI **800×1280 portrait natif** → weston rotate-90
→ 1280×800 paysage (la « 1024×600 » supposée était fausse).

## Verdict : GO Chromium

- 3×60 s lecture + kiosk : 0 xrun, audio core 2 intouché (23 %)
- SSE : 0 drop, slot state OK
- Réserves : RSS 359-373 MB proche du plafond 400M → watchdog RSS à
  prévoir (déjà à l'ARCHI §5) ; cpu0/1 à 85-93 % avec nice 19 = les
  priorités font leur travail mais peu de marge pour de futurs services

## Reste à faire (P4 finalisation)

- Validation visuelle utilisateur : orientation rotate-90 vs 270, tactile
  aligné, rendu de la console sur l'écran
- Persister weston.ini (recette) + activer mixer-kiosk.service
  (RDEPENDS deps chromium : libcxx/nspr/nss/upower dans l'image)
- Identifier le churn TIME_WAIT ~3-4 connexions/s du kiosk (loopback,
  bénin mais à comprendre — probablement des fetch périodiques de la page)
- Écoute utilisateur : les glitchs résiduels V8.33 ont-ils diminué
  (contention printk supprimée) ?

## Test utilisateur : EN ATTENTE (écran + écoute)
