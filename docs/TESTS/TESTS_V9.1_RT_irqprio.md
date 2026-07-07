# TESTS V9.1 — PREEMPT_RT + per-thread pinning + IRQ kthread prio 90

**Date** : 2026-05-24
**Commit** : `619b21e8` (branche `feature/v7.0-multiband-drc-tap`)
**Kernel** : `6.6.36-rt35 #1 SMP PREEMPT_RT`
**Statut** : ✅ GO — Validé empiriquement par utilisateur ("c'est beaucoup mieux")

## Stack RT déployée

```
┌─────────────────────────────────────────────────────────────────┐
│ Niveau 1 : Kernel linux-imx 6.6.36                              │
│   + patch-6.6.36-rt35 from kernel.org (105K, 165 fichiers)      │
│   CONFIG_PREEMPT_RT=y forcé via do_configure:append sed         │
│   CONFIG_IRQ_FORCED_THREADING_DEFAULT=y                          │
├─────────────────────────────────────────────────────────────────┤
│ Niveau 2 : Bootargs (boot.scr U-Boot)                           │
│   isolcpus=2,3 nohz_full=2,3 rcu_nocbs=2,3 threadirqs           │
├─────────────────────────────────────────────────────────────────┤
│ Niveau 3 : mixer-pro V9.0 per-thread pinning                    │
│   audio_thread (prio 99) + play_thread (prio 98) → core 2       │
│   cap_uac2 + play_uac2 (prio 95) → core 3                       │
│   analyzer (prio 60) + control (OTHER) → cores 0,1              │
│   CPUAffinity service = 0 1 2 3 (cgroup permissif)              │
├─────────────────────────────────────────────────────────────────┤
│ Niveau 4 : irq-prio-rt daemon (V9.1)                            │
│   Poll 5 s, bump 6 IRQ kthreads à prio 90 :                     │
│     - irq/229 30e60000.mailbox[3-0/1] (IPC A53↔HiFi4 DSP)       │
│     - irq/15  30bd0000.dma-controller (SDMA1)                   │
│     - irq/214 30e10000.dma-controller (SDMA2 audio)             │
│     - irq/23  32f10100.usb (DWC3 #1)                            │
│     - irq/24  32f10108.usb (DWC3 #2)                            │
│   Daemon nécessaire car mailbox kthread respawné par SOF        │
└─────────────────────────────────────────────────────────────────┘
```

## Diagnostic — pourquoi PREEMPT_RT seul ne suffisait pas

Sous PREEMPT_RT, chaque IRQ devient un kthread (`irq/N-name`) schedulable. Le
default systemd/PREEMPT_RT met les IRQ kthreads à **prio 50**. Notre audio_thread
est à **prio 80**. Sur le même core, audio_thread préempte le IRQ kthread →
`snd_pcm_readi(cap_dsp)` attend la notification mailbox qui ne vient pas →
blocage 1-2 ms → pic prof_iter 3-5 ms = glitch audible.

C'est une **priority inversion classique** : la tâche RT attend une notif d'un
producteur (IRQ kthread) qui ne peut pas tourner car notre RT task le préempte.

Solution : passer les IRQ critiques à **prio 90** (> audio_thread 80) →
préemption inversée : quand IRQ arrive, le kthread préempte audio_thread,
notifie ALSA, audio_thread reprend immédiatement avec data dispo.

## Tests cyclictest (baseline RT)

```bash
cyclictest -p 80 -t 4 -m -a 2-3 -i 1000 -D 30s -q
```

| Configuration kernel | T0 max | T1 max | T2 max | T3 max |
|---|---|---|---|---|
| **CONFIG_PREEMPT** (V8.33) | 1021 µs | 851 µs | 884 µs | **1225 µs** |
| **PREEMPT_RT** (V9.0+) | **53 µs** | 61 µs | 36 µs | **45 µs** |
| **Gain** | **19×** | **14×** | **25×** | **27×** |

→ Scheduler jitter passe de "ms-scale" à "µs-scale". Conforme à ce qu'on attend
de PREEMPT_RT (cible <100 µs, atteint).

## Tests empiriques mixer-pro (instrumentation V9.1-diag)

### Méthodologie

```bash
# Côté PC, lecture MP3 stéréo → channels 1+2 du gadget UAC2 8ch
ffmpeg -re -i tauri.mp3 -af "pan=8c|c0=FL|c1=FR" -ar 48000 \
       -c:a pcm_s32le -f alsa hw:1,0 -t 90

# Côté board, mesure :
curl -s http://127.0.0.1:8080/api/reset_drift_stats
# (jouer 90 s)
curl -s http://127.0.0.1:8080/api/drift  # histogramme prof_iter + wake_jit
journalctl -u mixer-pro -g "ITER PIC"     # outlier log iter > 3 ms
```

### Histogramme prof_iter_us (audio_thread loop, target 2 ms)

| Bucket | V9.0 (sans IRQ prio bump) | V9.1 (IRQ prio 90) |
|---|---|---|
| < 1.8 ms | 15 528 (32%) | 15 159 (32%) |
| 1.8 - 2.2 ms | 19 782 (41%) | 20 231 (43%) |
| 2.2 - 3.0 ms | 12 401 (26%) | 12 283 (26%) |
| 3.0 - 5.0 ms | **17** (0.04%) | **3 → 0** (-82%) |
| > 5.0 ms | 0 | 0 |

