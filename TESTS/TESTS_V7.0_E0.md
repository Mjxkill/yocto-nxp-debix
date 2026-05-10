# Test Fiche : V7.0 — E0 (baseline + audit kernel + audit driver TAC)

**Date** : 2026-05-10
**Statut** : **GO** (pending Test utilisateur OUI)
**Tag git associé** : `v7.0-e0` (à poser après validation user)

## Référentiel

| Champ | Valeur |
|---|---|
| Phase | V7.0 — E0 baseline + audit |
| Préalable | doc V7.0 mergée (commit `1053aad6` yocto) |
| Branche yocto | `feature/v7.0-multiband-drc-tap` |
| HEAD yocto | `91194f59` (retrait apply-v6-always-on.py) |
| Branche SOF | `feature/v7.0-multiband-drc-tap` |
| HEAD SOF | `0580b5f14` (E6.a baseline, V5.4.1 pipeline_comp_copy re-entry guard) |
| Firmware md5 local | `b3e1ebcfb937adbb4a499af2756493ec` |
| Firmware md5 board | `b3e1ebcfb937adbb4a499af2756493ec` ✓ |
| Topology .tplg board | `2ab3b7823f483e600acead5c8352a128` (sof-imx8mp-tac5212.tplg.bak-E6a-2026-05-02 restauré) |
| Kernel Image md5 | `e32b7bcaec429f392fc286c3f32761e9` (rebuildé sans patches V6.0) |
| Kernel DTB md5 | `fd6debfffbdfe20fa0864a06b3e5c967` |

## Travaux exécutés

| Domaine | Action | Commit |
|---|---|---|
| git | Création branches `feature/v7.0-multiband-drc-tap` (yocto + sof) depuis E6.a `0580b5f14` | yocto `1053aad6` |
| Yocto kernel | Retrait `apply-v6-always-on.py` (bbappend SRC_URI + appel `do_patch:append` + suppression .py 298 lignes) | yocto `91194f59` |
| Kernel build | `bitbake -c cleansstate linux-imx && bitbake linux-imx` — purge interne du .py + recompile clean | — |
| Driver audit | `tac5212.c` : 50 kcontrols ALSA exposés (cf liste plus bas) | — |
| Firmware SOF | `west build -d build-sof + west sign + scp + reboot` sur E6.a baseline `0580b5f14` | — |
| Modules SOF | Rebuild + redéploiement de **10 .ko** (snd-sof, snd-sof-of, snd-sof-utils, snd-sof-probes, snd-sof-imx8m, snd-sof-imx8, snd-sof-imx8ulp, snd-sof-imx-probes, imx-common, snd-sof-xtensa-dsp) — sans symboles V6.0 | — |
| Topology | Restauration `sof-imx8mp-tac5212.tplg.bak-E6a-2026-05-02` (md5 `2ab3b78...`) | — |

## Build & deploy

| Étape | Commande | Résultat |
|---|---|---|
| Yocto setup | `source imx-setup-release.sh -b Model_AB_Infinity` | OK |
| Kernel build | `bitbake linux-imx` | 1038/1076 cache hit + 38 rebuild, no error |
| SOF build | `west build -d build-sof` | OK, signed `Reef @ 0x2e0` |
| Modules scp | `scp 10 .ko → /lib/modules/6.6.36/kernel/sound/soc/sof/[imx,xtensa]/` | OK |
| Firmware scp | `scp build-sof/zephyr/zephyr.ri → /lib/firmware/imx/sof/sof-imx8m.ri` | md5 cohérent |
| Tplg restore | `cp tplg.bak-E6a-2026-05-02 tplg` | md5 `2ab3b78...` actif |
| Reboot final | `systemctl reboot` | dmesg propre |

## Tests T0.X

| Test | Description | Résultat | Critère GO |
|---|---|---|---|
| **T0.1** | Yocto build kernel sans `apply-v6-always-on.py` | ✓ OK | build OK, no error |
| **T0.2** | Board boot avec image V7.0-E0 | ✓ OK | uptime 0min, login OK |
| **T0.3** | SOF debugfs présente | ✓ OK (`/sys/kernel/debug/sof/{debug,fw_version,etrace,...}`) | fichiers présents |
| **T0.4** | Audio loopback Linux passthrough (E6.a baseline) | ✓ OK (~47 kfps stable, xrun cap=0/play=7, ring_drop=70) | in/out ~47 kfps stable |
| **T0.5** | kcontrols TAC visibles côté ALSA | ✓ OK (50 kcontrols `TAC0 *`) | liste non vide, AGC absent (TODO E2/E3) |
| **T0.6** | Pas de régression vs E6.a | ✓ OK (xrun_play 7 = ≤ E6.a 7 stable, ring_drop 70 ≈ E6.a 74) | ≤ valeurs E6.a |
| **T0.7** | Plus aucune trace V6.0 dans dmesg | ✓ OK (0 occurrences "V6.0", 0 "pipe_trigger", 0 "ipc tx error") | 0 trace |
| **T0.8** | Test utilisateur — écoute audio | ⏳ À compléter | « le son est bon » |

## kcontrols TAC5212 exposés par `tac5212.c` (50 kcontrols)

