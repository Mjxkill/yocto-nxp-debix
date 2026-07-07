# Test Fiche : V7.0 — E6.e (Effets natifs sur les 4 bus FX du mixer-pro)

**Date** : 2026-05-11
**Statut** : **GO MVP** — infrastructure effets livrée et fonctionnelle (set/get via socket OK). Coût CPU élevé (sample-par-sample, pas de vectorisation) : optimisation = E6.f
**Tag git associé** : `v7.0-e6e` (posé après commit)

## Référentiel

| Champ | Valeur |
|---|---|
| Phase | V7.0 — E6.e Effets natifs C sur les 4 bus FX (compressor, reverb, delay, EQ 3-band) |
| Préalable | E6.d MVP (mixer-pro daemon) GO |
| Pas de modif SOF/kernel/DT/firmware | userspace pur, ajout `effects.c`/`effects.h` |
| Branche yocto | `feature/v7.0-multiband-drc-tap` |

## Travaux exécutés

| Domaine | Fichier |
|---|---|
| Header | `effects.h` (vtable `fx_engine_t` : process, set_param, reset, get_state — prêt pour swap LV2 futur) |
| Effets natifs | `effects.c` (~440 LOC) : 4 implémentations C natives, sample-par-sample stéréo, paramètres avec CLAMP |
| Daemon mixer | `mixer-pro.c` : intégration `fx_engines[N_BUS_FX]`, `mix_frame()` appelle `fx->process()` par bus |
| Protocole socket | Ajout 3 commandes : `set_fx_param`, `get_fx`, `reset_fx` |
| CLI client | `mixerctl`: ajout `fxp <bus> <param> <value>`, `getfx <bus>`, `resetfx <bus>` |
| Recipe Yocto | `mixer-pro_1.0.bb`: SRC_URI étendu avec `effects.{c,h}`, DESCRIPTION mise à jour |
| Makefile | Compile `mixer-pro.c + effects.c` ensemble |

## 4 effets implémentés (default presets)

### Bus FX1 — Compressor
- Algo : peak envelope follower (attack/release exponentiels) + gain reduction soft-knee
- Paramètres :
  - `threshold` : -60..0 dB (default -20)
  - `ratio` : 1..20 (default 4)
  - `attack` : 0.1..500 ms (default 5)
  - `release` : 1..2000 ms (default 50)
  - `makeup` : -12..24 dB (default 0)
- État live exposé via `get_fx` : `env_l_db`, `env_r_db` (utile pour gain reduction meter dans le GUI E7)

### Bus FX2 — Reverb Schroeder
- Algo : 4 comb filters parallèles (~30 ms chacun avec damping LPF) + 2 allpass filters série (~2-5 ms)
- Paramètres :
  - `room_size` : 0..1 (default 0.5) → feedback comb 0.28..0.98
  - `damping` : 0..1 (default 0.5) → cutoff LPF dans la boucle comb
  - `wet` : 0..1 (default 0.5) → output level (signal pur wet, dry à part dans la matrice master)

### Bus FX3 — Delay stéréo
- Algo : ligne à retard ring buffer + feedback
- Paramètres :
  - `delay_ms` : 1..1000 ms (default 250)
  - `feedback` : 0..0.95 (default 0.4)
  - `wet` : 0..1 (default 0.5)

### Bus FX4 — EQ 3-band biquad (RBJ cookbook)
- Algo : low shelf (fixe 250 Hz) + peaking (fréquence/Q ajustables) + high shelf (fixe 5000 Hz) en cascade par canal
- Paramètres :
  - `low_gain` : -18..18 dB (default 0)
  - `mid_gain` : -18..18 dB (default 0)
  - `mid_freq` : 200..8000 Hz (default 1000)
  - `mid_q` : 0.1..10 (default 1.0)
  - `high_gain` : -18..18 dB (default 0)

## Protocole socket étendu

```
{ "op":"set_fx_param", "bus":<0..3>, "param":"<name>", "value":<float> }
{ "op":"get_fx",       "bus":<0..3> }                  → JSON état + meters live
{ "op":"reset_fx",     "bus":<0..3> }                  → clear buffers (sans toucher params)
```

CLI client :
```bash
mixerctl fxp 0 threshold -40         # set compressor threshold
mixerctl fxp 1 room_size 0.9         # set reverb room size
mixerctl fxp 2 delay_ms 500          # set delay
mixerctl fxp 3 high_gain 12          # set EQ high shelf
mixerctl getfx 0                      # dump compressor state + env meter
mixerctl resetfx 1                    # clear reverb buffers
```

## Tests T6e.X

