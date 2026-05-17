# ARCHI V7.0 — DSP indépendant + NPU dual-tap (asymétrique)

**Date** : 2026-05-10
**Auteur** : Michael (cadrage), assisté Claude
**Statut** : DRAFT — à valider avant tout code
**Baseline** : E6.a, commit SOF `0580b5f14`

## 1. Pourquoi V7.0

V6.0 (passthrough firmware-only DAI-to-DAI sans PCM hôte) **abandonné** après 3 semaines : SOF est conçu host-centric, le `pipe_task` body ne s'exécute jamais sans HOST anchor (mailbox 0x570 = 0xFFFFFFFF empiriquement).

V7.0 **repart de E6.a** et change de cap :
- 2 pipelines DSP **strictement indépendantes** (chacune avec son PCM HOST anchor — modèle SOF natif)
- Routing/mixage **délégué à Linux userspace**
- 2 **NPU taps asymétriques** : un sur l'INPUT brut (cap), un sur l'OUTPUT post-effets (play)

## 2. Vue système end-to-end

```
mics ─┐                                                      ┌─ speakers
      │                                                      │
   SAI7 RX ──→ PIPE 1 cap (DSP) ──→ PCM 0 cap ─┐          ┌─ PCM 1 play ──→ PIPE 2 play (DSP) ──→ SAI7 TX
      │             │                          │          │                       │
      │             ↓                          │          │                       ↓
      │       TAP IN (raw)                     │          │                 TAP OUT (post-FX)
      │             │                          │          │                       │
      │             ↓                          │          │                       ↓
      │     reserved-mem tap-in                │          │              reserved-mem tap-out
      │             │                          │          │                       │
      │             ↓                          │          │                       ↓
      │     /dev/imx-audio-tap-in              │          │              /dev/imx-audio-tap-out
      │             ↓                          │          │                       ↓
      │            NPU                          ↓          ↑                     NPU
      │                                       Linux Mixer / Routing / Send FX
      │                                          ↑          ↓
      │                            USB gadget ───┤          ├─→ USB gadget   (8 × 8)
      │                            Téléphone ────┤          ├─→ Téléphone    (2 × 2)
```

## 3. Pipeline 1 — Capture (8 voies)

```
SAI7 RX 8ch
      ├──→ TAP IN (raw, hook dai_dma_cb) ──→ rmem tap-in ──→ /dev/imx-audio-tap-in
      ↓
multiband_drc (8ch, 8 indép, multi-blob)
      ↓
drc D3 (8ch, 8 indép, state arrays)
      ↓
pga (8ch, 8 vols indép)
      ↓
PCM HOST 0 cap (ASIO IN, 8ch S32_LE 48 kHz)  →  /dev/snd/pcmC2D0c
```

| Étage | Component SOF | Paramètres | Origine |
|---|---|---|---|
| Capture DAI | `DAI SAI7 RX` | TDM 8 × S32_LE × 48 kHz | E6.a |
| **TAP IN (raw)** | hook `dai_dma_cb` cap | rmem `tap-in`, signal mic non traité | V7.0 nouveau |
| Compression multibande | `multiband_drc` 8 ch | blob 8 × N (multi-config patché) | V7.0 nouveau |
| Compression dynamique | `drc` D3 patché | state arrays + multi-blob 8 × M | E5.e.2-D3 |
| Volume | `pga` 8 ch | 8 vol kcontrols indép | E5.e.2 step1 |
| HOST PCM | `PCM 0 cap` | 8 ch S32_LE 48 kHz | E6.a |

## 4. Pipeline 2 — Playback (8 voies)

```
PCM HOST 1 play (ASIO OUT, 8ch S32_LE 48 kHz)  ←  /dev/snd/pcmC2D1p
      ↓
multiband_drc (8ch, 8 indép, multi-blob)
      ↓
pga (8ch, 8 vols indép)
      ↓
drc (8ch, 8 indép, limiteur)
      ├──→ TAP OUT (post-FX, hook pre-DAI TX) ──→ rmem tap-out ──→ /dev/imx-audio-tap-out
      ↓
SAI7 TX 8ch (master, BCLK/FSYNC source)
```