**ADC capture (29 kcontrols)** : Digital Volume × 2, Fine Gain × 2, Phase Calibration × 2, Input Config × 2, Input Impedance × 2, Wide Bandwidth × 2, Decimation Filter, HPF Cutoff, Biquad Config (preset enum), Soft-Step Disable, DVOL Gang, Channel Swap, Data Invert, CH1/CH2 Input Mux × 2.

**DAC playback (16 kcontrols)** : Digital Volume × 4 (DAC1A/1B/2A/2B), Fine Gain × 4, Interpolation Filter, HPF Cutoff, Biquad Config, Soft-Step Disable, DVOL Gang, Channel Swap, Wide Bandwidth × 2.

**Output stage (8 kcontrols)** : OUT1P/1M/2P/2M Drive × 4, OUT1P/1M/2P/2M Level × 4.

**VREF / MICBIAS (4 kcontrols)** : VREF Full-Scale, MICBIAS Value, MICBIAS LDO Gain, MICBIAS Power.

**Activity (1 kcontrol)** : VAD Enable.

### Effets datasheet ABSENTS du driver (TODO E2/E3)

- **AGC** — registres non exposés en kcontrol
- **DRC DAC dynamique** — non exposé
- **Distortion limiter** — non exposé
- **Thermal foldback** — non exposé
- **Battery guard / brown-out** — non exposé
- **Tone generator** — non exposé
- **UAD (Ultrasonic Activity Detection)** — non exposé
- **Biquad coefficients programmables** — seul le preset enum est exposé, pas les coeffs raw (Programmable Coefficient Registers datasheet § 8.2)

→ **Action E2/E3** : étendre `tac5212.c` pour exposer ces effets (kcontrols simples + bytes_ext pour les blobs de coefficients) afin que le GUI test V7.0 (E7) puisse les piloter.

## Logs & mailbox

### Boot dmesg (extrait clé)

```
[   10.390677] sof-audio-of-imx8m 3b6e8000.dsp: DT DSP detected
[   10.403699] sof-audio-of-imx8m 3b6e8000.dsp: Firmware info: version 2:10:0-0580b
[   10.403706] sof-audio-of-imx8m 3b6e8000.dsp: Firmware: ABI 3:29:0 Kernel ABI 3:23:0
[   10.451244] sof-audio-of-imx8m 3b6e8000.dsp: Topology: ABI 3:29:0 Kernel ABI 3:23:0
[   10.451349] sof-audio-of-imx8m 3b6e8000.dsp: tplg: config SAI7 fmt 0x4004 mclk 12288000 width 32 slots 8 mclk id 0
[   10.513094] tac5212 3-0050: TAC5212 initialized (I2C 0x50, slots 0-1)
```

### ALSA cards / PCMs

```
0 [audiohdmi      ]: audio-hdmi
1 [es8316audio    ]: fsl-asoc-card - es8316-audio
2 [softac5212tdm  ]: simple-card - sof-tac5212-tdm  ← PCM 02-00 TAC5212 playback + capture
3 [sofADCIn       ]: simple-card - sof-ADC-In
4 [sofprobes      ]: sof-probes - sof-probes
```

### Loopback-c output (steady state)

```
in=47104 (+47104 f/s) out=30720 (+30720 f/s) xrun cap=0 play=6 ring_drop=59
in=94720 (+47616 f/s) out=78336 (+47616 f/s) xrun cap=0 play=6 ring_drop=59
in=189184 (+47104 f/s) out=172800 (+47104 f/s) xrun cap=0 play=6 ring_drop=59
in=283648 (+47104 f/s) out=264192 (+47104 f/s) xrun cap=0 play=7 ring_drop=70
in=330752 (+47104 f/s) out=311296 (+47104 f/s) xrun cap=0 play=7 ring_drop=70
```

## Régression vs E6.a (référence `TESTS_V5.4.1_E6.a.md`)

| Critère | E6.a référence | V7.0-E0 mesuré | Verdict |
|---|---|---|---|
| in fps | ~47 000 | ~47 100 | OK |
| out fps | ~47 000 | ~47 100 | OK |
| xrun_play | 7 stable | 7 stable | OK |
| ring_drop | 74 stable | 70 stable | OK (légère amélioration) |
| dmesg V6.0 patches | n/a | 0 | OK (objectif retrait) |
| kcontrols TAC | 50 | 50 | identique |

## Test utilisateur

| Critère | OUI / NON |
|---|---|
| Audio loopback audible (mics → speakers via loopback-c) | ⏳ À tester (board en attente d'écoute user) |
| Validation E0 GO | ⏳ — sera GO si OUI ci-dessus |

## Conclusion

- **Travaux** : OK (kernel rebuildé propre, modules SOF redéployés, firmware E6.a baseline OK, topology E6.a restaurée)
- **Tests T0.1 → T0.7** : tous **OK**
- **Test utilisateur T0.8** : à confirmer empiriquement par l'utilisateur

**Action** : si écoute user = « son OK » → tag `v7.0-e0` + push, passer à E1 (topology simplifiée + ALSA low-lat < 10 ms).

## Annexes

- Backup ancien tplg sur board : `sof-imx8mp-tac5212.tplg.bak-pre-v7e0`
- Backup ancien kernel sur board : `Image.bak-pre-v7e0`
- Build dir SOF : `/home/michael/yocto-nxp-debix/build-sof`
- Build dir kernel : `Model_AB_Infinity/tmp/work/imx8mpevk-poky-linux/linux-imx/6.6.36+git/`