| Test | Description | Résultat | Critère GO |
|---|---|---|---|
| **T6e.1** | Build OK avec effects.c (alsa-lib + pthread + math + rt) | ✓ OK (warning truncation snprintf bénin) | warnings clean / OK |
| **T6e.2** | Daemon démarre, 4 FX engines initialisés avec presets default | ✓ OK (log "FX engines : 0=compressor 1=reverb 2=delay 3=eq") | engines OK |
| **T6e.3** | `getfx N` retourne l'état JSON avec params + meters live | ✓ OK (4 buses, env_l_db/env_r_db visibles sur compressor) | JSON complet |
| **T6e.4** | `set_fx_param` modifie un paramètre avec ack | ✓ OK (8 params modifiés sur 4 buses, ack `{"ok":true,...}`) | ack + valeur reflétée dans get_fx |
| **T6e.5** | Gestion d'erreurs : param inconnu / bus hors range | ✓ OK (`{"ok":false,"err":"unknown fx param"}`, idem bad bus) | refus propre |
| **T6e.6** | Routage via bus FX produit du signal | ✓ OK (Test 2 mic 0 → bus 1 reverb → out 0 : tap-OUT voie 0 = -92 dB vs -inf sans routage) | tap-OUT non silencieux |
| **T6e.7** | 0 régression matrice : voies non routées strictement silencieuses | ✓ OK (voies 2-7 = -inf dB) | -inf dB exact |
| **T6e.8** | Throughput pipeline avec 1 effet actif (EQ seul) | ⚠️ **23 kHz** (47 % nominal 48 kHz) — coût CPU sample-par-sample non vectorisé | ≥ 80 % nominal pour GO complet |
| **T6e.9** | Throughput pipeline avec reverb seul (4 combs + 2 allpass) | ⚠️ **15 kHz** (32 % nominal) — reverb le plus coûteux | idem |
| **T6e.10** | Throughput pipeline avec les 4 bus actifs en parallèle | ⚠️ **17 kHz** (36 % nominal) | idem |
| **T6e.11** | Latence ALSA buffer interne | ✓ **8 ms** (`play_delay=384`, `cap_delay=0`) | < 10 ms (déjà OK E6.d) |
| **T6e.12** | 0 régression DSP/SOF (firmware/kernel inchangés) | ✓ OK | tags v7.0-e0..e6d préservés |

## Limitations connues + Roadmap E6.f

### Throughput dégradé avec effets actifs

| Configuration | Throughput | Ratio nominal |
|---|---|---|
| Aucun effet (passthrough) | 48 kHz | 100 % |
| EQ seul | 23 kHz | 47 % |
| Reverb seul | 15 kHz | 32 % |
| Compresseur + reverb + delay + EQ | 17 kHz | 36 % |

**Cause** : chaque effet traite sample-par-sample dans la boucle principale (96 frames × 4 bus × N opérations par sample). Sur ARM Cortex-A53 sans vectorisation, le coût cumulé dépasse le budget temps 2 ms / 96 frames.

**Conséquence pratique** : avec effets actifs, le mixer-pro ne consomme que ~30-50 % des samples ALSA → drops réguliers (ALSA recover les périodes non lues). L'audio passe mais avec aliasing et artefacts. **Inutilisable en production avant optimisation.**

### Plan d'optimisation E6.f (sprint dédié)

1. **Block processing** : refactor `fx->process()` en `fx->process_block(in_array, out_array, n_frames)` pour permettre vectorisation auto-vectorisation GCC + cache friendliness.
2. **NEON SIMD intrinsics** : compiler les biquads et le reverb avec `arm_neon.h` (vectorisation 4-wide float). Gain typique ×3-4 sur A53.
3. **Precompute ramp tables** : pour le compresseur, lookup table `attack_coef`/`release_coef` au lieu d'`expf` (déjà fait au set_param, pas dans la boucle ; mais à vérifier pour env_to_db dans get_state).
4. **PREEMPT_RT kernel patch** : éliminer le jitter scheduling qui aggrave les overrun.
5. **CPU pinning** : `taskset` mixer-pro sur 1 cœur dédié, autres tâches sur autres cœurs.

Cible E6.f : ≥ 80 % nominal avec les 4 bus actifs en parallèle.

### Validation perceptuelle des effets : pas mesurée

L'infrastructure E6.e prouve que :
- Les algorithmes effets sont implémentés et chargés
- Les paramètres sont modifiables via socket
- Le routage via bus produit un signal

Mais **la validation audio perceptuelle** (entendre la compression, le reverb decay, l'echo delay, le shelf EQ) n'est pas faite. Elle nécessite :
- Un signal calibré injecté sur les inputs du mixer (sinus, click, voix)
- Une mesure spectrale ou temporelle sur les outputs (FFT, RTA, envelope)
- Idéalement le GUI test E7 qui pilote tout

Reportée à un sprint validation audio dédié + à l'étape E7 (GUI).

## Conclusion

- **Infrastructure effets E6.e livrée** : 4 effets natifs C (compressor, reverb, delay, EQ) chargés par défaut sur les 4 bus FX
- **Protocole socket complet** : `set_fx_param`, `get_fx`, `reset_fx` fonctionnels
- **0 régression** matrice/DSP/SOF
- ⚠️ **Throughput dégradé** (15-23 kHz avec effets actifs) → **E6.f mandatory avant production** (block processing + NEON)
- ⚠️ **Validation perceptuelle des effets pas faite** (à coupler au GUI E7 ou test audio dédié)

**Prochaine étape** : E6.f (optim perf) **OU** E7 (GUI test) selon priorité utilisateur.
