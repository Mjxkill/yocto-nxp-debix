# Test Fiche : V8.33 — RT baseline (isolcpus + self-paced)

**Date** : 2026-05-17
**Statut** : GO (acceptable, glitchs résiduels rares)

## Référentiel

| Champ | Valeur |
|---|---|
| Phase de référence | Phase 1a.3 — V7.0 mixer-pro RT |
| Version de référence | V8.33 |
| Étape | RT baseline : isolcpus + CPUAffinity + self-paced audio_thread |
| Commit yocto-nxp-debix | (à compléter après commit) |
| Branche | `feature/v7.0-multiband-drc-tap` |
| Board IP | 192.168.0.9 |
| Kernel cmdline ajouté | `isolcpus=2,3 nohz_full=2,3 rcu_nocbs=2,3` |
| Recipe boot script | `meta-local/recipes-bsp/boot-script-rt` |
| CPUAffinity mixer-pro | `2 3` |
| CPUAffinity mixer-gui-http | `0 1` |

## Modifications déposées

### Code mixer-pro (V8.33)
- **`audio_thread` self-paced** : `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, ...)` à 2 ms exact entre iters
- Buffer accumulator `buf_acc` côté `cap_uac2_thread` (push atomique 96 ou rien)
- Pre-fill 192 frames côté consumer (audio_thread) via `g_uac2_cap_warm`
- Pre-fill 192 frames côté play (play_uac2_thread → writei zéros tant que ring < 192)
- Ring USB cap = 4 periods = 384 frames = 8 ms
- HW timestamp `snd_pcm_status_get_audio_htstamp` testé mais f_uac2 ne supporte pas → fallback CLOCK_MONOTONIC + appl_ptr + avail
- Drift mesuré + auto-piloté `shift_ppm` (clamp ±500 ppm, désactivé pendant idle)
- ASRC drop/insert ±2 samples par push (moyenne 1 sample interpolé, raccord clean)
- `shift_controller_thread` désactivé (cap_uac2_thread est seul writer de g_shift_ppm)

### Stats timing
- Buckets glissants 10 s (10 × 1 s) pour avg10s wr/rd push/pop
- Min/max globaux persistants (reset uniquement via "Reset Stats" GUI)
- Affichés dans topbar GUI : `wr min/avg10s/max µs`, `rd min/avg10s/max µs`
- Log systemd toutes les 5 s : `journalctl -fu mixer-pro -g TIMING`

### Yocto
- Recipe `boot-script-rt` : compile `boot.cmd` → `boot.scr` via mkimage, installe sur `/boot/boot.scr`
- bbappend `imx-image-full.bbappend` : `IMAGE_INSTALL:append = " boot-script-rt"`
- `mixer-pro.service` : `CPUAffinity=2 3`
- `mixer-gui-http.service` : `CPUAffinity=0 1`

## Devices ALSA + audio

| Device | Card | Rôle | Format |
|---|---|---|---|
| `hw:softac5212tdm,0` | 3 | DSP cap+play 8 ch S32_LE 48 kHz | period=96 buffer=384 |
| `hw:UAC2Gadget,0` | 2 | USB gadget cap+play 8 ch S32_LE 48 kHz | period=96 buffer=384 |

## Tests réalisés (Claude — automatisés sur board)

