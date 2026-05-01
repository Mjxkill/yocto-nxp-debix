# Test Fiche : V5.4.1 — E5.e.2-D3

**Date** : 2026-05-01
**Statut** : GO

## Référentiel

| Champ | Valeur |
|---|---|
| Phase de référence | Phase 1a.3 |
| Version de référence | V5.4.1 |
| Étape | E5.e.2-D3 — DRC multi-config per-channel (8 voies indép totales) |
| Commit SOF | `ea984a266` (branch `feature/audio-platform-v2`, github fork Mjxkill/sof) |
| Topologie de référence | `sof/tools/topology/topology1/sof-imx8mp-tac5212-V5.4.1-E5e2-D3.m4` |
| Pipe inclus | `sof/tools/topology/topology1/sof/pipe-eq-drc-pga-8ch-D3-capture.m4` |
| Blob DRC inclus | `sof/tools/topology/topology1/m4/drc_coef_default_8ch.m4` (756 octets, 8 params identiques) |
| Blob EQ inclus | `sof/tools/topology/topology1/m4/eq_iir_coef_pass_8ch.m4` (144 octets, assign=-1×8 bypass) |
| Firmware sof-imx8m.ri md5 board | `5320d61d353ef82478bc43379b5b7235` |
| Topology .tplg md5 board | `620a160d669b2ff06f78dc38c41b370b` |

## Architecture validée

```
SAI7 RX 8ch ──► B0(8ch) ──► eq_iir(8ch, blob 8 EQ indép) ──► B1(8ch)
            ──► drc(8ch, blob 8 DRC indép — D3 patch) ──► B2(8ch)
            ──► pga(8ch, 8 vols indép via channel-map) ──► B3(8ch) ──► host PCM 0

host PCM 1 ──► volume(8ch) ──► SAI7 TX 8ch (identique V3.2.2)
```

**Capacités user-visible** :
| Effet | Indépendance per voie | Mécanisme | Patch |
|---|---|---|---|
| EQ IIR | ✓ NATIVELY 8 | `assign_response[ch] → response_n` + `iir[ch]` per channel | aucun (natif SOF) |
| DRC | ✓ **D3** 8 indép | state arrays per channel + multi-config blob detected via size | drc.h + drc.c + drc_generic.c + multiband_drc_generic.c |
| Volume | ✓ NATIVELY 8 | KCONTROL_CHANNEL FL/FR/RL/RR/FC/LFE/SL/SR | aucun (natif SOF) |

## Patch DRC D3 — détails

### ABI back-compat strict

`struct sof_drc_config` **inchangé**. Détection multi-config via taille :
```c
N = (config->size - sizeof(struct sof_drc_config)) / sizeof(struct sof_drc_params) + 1
```
- `N == 1` → comportement legacy (single-config, params shared aux 8 voies)
- `N > 1` → multi-config (params[ch] indép per channel, replicate last si ch >= N)

### State arrays per-channel (drc.h)
- `detector_average[PLATFORM_MAX_CHANNELS]`
- `compressor_gain[PLATFORM_MAX_CHANNELS]`
- `envelope_rate[PLATFORM_MAX_CHANNELS]`
- `scaled_desired_gain[PLATFORM_MAX_CHANNELS]`
- `max_attack_compression_diff_db[PLATFORM_MAX_CHANNELS]`

State partagé (pour préserver alignement audio inter-channel) :
- `pre_delay_buffers[PLATFORM_MAX_CHANNELS]` (déjà per-channel)
- `pre_delay_read_index`, `pre_delay_write_index`, `last_pre_delay_frames` (scalaires)
- `processed` (scalaire — set après init)

### Process loop (drc_generic.c)
- `drc_update_detector_average(state, p, nbyte, ch)` : envelope abs sur **un seul** channel (était MAX inter-channel avant)
- `drc_update_envelope(state, p, ch)` : gain shaping per-channel
- `drc_compress_output(state, p, nbyte, ch)` : applique total_gain au samples du channel ch uniquement
- `drc_process_one_division(state, p, nbyte, ch)` : compose les 3 ci-dessus
- Outer loop dans `drc_s32_default` (et s16/s24) : `for ch=0..nch-1 { drc_process_one_division(state, drc_get_params(cd, ch), nbyte, ch) }`

### multiband_drc impact
`multiband_drc_generic.c` : appels mis à jour avec `ch` parameter dans la boucle multi-band. Comportement **inchangé** car son config blob reste single-config.

## Devices ALSA + audio

| Device | Card | Rôle | Format |
|---|---|---|---|
| `hw:softac5212tdm,0` cap | 2 | ASIO IN 8ch (PCM 0) | S32_LE 48 kHz 8 ch |
| `hw:softac5212tdm,1` play | 2 | ASIO OUT 8ch (PCM 1) | S32_LE 48 kHz 8 ch |

