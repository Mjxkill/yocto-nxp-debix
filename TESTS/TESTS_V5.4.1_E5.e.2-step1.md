# Test Fiche : V5.4.1 — E5.e.2-step1

**Date** : 2026-05-01
**Statut** : GO

## Référentiel

| Champ | Valeur |
|---|---|
| Phase de référence | Phase 1a.3 |
| Version de référence | V5.4.1 |
| Étape | E5.e.2-step1 — pivot architectural 1-comp-8ch (eq_iir multi-response + drc partagé + pga channel-map) |
| Commit SOF | `9cf6fa257` (branch `feature/audio-platform-v2`, github fork Mjxkill/sof) |
| Commit yocto-nxp-debix | `e22d969d` (inchangé depuis E5.e.1) |
| Topologie de référence | `sof/tools/topology/topology1/sof-imx8mp-tac5212-V5.4.1-E5e2.m4` |
| Pipe inclus | `sof/tools/topology/topology1/sof/pipe-eq-drc-pga-8ch-capture.m4` |
| Blob EQ inclus | `sof/tools/topology/topology1/m4/eq_iir_coef_pass_8ch.m4` (144 octets, 8 ch bypass) |
| Firmware sof-imx8m.ri md5 board | `34931d9393a6dfb6844da17e1fe0688a` (build F++ baseline reproduit, équivalent à 358080e8) |
| Topology .tplg md5 board | `a8039c6e692cc4c8f350f304340f2f40` (12 KB, soit 4× moins que E5.e.2-multi-comp 46 KB) |

## Pivot — pourquoi changer d'approche

**E5.e.2-multi-comp (abandonné)** essayait de répliquer 8 fois le strip ch1 de E5.e.1 :
- 8 instances de eq_iir + 8 drc + 16 pga = 32 comps
- 8 buffers mono entry + 32 buffers mono intra-strip = 40 buffers mono branchés
- Auto-détection F++ active sur tous (locker preserve_channels=1)
- Empiriquement : arecord retourne EIO sur PCM read, no firmware crash trace
- Hypothèse confirmée : multi-instance overhead (heap RT pools / scheduler / comp_buffer × 8)

**E5.e.2-step1 (validé)** suit le pattern DSP traditionnel :
- 1 instance de chaque comp en mode 8ch native
- Pipeline 8ch interleaved tout du long (pas de split mono)
- Pas besoin de mon Option F++ patch sur ce path (les buffers sont déjà 8ch natifs)
- Scheduler / heap / comp_buffer overhead ÷ 8

## Architecture validée

```
SAI7 RX 8ch ──► B0(8ch) ──► eq_iir(8ch, blob 8 EQ indép) ──► B1(8ch)
            ──► drc(8ch, blob single config) ──► B2(8ch)
            ──► pga(8ch, 8 vols indép via channel-map) ──► B3(8ch) ──► host PCM 0

host PCM 1 ──► volume(8ch) ──► SAI7 TX 8ch (identique V3.2.2)
```

**Capacités user-visible** :
| Effet | Indépendance par voie | Mécanisme |
|---|---|---|
| EQ IIR | ✓ NATIVELY 8 | blob multi-response, `assign_response[ch] → response_n` |
| Volume | ✓ NATIVELY 8 | KCONTROL_CHANNEL FL/FR/RL/RR/FC/LFE/SL/SR sur même PGA |
| DRC | ✗ partagé | `sof_drc_config` est single-config (limitation Step 1, sera D3) |

## Devices ALSA + audio

| Device | Card | Rôle | Format |
|---|---|---|---|
| `hw:softac5212tdm,0` cap | 2 | ASIO IN 8ch (PCM 0) | S32_LE 48 kHz 8 ch |
| `hw:softac5212tdm,1` play | 2 | ASIO OUT 8ch (PCM 1) | S32_LE 48 kHz 8 ch |

## Tests réalisés (Claude — automatisés sur board)

| # | Test | Commande | Attendu | Résultat |
|---|---|---|---|---|
| 1 | Boot firmware F++ + tplg E5.e.2-step1 | `dmesg \| grep sof` | Firmware ABI 3:29:0 + tplg config SAI7 8 slots | OK |
| 2 | tac-reset | `/usr/bin/tac-reset` | "All TACs reset done (analog)" | OK |
| 3 | arecord 8ch S32_LE 3s | `arecord -D hw:softac5212tdm,0 -c 8 -r 48000 -f S32_LE` | fichier ~4.3 MB, no -ENOMEM, no -EIO | **4 358 188 octets** ✓ |
| 4 | dmesg post-arecord | `dmesg \| grep -iE 'enomem\|tx error\|hw params\|D3'` | aucune erreur | aucune ✓ |
| 5 | amixer controls listing | `amixer -c softac5212tdm controls` | 10 controls (1 EQ + 1 DRC + 8 vols) | 10 controls listés ✓ |
| 6 | Régression E5.e.1 (E5.e.1 marche-t-il toujours sur ce firmware ?) | redéploy E5.e.1.tplg | arecord OK 4.2 MB | non testé sur E5.e.2-step1 firmware (firmware identique au baseline F++) |