### Outlier breakdown — TOUS pics ont la même signature

```
V9.0 (sans IRQ prio) — typical pic :
  ITER PIC 3400µs wake=6791µs cap=2672µs mix=721µs push=6µs
                              ^^^^^^^^^^
                              cap blocked 1+ms longer than usual (1.4 ms)
                              = mailbox IRQ kthread préempté → readi attend
```

Après IRQ prio 90 bump : les pics tombent à 3 sur 90 s = 1 toutes les 30 s.

### Compteurs ring V8.33 vs V9.1

| Métrique | V8.33 baseline | V9.1 RT + IRQ prio |
|---|---|---|
| `prof_iter_us` avg | 2553 µs | **1999 µs** (target tenu) |
| `prof_cap_us` avg | 1700 µs | 1437 µs |
| `prof_mix_us` avg | 550 µs | 550 µs |
| `wr_us_max` | 3900 µs | 3088-3173 µs |
| `rd_us_max` | 3534 µs | 2937-4283 µs |
| **`cap_full_evt`** | **62/s** | **0-48/s** |
| `cap_empty_evt` | 0.03/s | 0.03/s |
| `xruns_cap/play` | 0 | 0 |

## Test régénération image complète

```bash
bitbake imx-image-full
```

Résultat : ✅ EXIT=0, 16494 tasks all succeeded.
Image : `imx-image-full-imx8mpevk.rootfs-20260524151814.wic` (13.4 GB).

Manifest contient les 5 packages V9.1 critiques :

```
boot-script-rt armv8a 1.0-r0                 (boot.scr avec threadirqs)
irq-prio-rt armv8a 1.0-r0                    (daemon poll 5 s bump prio 90)
kernel-image-6.6.36-rt35 imx8mpevk           (PREEMPT_RT kernel)
kernel-image-image-6.6.36-rt35 imx8mpevk     (binary)
mixer-pro armv8a 1.0-r0                      (V9.0 pinning + V9.1 diag)
```

Reflasher cette `.wic` sur une nouvelle SD régénère un système V9.1 RT
identique à l'actuel.

## Vérification au boot

```bash
$ ssh root@192.168.0.9
$ uname -r
6.6.36-rt35

$ cat /proc/version
Linux version 6.6.36-rt35 ... #1 SMP PREEMPT_RT ...

$ cat /proc/cmdline
... isolcpus=2,3 nohz_full=2,3 rcu_nocbs=2,3 threadirqs

$ systemctl is-active mixer-pro mixer-gui-http irq-prio-rt
active
active
active

$ ps -e -o pid,rtprio,comm | grep 'irq/(229|15|214|23|24)'
80    90  irq/15-30bd0000.dma-controller
92    90  irq/23-32f10100.usb
93    90  irq/24-32f10108.usb
124   90  irq/214-30e10000.dma-controller
1117  90  irq/229-30e60000.mailbox[3-0]
1118  90  irq/229-30e60000.mailbox[3-1]

$ ps -L -p $(pidof mixer-pro) -o tid,psr,policy,rtprio,comm
  TID PSR POL RTPRIO COMMAND
  ...   2 FF      99 mixer-pro    # audio_thread
  ...   2 FF      98 mixer-pro    # play_thread
  ...   3 FF      95 mixer-pro    # cap_uac2_thread
  ...   3 FF      95 mixer-pro    # play_uac2_thread
  ...   1 FF      60 mixer-pro    # analyzer_thread
  ...   0 FF      80 mixer-pro    # control_thread
```

Toute la stack RT V9.1 démarre automatiquement et est persistante.

## Verdict utilisateur

> *"c'est beaucoup mieux ! ce n'est pas parfait il y a encore du boulot de propreté
> et de temps réel à comprendre... nettoie et commit cette version, test une
> regeneration de l'image complète pour voir commit et push"*

→ **GO** pour V9.1 comme baseline RT acceptable. Glitchs résiduels rares
(au-delà du tuning Linux RT). Pistes futures dans la mémoire `[[v8-33-suite]]`
et ARCHI §9 ("V9.x suite — diminishing returns").

## Reste à faire (au-delà du scope V9.1)

Pics résiduels (3/90 s) au-delà du Linux RT. Pistes par effort croissant :

1. **NoC QoS** audio/USB priority (~2-4 h, risque bas)
2. **ALSA buffer cap_dsp 4 → 8 periods** absorbe pics (+8 ms latence, risque bas)
3. **Backport `p_sync=async` f_uac2** kernel 6.10 → 6.6.36 (~1-2 j, risque moyen)
4. **SOF firmware tuning** — mode DOUX, critic_analyze obligatoire (~1-5 j, risque HAUT)

Et au niveau projet (ARCHI V7.0) :
- NPU ingé son ML (skeleton seulement, vrai but du projet)
- GUI test E8 réglage tous effets live
- 2e USB téléphone (actuel = aloop placeholder)
- Support LV2 plugins
- Mémoire effets persistante au reboot