| Étage | Component SOF | Paramètres | Origine |
|---|---|---|---|
| HOST PCM | `PCM 1 play` | 8 ch S32_LE 48 kHz | V7.0 simplifié vs E6.a |
| Compression multibande | `multiband_drc` 8 ch | blob 8 × N′ | V7.0 nouveau |
| Volume | `pga` 8 ch | 8 vol kcontrols indép | réutilisé |
| Limiteur | `drc` 8 ch | blob 8 × M′ | réutilisé |
| **TAP OUT (post-FX)** | hook post-strips, pre-DAI TX | rmem `tap-out`, signal speaker post-traité | V7.0 nouveau |
| Playback DAI | `DAI SAI7 TX` | TDM 8 × S32_LE × 48 kHz · master | E6.a |

**Suppressions vs E6.a** : `mixer16`, `deinterleave_8`, `interleave_8`, tous les buffers mono intermédiaires.

## 5. Effets TAC5212 dans la chaîne (NPU pilote tout)

Le TAC5212 n'est PAS un convertisseur transparent. Tous ses effets internes font partie de la chaîne audio V7.0, et le NPU doit pouvoir agir sur l'**ensemble unifié** (TAC + DSP). Source : datasheet SLASF23A § 7.1, p. 28.

**Chaîne audio complète V7.0** :

```
Capture :
mics → [TAC ADC : AGC → HPF → biquads → gain → decim] → SAI7 RX
     → [DSP cap : multiband_drc → drc D3 → pga] → PCM 0

Playback :
PCM 1 → [DSP play : multiband_drc → pga] → SAI7 TX
      → [TAC DAC : interp → biquads → DRC → gain → limiter+foldback] → speakers
```

> **Note V7.0-E3 final (2026-05-11)** : le `drc` limiteur final côté play a été retiré après diag T3.9. Les coeffs default (héritage musique) provoquaient un pattern « tic à l'attack » incompatible avec la voix temps réel ; la double compression (`multiband_drc` + `drc`) était redondante. Un vrai limiteur calibré voix pourra revenir en **E3.b** (futur sprint).