| # | Test | Commande | Attendu | Résultat |
|---|---|---|---|---|
| 1 | Cmdline contient isolcpus | `cat /proc/cmdline` | `isolcpus=2,3 nohz_full=2,3 rcu_nocbs=2,3` | ✅ OK |
| 2 | CPUs isolés | `cat /sys/devices/system/cpu/isolated` | `2-3` | ✅ OK |
| 3 | Affinity mixer-pro | `taskset -p $(pidof mixer-pro)` | mask `0xc` (cores 2,3) | ✅ OK |
| 4 | Affinity gui-http | `taskset -p $(pidof mixer-gui-http)` | mask `0x3` (cores 0,1) | ✅ OK |
| 5 | Threads sur cores RT | `ps -L -p $(pidof mixer-pro) -o tid,psr,policy` | PSR = 2 ou 3, POL = FF | ✅ OK |
| 6 | mixer-pro --version | `/usr/bin/mixer-pro --version` | `v8.33-selfpaced` | ✅ OK |
| 7 | API drift accessible | `curl -s http://127.0.0.1:8080/api/drift` | JSON avec wr_us_*, rd_us_* | ✅ OK |
| 8 | Stats timing dans GUI | Navigateur, topbar | widgets wr et rd visibles | ✅ OK |
| 9 | Bouton "Reset Stats" | Click GUI | Reset min/max + counters | ✅ OK |

## Mesures empiriques (aplay sine 1 kHz × 25 s, mixer routing in[8..15]→out[0..7])

| Métrique | Sans isolcpus | V8.32 (isolcpus seul) | V8.33 (isolcpus + self-paced) |
|---|---|---|---|
| wr push min | 5 µs | 1336 µs | 439 µs |
| wr push max | **4565 µs** | 2703 µs (-40%) | 3544 µs |
| wr push avg10s | 2003 µs | 2000 µs | 1999 µs |
| rd pop min | 579 µs | 635 µs | 595 µs |
| rd pop max | **4683 µs** | 3341 µs (-29%) | 5959 µs |
| rd pop avg10s | 2001 µs | 1998 µs | 1999 µs |
| cap_empty | ~1050/run (5min) | 1.6M (busy-loop bug) | **12-14 sur 25s = 0.5/s** |
| cap_full | 200/run | minimal | ~1.3k/s (saturation haut OK) |
| xruns_cap | 0-3 | 0 | 17-18 (résiduel) |

## Test utilisateur (chain complète)

**Setup** : MIC → DSP cap → mixer-pro → USB PLAY → DAW (DJ host PC) → USB CAP → mixer-pro → DSP play → HP

**Verdict utilisateur 2026-05-17** :
- **Son acceptable** (vs précédentes versions où il était inutilisable)
- **Latence limite mais réactif** (≈ 8-10 ms cumulés cap+play USB ring)
- **Petits glitchs rares** (cap_empty 0.5/s + xruns_cap résiduels)
- GUI réactive : vu-mètres et FFT temps réel OK
- **NON-acceptable pour matériel pro** (cible 0 glitch encore loin)
- ACCEPTABLE pour continuité du projet et étape intermédiaire

## Reste à faire pour 0 glitch (cible pro)

1. **Phase B — Kernel PREEMPT_RT** : patch + rebuild
2. **IRQ affinity USB DWC3 + SAI sur cores 2,3** : `/proc/irq/N/smp_affinity`
3. **PLC (Packet Loss Concealment)** : répéter dernière period au lieu de memset zéros
4. **ASRC matériel i.MX 8M Plus** : utiliser les 4 modules ASRC HW du SoC (gros chantier kernel + SOF)
5. **Asservissement clock DSP sur USB SOF** : driver SAI suit le SOF USB (clean fix audiophile)

## Notes

- Pendant idle (pas d'aplay côté host), la mesure drift via CLOCK_MONOTONIC+avail est invalidée par le garde-fou (rate hors ±1% ou |ppm|>500). shift_ppm conserve sa valeur précédente, pas de dérive folle comme V8.26 (-86000 ppm).
- f_uac2 gadget ne supporte pas `snd_pcm_status_get_audio_htstamp` (test côté board avec mini-prog C). Fallback CLOCK_MONOTONIC + appl_ptr utilisé.
- Le ring USB cap saturé en haut (cap_full ~1.3k/s) est attendu : self-paced 500 Hz vs DSP HW légèrement plus rapide → producer USB pousse trop, le try_push refuse proprement (pas de perte data).
