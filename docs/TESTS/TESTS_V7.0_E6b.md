# Test Fiche : V7.0 — E6.b (Téléphone simulé snd-aloop 2×2)

**Date** : 2026-05-11
**Statut** : **GO** — sine 1 kHz aloop bidir préservé -6 dBFS / -12 dB RMS
**Tag git associé** : `v7.0-e6b` (posé après commit groupé E6.b + E6.c)

## Référentiel

| Champ | Valeur |
|---|---|
| Phase | V7.0 — E6.b Téléphone simulé via snd-aloop (placeholder VoIP, faute de modem hardware) |
| Préalable | E6.a GO (tag `v7.0-e6a`, commit yocto `e43f3a40`) |
| Branche yocto | `feature/v7.0-multiband-drc-tap` |
| Pas de modif SOF/kernel/DT/firmware | tout est userspace + module aloop standard |

## Travaux exécutés

| Domaine | Action |
|---|---|
| modprobe | `/etc/modprobe.d/snd-aloop-phone.conf` : `options snd-aloop enable=1 pcm_substreams=1 index=10 id=Phone` |
| modules-load | `/etc/modules-load.d/snd-aloop-phone.conf` : `snd-aloop` |
| service | `snd-aloop-phone.service` oneshot (modprobe snd-aloop) |
| Recipe | `meta-local/recipes-bsp/snd-aloop-phone/snd-aloop-phone_1.0.bb` (allarch, SYSTEMD_AUTO_ENABLE) |
| Image | `IMAGE_INSTALL += " snd-aloop-phone"` dans `imx-image-full.bbappend` |

## Architecture

```
Phone (card 10, snd-aloop 2 ch)
 ├─ hw:Phone,0  — playback (1 substream)  ←─ aplay envoie ici
 │                          │
 │                          ↓ (loopback interne)
 ├─ hw:Phone,1  — capture (1 substream)  →─ arecord lit ici
```

Placeholder pour intégration future :
- Voie A : pile VoIP/SIP qui écrit sur `hw:Phone,0` et lit `hw:Phone,1` côté Linux app
- Voie B : connexion via `alsaloop` (E6.c) au mixer pour router vers DSP/UAC2

## Tests T6b.X

| Test | Description | Résultat | Critère GO |
|---|---|---|---|
| **T6b.1** | Module snd-aloop chargé au boot | ✓ OK (`lsmod | grep snd_aloop`) | présent |
| **T6b.2** | Card 10 `Phone Loopback` visible | ✓ OK (`/proc/asound/cards` row 10) | card visible |
| **T6b.3** | 2 PCMs `hw:Phone,0` + `hw:Phone,1` (play + cap) | ✓ OK (aplay -l + arecord -l) | 2 PCMs présents |
| **T6b.4** | Bidir aloop : aplay sine 1kHz `hw:Phone,0` → arecord `hw:Phone,1` | ✓ OK (peak -6.02 dB / RMS -12.04 dB préservé sur 2 voies) | sine 1kHz visible |
| **T6b.5** | Pas de régression sur les autres cartes (0-5) | ✓ OK | 6 cards + Phone = 7 visibles |

## Mesure empirique

| Métrique | Valeur |
|---|---|
| Card index | 10 (forcé via `index=10` pour éviter collision DSP/UAC2/HDMI) |
| Card id | `Phone` |
| Channels | 2 |
| Format testé | S16_LE 48 kHz (compatible VoIP standard) |
| Sine 1kHz aplay → arecord | -6.02 dB peak / -12.04 dB RMS (input vol 0.5 → -6 dBFS) |

## Notes techniques

### Pourquoi `pcm_substreams=1`

Par défaut, snd-aloop expose 8 substreams par device — overkill pour un placeholder téléphone. On réduit à 1 paire pour clarifier l'usage : 1 voie play + 1 voie cap, point.

### Card index fixe = 10

Force `index=10` pour ne pas dépendre de l'ordre d'enumeration. Préserve la stabilité du mapping `hw:Phone` à travers les reboot et ajouts de cartes.

### Téléphone vrai = futur

Quand un modem hardware (USB cellular dongle, GSM modem, ou puce SIM) ou une pile VoIP (linphone, baresip, etc.) sera intégré, on remplace snd-aloop par le PCM du modem en gardant le nom `Phone` (id stable). Toutes les routes alsa-route-bridge restent compatibles.

## Conclusion

- **Card `Phone` 2×2 disponible** — placeholder fonctionnel pour la 3e paire du mixer Linux
- **Bidir aloop fonctionne** — preuve que les samples traversent sans corruption
- **0 régression** — 6 cards d'origine intactes
- Prêt à être routé via alsa-route-bridge (E6.c) ou plus tard pour pile VoIP réelle