**Effets TAC5212 ADC (capture)** : AGC · HPF · Biquad filters par canal · Gain/Volume · Phase & gain calibration · Decimation filter (linear-phase / low-lat / ultra-low-lat) · Digital channel mixer · Mic bias programmable · PDM mic decimation (jusqu'à 4 mics PDM)

**Effets TAC5212 DAC (playback)** : Interpolation filter (linear-phase / low-lat / ultra-low-lat) · Biquad filters par canal · DRC · Gain/Volume · Distortion limiter · Thermal foldback · Battery guard (brown-out) · Tone generator · VAD (Voice Activity Detection) · UAD (Ultrasonic Activity Detection)

**Pilotage NPU** : le NPU calcule des consignes/coefficients, le GUI test V7.0 (E7) ou un orchestrateur userspace les applique aux kcontrols ALSA. Driver `tac5212.c` doit exposer ces effets en kcontrols (audit E0).

## 6. Devices Linux mixer

Le mixer Linux est connecté à 3 paires symétriques :

| Device | Channels | Direction | Source / sink | Usage |
|---|---|---|---|---|
| **DSP TAC5212** | 8 × 8 | cap + play | PCM 0 cap / PCM 1 play | Capture mics + restitution speakers locaux |
| **USB gadget audio** | 8 × 8 | cap + play | USB 2.0 audio class — vu comme carte son externe par PC host | I/O studio externe (DAW PC ↔ board) |
| **Téléphone** | 2 × 2 | cap + play | Modem voice / VoIP / PCM dédié | Communication voix entrante/sortante |

Le mixer ne fait **aucun appel au DSP**. Il prend N inputs (DSP cap, USB cap, phone cap), produit M outputs (DSP play, USB play, phone play), routing/gain/send FX au choix de l'app userspace.

## 7. Invariants stricts

| # | Invariant | Mémoire |
|---|---|---|
| I1 | TX = master, RX = slave, ASYNC, BCLK continu (FCONT=1) | `feedback_tx_master_drives_all.md` |
| I2 | DMA 2 ms, `SCHEDULE_TIME_DOMAIN_DMA` exclusivement | `feedback_dma_2ms_definitive.md` |
| I3 | 8 ch = 1 component multi-channel, jamais 8 instances | `feedback_pcm_8ch_pas_separes.md` |
| I4 | Pas de cross-pipeline DSP cap ↔ play | nouveau V7.0 |
| I5 | NPU taps présents en permanence (in + out) | `project_npu_non_negotiable.md` |
| I6 | tac-reset avant tout test capture | `tac_reset_required_after_boot.md` |
| I7 | period=2000 µs, S32_LE, 8 slots TDM, 48 kHz | E6.a baseline |
| I8 | Filename firmware = `sof-imx8m.ri` | `sof_firmware_deploy_path.md` |

## 8. Priorités V7.0

1. **Latence end-to-end < 10 ms** (mic → DSP cap → mixer Linux → DSP play → speaker) — non-négo
2. **GUI de test V7.0** (mixer + réglage de TOUS les effets DSP/TAC en direct) — livrable
3. **2 NPU taps** (in raw + out post-FX) — plomberie livrable, NPU consommateur en aval
4. **Ardour n'est PAS un objectif** — pas le DAW cible, pas dans l'image, pas optimisé pour

## 9. Roadmap

| Étape | Objectif | Critère GO |
|---|---|---|
| **E0** | Branche `feature/v7.0-multiband-drc-tap` créée depuis `0580b5f14`, doc V7.0 commit/push, **retrait `apply-v6-always-on.py`**, fiche E0 baseline E6.a re-vérifiée | boot OK, audio loopback OK, kernel sans patches V6.0 |
| **E1** | Topology simplifiée + ALSA low-lat Linux : retirer mixer16/deinterleave/interleave, PCM 1 → SAI7 TX direct, MMAP + SCHED_FIFO + mlockall | latence boucle ALSA mesurée < 10 ms, 0 xrun sur 60 s |
| **E2** | Pipe cap : remplacer `eq_iir` par `multiband_drc` (8 ch multi-blob, patch state arrays) | 8 multibandes indép, latence E1 préservée |
| **E3** | Pipe play : strips OUT `multiband_drc → pga` (8 ch indép) — drc final retiré après diag tic tic | 8 voies play indép, audio OK, latence préservée |
| **E4 ✓** | **Tap IN brut** (PIPE 1) : `apply-npu-tap-dt.py` refactoré 2 carves + 2 nodes, module multi-instance via prop DT `device-name`, hook firmware `dai_dma_cb` capture, `/dev/imx-audio-tap-in` + `/dev/imx-audio-tap-out` exposés | **GO 2026-05-11** — tap-in 1.22 MB/s, tap-out V3.2.2 non régressé, loopback E3 préservé |
| **E5 ✓** | **Tap OUT post-FX** (PIPE 2) : plomberie livrée en E4 (refactor dual-tap), validation empirique en E5 | **GO 2026-05-11** — modulation PGA Strip1 -40 dB visible exactement sur tap-out, tap-in/-out simultanés OK |
| **E6.a ✓** | USB gadget UAC2 8×8 isolé (configfs + systemd) | **GO 2026-05-11** — bidir validé : PC→board sine 880 -4.4 dBFS, board→PC sine 440 -6 dBFS sur 8 voies, format S32_LE 48kHz négocié high-speed, 0 régression DSP |
| **E6.b ✓** | Téléphone 2×2 (snd-aloop simulé, faute de modem hardware) | **GO 2026-05-11** — card 10 `Phone`, sine 1 kHz aloop bidir préservé -6 dBFS |
| **E6.c ✓** | Routing ALSA paire-à-paire (alsa-route-bridge déclaratif via alsaloop) | **GO 2026-05-11** — route E2E DSP cap → UAC2 → PC `arecord` validée (4.6 MB/3 s, débit nominal). PipeWire dispo en réserve |
| **E6.d ✓ MVP** | Mixer console DAW SW (`mixer-pro` daemon C) — 26 in × 18 out, 4 bus FX stéréo + sends, socket JSON `/run/mixer-pro.sock` | **GO MVP 2026-05-11** — matrice précise (voies non routées = -inf dB), 48 kHz steady, **latence ALSA interne 8 ms** (mesurée `snd_pcm_delay`) |
| **E6.e ✓ MVP** | 4 effets natifs C sur les bus FX : compressor, reverb Schroeder, delay, EQ 3-band biquad. `set_fx_param` via socket JSON | **GO MVP 2026-05-11** — vtable `fx_engine_t` (prêt LV2 futur), 8 params modifiés en live OK, routage produit signal (-92 dB vs -inf sans). Validation perceptuelle non mesurée |
| **E6.f ✓ Diagnostic** | Profiling concret via `clock_gettime` exposé dans `get_state` JSON (`prof_cap_us`, `prof_mix_us`, `prof_play_us`, `prof_iter_us`) | **Diagnostic 2026-05-11 — Effets PAS le hotspot** : mix 4 effets actifs = **473 µs** (24 % budget) ; le coupable = `snd_pcm_readi/writei` DSP (512 ms par cycle = recover SOF IPC). → E6.g = retrait `snd_pcm_link` + 3-thread + lockfree ring buffers (kernel a déjà `CONFIG_PREEMPT=y`, PREEMPT_RT non critique) |
| **E6.g ✓** | Refactor mixer-pro : retrait `snd_pcm_link` + 2 threads (audio cap+mix / play DSP) + ring SPSC 32 périodes | **GO 2026-05-11** — **48 288 Hz steady** (100.6 % nominal, vs 36 % avant) avec 4 bus FX actifs, **0 xrun steady**, latence ALSA 8 ms. Latence ring +58 ms (trade-off design, optim possible E6.h). Total E2E mixer ~66 ms |
| **E6.h ✓** | Optim latence : `nanosleep` → eventfd + ring 32 → 8 périodes | **GO 2026-05-11** — **Latence E2E mixer 14 ms** (vs 66 ms E6.g), 48 224 Hz steady, 0 xrun. **E2E pipeline complet ~20 ms** (vs 72 ms). `ring_fill` 288-384 stable, `ring_drops` bornés à init |
| **E6.i ✗** | Drainage agressif play_thread BLOCKING + N_PERIODS=2 (MAX_DRAIN_PERIODS=4) | **KO 2026-05-11 reverted** (`e32d184c` → `493d8217`) — empirique board : `ring_fill steady 768` (max, vs cible 0-96), latence côté play **20 ms** vs 12 ms E6.h. Cause : `snd_pcm_writei` blocking est self-limiting à 48 kHz → drain ne consomme jamais plus vite que la production → ring se remplit au max. Fiche `TESTS_V7.0_E6i.md` |
| **E6.j ✗** | Drainage NONBLOCK `snd_pcm_writei` + `snd_pcm_avail_update` (path workers deepseek/glm) | **KO 2026-05-11 non commité** — empirique board : Δ ring_drops ~50 000 frames/s continu, Δ xrun ~7-11/s steady, ring_fill 672 + spikes 3456. Cause : `snd_pcm_writei` NONBLOCK retourne quasi-systématiquement `-EAGAIN` sur SOF i.MX8MP → drain ne s'exécute pas, audio_thread drop massif. Warning anticipé par les workers (comportement non vérifié sur SOF). Fiche `TESTS_V7.0_E6j.md` |
| **E6.k ✗** | N_PERIODS=2 SEUL (logic play_thread blocking E6.h conservée 1:1) | **KO 2026-05-11 non commité** — empirique board : ring saturé MAX 768 (+14 ms vs E6.h), Δ ring_drops 19/s continu en steady, latence côté play **18-20 ms** vs 12-14 ms E6.h. Cause : réduire ALSA buffer (8 ms → 4 ms) transfère le tampon vers le ring SPSC car le système est self-régulé à 48 kHz. Gain ALSA -2 ms < régression ring +8 ms. Fiche `TESTS_V7.0_E6k.md` |
| **E6.l ✗** | Single-thread (suppression ring SPSC + play_thread + eventfd) + N_PERIODS=2 | **KO 2026-05-11 non commité** — empirique board : throughput effondré à **1 546 Hz (3.2 % nominal)**, `prof_iter_us` cyclique **122 ms** toutes les 5 s, Δ xrun **16/s**. Cause : recover SOF 60 ms en single-thread bloque cap+play **en cascade** (`prof_cap_us=60ms` ET `prof_play_us=60ms`). Le ring SPSC d'E6.h absorbait ces recover en isolant les threads — supprimé, cercle vicieux. Convergence avec théorème de Little (worker claude-code) : plancher physique architectural ~10 ms = cible inatteignable. Fiche `TESTS_V7.0_E6l.md` |
| **E7** ✓ | **GUI HTTP V7.0** : daemon C `mixer-gui-http` (libmicrohttpd) + frontend Alpine.js + Tailwind via CDN. Backend REST `/health`, `/api/state`, `/api/cmd`, statique `/`. Connect-per-request vers `/run/mixer-pro.sock`. CORS *. | **GO 2026-05-11** — commit `079d3ddb`. Tests board : POST set_send/set_master/set_fx_bus/reset_fx/set_mute tous OK. mute_mask state propagation confirmée |
| **E7.1** ✓ | **Peak meters + SSE streaming temps réel** : `peak_in[26]/peak_out[18]/peak_fx[8]` atomic post-mix mixer-pro + op `get_meters` JSON compact. Endpoint `/api/stream` SSE 30 Hz (chunked callback RFC 8895). Frontend EventSource binding dynamique meter-fill height (échelle -60..0 dBFS) | **GO 2026-05-11** — commit `d055b946`. Mesure board : prof_iter_us 1797 µs (critic gate < 1980), xrun=0, vumètres live 30 Hz validés user. Brique technique temps réel pour E7.5 (FFT) |
| **E7.2 ✓** | **UI premium "Apple-class"** : layout groupé, faders verticaux DAW-style, dark glassmorphism | **GO** — livré sur la branche `feature/v7.0-multiband-drc-tap`, intégré dans les commits E7.3a→E7.4 |
| **E7.3 ✓** | **Panels effets DSP complets** : pour chaque bus FX (compressor/reverb/delay/EQ), tous params exposés en sliders + modal | **GO** — commit `61849c36` (E7.4.b inclut le modal FX bus), tous params modifiables live |
| **E7.4 ✓** | **Effets TAC5212 + DSP DRC editor + crossover + layout grid + sidebar tabs** : 24 biquads × 4 TACs + 6 paged-blob effets par TAC, inline DRC/MULTIBAND_DRC editor per-channel, crossover LR-style éditable Hz, layout 2-col grid, effect tabs sidebar, tac-reset persistance systemd | **GO 2026-05-12** — bundle `45e6592b`, 339 kcontrols ALSA, 4 TACs bindés (fsl,imx-audio-card), DRC + crossover validés son OK utilisateur. Fiche `TESTS_V7.0_E7.4.md` |
| **E7.5 ✓** | **4 analyzer taps FFT + scope stéréo** : daemon 4× ring lock-free + analyzer_thread RT prio 60 + FFT radix-2 1024 inline + 128 bins dB + 64 paires scope ; GUI 4 boîtes 410×~410 px sous outputs, sélecteur source (mono ou stéréo selon stereo-link) | **GO 2026-05-12** — commits `4f4e4d0b` + `4124f97f`. prof_iter_us 1708→1959 µs (+15%), latence 8 ms inchangée, xrun 12/7h58 (0.0004/s). Fiche `TESTS_V7.0_E7.5.md` |
| **E7.6 ✓** | **Documentation consolidée** : fiches `TESTS_V7.0_E7.4.md` (E7.4 initial → E7.4.n consolidé) + `TESTS_V7.0_E7.5.md`, MAJ roadmap §9 ARCHI_V7.0 avec verdicts + commits | **GO 2026-05-13** |
| **V8.0 → V8.32** | **Sprint UAC2 8×8 + ASRC + diagnostic ring** : isolation UAC2 cap/play par threads dédiés, drift mesure passive, ASRC drop/insert ±N samples piloté par fill ring, push atomique 96 frames, pre-fill 192, stats wr/rd timing min/avg10s/max dans GUI | **GO partiel 2026-05-16** — itérations V8.1 (threads) → V8.32 (timing stats + min/max persistants). Drift mesuré +554 ppm sur board (irréaliste, problème mesure). Plusieurs régressions ASRC reverted. cap_empty restait massif sans isolcpus. |
| **V8.33 ✓** | **RT baseline : isolcpus=2,3 + CPUAffinity + self-paced audio_thread** : `boot.scr` U-Boot ajoute `isolcpus=2,3 nohz_full=2,3 rcu_nocbs=2,3`. mixer-pro.service `CPUAffinity=2 3`, mixer-gui-http `0 1`. audio_thread `clock_nanosleep ABSTIME` 2 ms exact pour throttle 500 Hz exact (fix busy-loop sans tick scheduler) | **GO 2026-05-17** — chaîne MIC→DSP→USB→DAW→USB→DSP→HP validée par utilisateur. Son acceptable, latence limite mais réactif, glitchs résiduels rares. **NON acceptable pro mais OK étape intermédiaire**. cap_empty 0.5/s. wr/rd max 3-6 ms (vs 4.5 ms avant). Fiche `TESTS_V8.33_RT_isolcpus.md` |
| **B0/B2 ✗** | **IRQ affinity DWC3 testé empiriquement 2026-05-17** : hypothèse 28M IRQs core 0 = bottleneck. Mesures live avec DAW actif : mask 0xc (cores 2,3) **EMPIRE** (wr/rd max 2700→5054 / 2946→7419, cap_full 0→15745, drift -13→-367 ppm) — cache contention massive avec audio_thread. Mask 0xf (default core 0) reste optimal. | **REJETÉ empiriquement** — théorie fausse. Baseline 0xf donne wr/rd max ~3ms steady avec DAW. Pics 5959 µs fiche V8.33 = transitoires, pas du scheduling. B1 PREEMPT_RT également skippé (gain ~100µs sur ~5ms pic = effort disproportionné). |
| **V8.34 ✓** | **PLC (Packet Loss Concealment) côté cap ring** : remplace `memset zéros` par répétition de la dernière period valide avec fade-out progressif (ramp gain Q8 `{256,256,256, 192,128,64,32,16, 0}` = 3 reps full → 5 reps fade → silence à 16 ms). Globals statiques `g_plc_last_period`, compteur `g_plc_events` exposé dans GUI topbar widget `PLC <N>`. Passif : zero overhead tant que pas de starvation. | **DÉPLOYÉ 2026-05-17** — chaîne MIC→DSP→USB→DAW→USB→DSP→HP : pas de régression empirique (wr 3316 vs 2700, rd 2937 vs 2946, xruns 0, drift -0.9 vs -13 ppm). `plc_events=0` sur 30s steady (PLC pas déclenchée). Validation utilisateur condition réelle EN COURS. Fiche `TESTS_V8.34_PLC.md` |
| **V8.x suite — options lourdes** | Si V8.34 PLC insuffisante en pratique : PREEMPT_RT kernel (gros chantier, gain incertain), ALSA loopback virtual device pour court-circuiter UAC2 gadget, ou architecture mixer hardware externe. | **À évaluer après retour utilisateur sur V8.34** |

### Bilan final optim latence userspace V7.0 (E6.i/j/k/l)

**4 itérations empiriques d'optim latence userspace ont échoué** :

| Sprint | Approche | Verdict | Latence mesurée |
|---|---|---|---|
| E6.h ✓ | eventfd + ring 8 périodes | baseline | 14 ms côté mixer |
| E6.i ✗ | drainage blocking + MAX_DRAIN=4 | reverted | ring saturé 16 ms |
| E6.j ✗ | drainage NONBLOCK + avail_update | non commité | EAGAIN systématique, drops 50k/s |
| E6.k ✗ | N_PERIODS=2 seul | non commité | ring saturé 16 ms, +6 ms régression |
| E6.l ✗ | single-thread + N_PERIODS=2 | non commité | recover cascade 122 ms, throughput 3.2 % |

**Conclusion architecturale** (théorème de Little, claude-code worker) :

> Plancher physique V7.0 = TAC ADC 0.5 + DMA RX 2 + cap pipe 0.5 + ALSA cap min 2
> + mix 0.5 + ALSA play min 2 + DMA TX 2 + DAC 0.5 = **10 ms exact AVANT marge anti-xrun**.

La cible < 10 ms acoustique V7.0 est **mathématiquement inatteignable** sans
refonte. E6.h (14 ms côté mixer, ~20 ms E2E) est la **limite pratique** sur
ce SOF i.MX8MP en userspace.

**Décision** :
- E6.h = **baseline V7.0 finale**, livrable stable
- Cible < 10 ms reportée à **V8.0 mixer DSP SOF natif** (piste 2 convergence 4/5 workers, effort 4-8 semaines, gain structurel -8 à -14 ms en éliminant ring + 1 ALSA buffer + 1 DMA traversée)

Investigation critic E6.l (job `f9547d24`, 6 workers, 688 s) documente cette
conclusion avec analyse exhaustive des 17+ pistes alternatives.

### Bilan optim latence userspace (E6.i/j/k)

Trois itérations ont échoué empiriquement à descendre sous E6.h (14 ms). Conclusion :
le système producteur-consommateur `audio_thread / play_thread` est self-régulé à
48 kHz par le hardware DMA SOF. **Le tampon total (ring + ALSA) est conservatif** :
on ne peut pas le réduire en touchant un seul paramètre côté userspace sans
transférer la latence ailleurs ou casser la stabilité.

Pour vraiment passer sous 10 ms acoustique, il faut envisager une voie architecturale
(mixer DSP SOF natif, ou patch kernel SOF) — sprint E6.l à investiguer ou hors scope V7.0.

## 10. Patches kernel — audit V6.0 → V7.0

| Patch / fichier | V7.0 | Action |
|---|---|---|
| `0001-imx8mp-evk-audio-mipi.patch` | Garder | Board hardware |
| `spdif.cfg`, `disable-at24.cfg` | Garder | Hors scope audio principal |
| SOF imx-probes (`imx-probes.c`, `apply-imx-probes.py`, `sof-imx-probes.cfg`) | Garder | Debug auxiliaire |
| TAC5212 (`tac5212.c/h`, `apply-tac5212-dt.py`, `tac5212.cfg`) | Obligatoire | Sans ça, pas d'audio |
| `apply-npu-tap-dt.py` (2 reserved-mem V7.0-E4) | **OK V7.0-E4** | `tap_in_buffer@94270000` + `tap_out_buffer@942B0000`, 2 nodes `imx_audio_tap_in/out` (prop `device-name`) |
| `apply-sdram2-dt.py` (8 MB DSP-only) | Évaluer | mixer16 retiré mais multiband_drc/drc blobs peuvent réclamer la mémoire ; confirmer E2/E3 |
| **`apply-v6-always-on.py`** (K0+K1+K2+K4+K5) | **RETIRER (E0)** | V6.0 only ; V7.0 a PCM HOST anchors → start standard ALSA |

## 11. Diag mailbox (`/sys/kernel/debug/sof/debug`)

| Adresse | Compteur | Sens |
|---|---|---|
| `0x500` | marker `0xCAFE0420` | sdma_set_config block ran |
| `0x504` | sdma_set_config calls | nb total per boot |
| `0x510-0x52C` | sdma_chan_type[0..7] | type DMA |
| `0x530-0x54C` | hw_event[0..7] | 12=SAI7 RX, 13=SAI7 TX |
| `0x550-0x56C` | direction[0..7] | sens DMA |
| `0x570 / 0x574` | pipe_task body counter | > 0 sur les 2 pipelines |
| `0x600-0x68C` | sdma_copy + dai_dma_cb | trafic DMA effectif |
| E4/E5 | nouveaux compteurs taps | adresses dédiées, jamais réutilisées |

## 12. Risques

| Risque | Probabilité | Mitigation |
|---|---|---|
| `multiband_drc` SOF stock pas multi-instance | Haute | Patch state arrays + multi-blob (modèle drc D3) |
| 2 NPU taps trop de bande passante DDR | Moyenne | Reserved-mem séparées, mesure SDMA bandwidth E5 |
| ALSA low-latency casse Ardour | Faible | Sprint dédié post-E6, isolation mlockall via systemd |
| `drc` cap + `drc` play simultanés — conflit ABI | Moyenne | Patch déjà multi-instance, re-tester sur 2 pipes |
| Tap brut + tap fx désynchronisés (jitter NPU) | Faible | Timestamps SOF dans rmem header |

## 13. Hors-scope V7.0

- **Send FX userspace avancés** (réverb, delay, sidechain inter-voies) — le GUI test V7.0 (E7) couvre seulement mixer + paramètres d'effets DSP/TAC
- E8 GUI v2 (FFT NPU + visualisations ML) — évolution post-V7.0 du GUI E7
- Code NPU (modèles TFLite/Vela)
- Ardour (jamais cible)
- V6.0 archive (branche `feature/v6-always-on-async` conservée)

## 14. Plan de développement complet

Le plan détaillé (8 étapes E0→E7, tableaux travaux + tests + critère GO + contenu type des fiches `TESTS_V7.0_E<n>.md`) est rendu en section 12 du PDF `ARCHI_V7.0.pdf`. Synthèse par étape :

| Étape | Tag git | Livrable | Fiche | Préalable |
|---|---|---|---|---|
| E0 | `v7.0-e0` | Branche + audit kernel + audit driver TAC + baseline E6.a | `TESTS_V7.0_E0.md` | doc V7.0 mergée |
| E1 | `v7.0-e1` | Topology simplifiée + ALSA low-lat < 10 ms | `TESTS_V7.0_E1.md` | E0 GO |
| E2 | `v7.0-e2` | multiband_drc CAP 8 ch indép | `TESTS_V7.0_E2.md` | E1 GO |
| E3 | `v7.0-e3` | Strips OUT play 8 ch | `TESTS_V7.0_E3.md` | E2 GO |
| E4 | `v7.0-e4` | Tap IN brut + /dev/imx-audio-tap-in | `TESTS_V7.0_E4.md` | E3 GO |
| E5 | `v7.0-e5` | Tap OUT post-FX + /dev/imx-audio-tap-out | `TESTS_V7.0_E5.md` | E4 GO |
| E6 | `v7.0-e6` | USB gadget 8×8 + téléphone 2×2 | `TESTS_V7.0_E6.md` | E5 GO |
| E7 | `v7.0-e7` | GUI test V7.0 (mixer + tous effets) | `TESTS_V7.0_E7.md` | E6 GO |

**Règle d'or** : aucune étape ne passe à la suivante tant que **tous les tests Tn.X = OK** ET **« Test utilisateur OUI »**. NO-GO = retour analyse + investigation critic, pas de saut.

**Contenu type des fiches** : en-tête (date / statut / commit / md5), référentiel, travaux exécutés, build & deploy, tests automatisés (Tn.X), logs & mailbox, mesure latence, test utilisateur OUI/NON, régression vs étape précédente, conclusion GO/NO-GO, annexes.

## 15. Décision de branchage

| Repo | Source | Nouvelle branche V7.0 |
|---|---|---|
| `sof/` | `feature/audio-platform-v2` @ `0580b5f14` | `feature/v7.0-multiband-drc-tap` |
| `yocto-nxp-debix/` | `feature/audio-platform-v2` (HEAD courant) | `feature/v7.0-multiband-drc-tap` |
| `meta-local/` | idem yocto | idem yocto |