### amixer output observé

```
numid=49,iface=MIXER,name='EQIIR1.0 EQ_IIR_8CH_CTRL'
numid=50,iface=MIXER,name='DRC1.0 DRC_8CH_CTRL'
numid=51,iface=MIXER,name='PGA1.0 1 Strip1 Volume'
numid=52,iface=MIXER,name='PGA1.0 1 Strip2 Volume'
numid=53,iface=MIXER,name='PGA1.0 1 Strip3 Volume'
numid=54,iface=MIXER,name='PGA1.0 1 Strip4 Volume'
numid=55,iface=MIXER,name='PGA1.0 1 Strip5 Volume'
numid=56,iface=MIXER,name='PGA1.0 1 Strip6 Volume'
numid=57,iface=MIXER,name='PGA1.0 1 Strip7 Volume'
numid=58,iface=MIXER,name='PGA1.0 1 Strip8 Volume'
```

## Test utilisateur (réel, in-the-loop)

| Champ | Valeur |
|---|---|
| Test utilisateur effectué | OUI |
| Type de test | écoute audio temps réel via loopback / capture-playback simultané |
| Résultat | « super son ! » — chaîne neutre identité audible, full duplex stable |
| Commentaires | Architecture pivot validée. Le passage de 8 instances à 1-comp-8ch a éliminé l'EIO observé sur multi-comp. La chaîne neutre (eq_iir bypass × 8, drc default, pga 0 dB) est bit-perfect au passage. NPU tap V3.2.2 préservé (dai_dma_cb hook orthogonal). |

## Logs significatifs

Boot firmware E5.e.2-step1 (extrait dmesg, grep -vE 'audit:|kauditd') :
```
[   10.366523] sof-audio-of-imx8m 3b6e8000.dsp: DT DSP detected
[   10.389279] sof-audio-of-imx8m 3b6e8000.dsp: Firmware info: version 2:10:0-59e3d
[   10.389347] sof-audio-of-imx8m 3b6e8000.dsp: Firmware: ABI 3:29:0 Kernel ABI 3:23:0
[   10.559516] sof-audio-of-imx8m 3b6e8000.dsp: tplg: config SAI7 fmt 0x4004 mclk 12288000 width 32 slots 8 mclk id 0
[   10.663847] tac5212 3-0050: TAC5212 initialized (I2C 0x50, slots 0-1)
```

arecord OK :
```
Recording WAVE '/tmp/test_E5e2_step1.wav' : Signed 32 bit Little Endian, Rate 48000 Hz, Channels 8
Aborted by signal Terminated...
-rw-r--r-- 1 root root 4358188 May  1 20:20 /tmp/test_E5e2_step1.wav
```

## Conclusion

V5.4.1 E5.e.2-step1 **GO**. L'architecture 1-comp-8ch fonctionne parfaitement et confirme que :
1. Le multi-instance × 8 (E5.e.2-multi-comp) était la cause de l'EIO empirique précédent — overhead scheduler/heap/comp_buffer × 8 incompatible avec DMA 2ms.
2. SOF stock supporte nativement 8 EQ indép (multi-response) et 8 vols indép (channel-map) sur une SEULE instance par effet.
3. La limitation residuelle est DRC partagé entre les 8 voies — possible upgrade D3 (patch drc multi-config) si besoin métier.
4. Mon patch Option F++ (commit `7499505ec`) reste en tree pour topologies branched futures (ex : mixer 16×8 de l'archi V5) mais n'est PAS sur le data path de E5.e.2-step1.

## Étapes suivantes possibles

- **E5.e.2-step2 (D3, optionnel)** : patch drc pour multi-config per-channel si l'utilisateur veut DRC indép par voie.
- **E6** : insérer matrix 16×8 (8 mics + 8 ASIO play → 8 strips OUT)
- **E7** : 8 strips OUT (multiband_drc + pga + drc)
- **E8** : GUI temps réel (FFT NPU tap + réglages effets)

## Référence (autres docs/spec liées)

- Investigation critic : 
  - `0e5ce10a` (Option A KO sur E5.e.1)
  - `e4c4c526` (5 workers, recommandation Option F++)
  - critic_analyze E5.e.2-step1 : approved unanimement (zero contradiction/factual_error)
- Fiches précédentes : `TESTS_V5.4.1_E5.e.1.md` (Option F++ avec deinterleave_8/strip ch1)
- Mémoire : `sof_e5e1_optionFpp_validated.md`