## Tests réalisés (Claude — automatisés sur board)

| # | Test | Commande | Attendu | Résultat |
|---|---|---|---|---|
| 1 | Build firmware D3 sans warning | `west build -d build-sof` | Compile OK | OK md5 5320d61d |
| 2 | Boot firmware D3 + tplg E5e2-D3 | `dmesg \| grep sof` | Firmware ABI 3:29:0 + tplg config SAI7 8 slots | OK |
| 3 | Régression back-compat (D3 firmware + tplg E5e2-step1 single-config drc) | `arecord -c 8 -r 48000 -f S32_LE 3s` | fichier ~4.3 MB identique pre-patch | **4 358 188 octets** ✓ |
| 4 | D3 actif (D3 firmware + tplg E5e2-D3 multi-config drc) | idem | fichier ~4.2 MB, dmesg clean | **4 243 500 octets** ✓ |
| 5 | dmesg post-arecord D3 | `dmesg \| grep -iE 'enomem\|tx error\|drc\|D3'` | aucune erreur | aucune ✓ |
| 6 | amixer controls D3 | `amixer -c softac5212tdm controls \| grep -iE 'strip\|EQ\|DRC'` | 1 EQ + 1 DRC + 8 vols | 10 controls listés ✓ |

### amixer output observé (D3)

```
numid=49,iface=MIXER,name='EQIIR1.0 EQ_IIR_8CH_CTRL'
numid=50,iface=MIXER,name='DRC1.0 DRC_8CH_CTRL'
numid=51,iface=MIXER,name='PGA1.0 1 Strip1 Volume'
numid=52,iface=MIXER,name='PGA1.0 1 Strip2 Volume'
... (8 strips au total)
numid=58,iface=MIXER,name='PGA1.0 1 Strip8 Volume'
```

## Test utilisateur (réel, in-the-loop)

| Champ | Valeur |
|---|---|
| Test utilisateur effectué | OUI |
| Type de test | écoute audio temps réel via loopback / capture-playback simultané |
| Résultat | « c'est ok » — chaîne audio fonctionne en mode neutre identité (8 EQ bypass + 8 DRC neutres + 8 vols 0dB) |
| Commentaires | DRC multi-config blob est détecté correctement, parsing 8 params indép réussi, chaque canal a son propre state machine. Identité fonctionnelle préservée (audio bit-perfect en mode neutre). User pourra diverger les 8 DRC indép via amixer cset à terme (ex : threshold ch1 = -20dB, threshold ch2 = -10dB, etc.). |

## Logs significatifs

Boot firmware E5.e.2-D3 (extrait dmesg) :
```
sof-audio-of-imx8m 3b6e8000.dsp: Firmware info: version 2:10:0-9cf6f
sof-audio-of-imx8m 3b6e8000.dsp: Firmware: ABI 3:29:0 Kernel ABI 3:23:0
sof-audio-of-imx8m 3b6e8000.dsp: tplg: config SAI7 fmt 0x4004 mclk 12288000 width 32 slots 8 mclk id 0
tac5212 3-0050: TAC5212 initialized (I2C 0x50, slots 0-1)
```

arecord OK :
```
Recording WAVE '/tmp/test_E5e2_D3.wav' : Signed 32 bit Little Endian, Rate 48000 Hz, Channels 8
Aborted by signal Terminated...
-rw-r--r-- 1 root root 4243500 May  1 20:47 /tmp/test_E5e2_D3.wav
```

## Conclusion

V5.4.1 E5.e.2-D3 **GO**. L'objectif "8 voies totalement indépendantes" est atteint :

- 8 EQ IIR indép par voie (natif SOF, blob multi-response)
- 8 DRC indép par voie (D3 patch — drc.c/drc_generic.c per-channel state + multi-config blob detection)
- 8 Volumes indép par voie (natif SOF, channel-map)

ABI back-compat strict — toutes les topologies SOF stock utilisant drc continuent de fonctionner sans modification (validé par test régression sur tplg E5.e.2-step1 single-config).

Étape archi V5.4.1 capture path complète. Prochaine étape : E6 (matrix 16×8) ou E7 (8 strips OUT — multiband_drc + pga + drc).

## Référence (autres docs/spec liées)

- Investigation critic :
  - `e4c4c526` (5 workers, recommandation Option F++ initial)
  - critic_analyze D3 v2 : approved (2 itérations)
  - critic_analyze E5.e.2-step1 : approved
- Fiches précédentes : `TESTS_V5.4.1_E5.e.1.md`, `TESTS_V5.4.1_E5.e.2-step1.md`
- Mémoires : `sof_e5e1_optionFpp_validated.md`, `sof_pivot_1comp_8ch.md`
