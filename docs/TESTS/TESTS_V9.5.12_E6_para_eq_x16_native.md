# Test Fiche : V9.5.12 — E6 para_eq_x16 natif + lock-free chain.set_param + GUI courbe EQ

**Date** : 2026-06-09
**Statut** : GO (architecture stable, modèle à améliorer en Phase 3)

## Référentiel

| Champ | Valeur |
|---|---|
| Phase | Phase 2 — Step D fix + Step E (UI dashboard) |
| Version | V9.5.12 |
| Étape | E6 — EQ natif lock-free + retrait target_lock + GUI courbe |

## Diagnostic step-by-step (LED comme oracle)

| Config testée | LED | Conclusion |
|---|---|---|
| LSP Para EQ x16 + daemon push 76 params/cycle | ❌ glitch | ? |
| LSP Para EQ x16 + push **EQ only** (16 gains) | ❌ glitch | EQ coupable |
| LSP Para EQ x16 + push **exciter only** | ✅ OK | exciter OK |
| LSP Para EQ x16 + push **stereo only** | ✅ OK | stereo OK |
| LSP Para EQ x16 + push **limiter only** | ✅ OK | limiter OK |
| Calf Equalizer 12B + manual rampe 80/s | ❌ glitch | Plugin LV2 = problème générique |
| `para_eq_x16` natif (nouveau) + manual rampe | ❌ glitch | reste à voir |
| `para_eq_x16` + **target_lock RETIRÉ** + bulk push | ✅ OK | **trouvé** |

→ **Cause racine** : `pthread_mutex_lock(&g_st.target_lock)` autour de `chain.set_param` contendait avec audio_thread RT99. Lock retiré = pas de contention.

## Modifications mixer-pro

### 1. `effects.c/h` — nouveau moteur natif `para_eq_x16`

- 16 biquads peak RBJ cookbook stéréo
- Lock-free (atomic write float + biquad recompute ~50 FLOPs)
- Pas de worker thread, pas de lilv overhead
- Params : `bN_freq`, `bN_gain_db`, `bN_q` (N=0..15)
- Defaults : log-spaced 20-20000 Hz, gain 0 dB, Q 1.0
- Cost run-time : 3072 biquad_step / period (96 frames × 2 ch × 16 bands) = ~50 µs/period

### 2. `mixer-pro.c` — retrait target_lock + SHM tap

```c
/* AVANT */
pthread_mutex_lock(&g_st.target_lock);
int rc = g_insert_chain.set_param(...);
pthread_mutex_unlock(&g_st.target_lock);

/* APRÈS V9.5.12 */
int rc = g_insert_chain.set_param(...);
```

Le lock était overcautious : `chain.set_param` est interne lock-free pour LV2 (atomic ctrl_target write) ET para_eq_x16 (struct field write + biquad recalc).

### 3. SHM tap audio export

- `/dev/shm/mixer-pro-tap-usb` : ring 32 KB stéréo float32, header 128 B
- `audio_thread` écrit USB IN [8,9] chaque période (memcpy 768 B/2ms = trivial)
- daemon `mixer-ml-inference` mmap et lit (process séparé)

### 4. Endpoints socket Unix

- `set_assistant_mode {"mode":"mastering|passthrough","source":"hw|usb"}`
- `get_assistant` → mode + source (le daemon poll cet état à 2 Hz)

## Modifications mixer-ml-inference (daemon)

### Push para_eq_x16 (48 params/cycle pour EQ)

```c
for (int b = 0; b < 16; b++) {
    /* push bN_freq + bN_gain_db + bN_q par bande */
    "[0,\"b%d_freq\",%.1f],[0,\"b%d_gain_db\",%.3f],[0,\"b%d_q\",%.3f]"
}
```

Total : 16 EQ × 3 + 4 exciter + 3 stereo + 5 limiter = **60 params/cycle bulk**.
Cycle 100 ms = 10 Hz push.

### Debug flags ML_PUSH_ONLY / ML_APPLY_PARAMS / ML_INVOKE_ENABLED

Variables env pour isoler le coupable lors du diagnostic (peuvent être désactivées).

## Modifications mixer-gui-http (GUI)

### Panel "Mixer Assistant" overlay

- Bouton header `🎚 Assistant ●` (point = mode mastering actif)
- Dropdown mode (passthrough/mastering) + source (hw/usb)
- Affichage chain LV2 (4 slots)
- **Canvas courbe EQ live** : somme exacte des 16 biquads peak RBJ cascadés
  - Width 840 × Height 272
  - Grid Hz log + Grid dB ±24
  - Refresh 5 Hz (poll `get_insert`)
  - Tri par fréquence croissante (modèle prédit dans ordre arbitraire)
  - Points sur f0 par bande (vert = boost, orange = cut)
- Légende 16 colonnes triées : f / dB / Q
- Dials Exciter + Stereo + Limiter live

## Tests réalisés

| # | Test | Résultat |
|---|---|---|
| 1 | Build mixer-pro avec para_eq_x16 + lock-free | ✅ |
| 2 | Build mixer-ml-inference V1 (push 60 params) | ✅ |
| 3 | Setup chain `para_eq_x16 + Calf Exciter + Calf Stereo + LSP Limiter` | ✅ active n=4 |
| 4 | Daemon ML actif + LED clignote | ✅ stable |
| 5 | Play wav UAC2 + capture tap_out | ✅ son passe, signature mastering visible |
| 6 | GUI courbe EQ live affichée | ✅ refresh 5 Hz |
| 7 | Live param change daemon : LED reste alive | ✅ |
| 8 | Verdict auditif utilisateur | "ça ressemble à l'eval c'est bien" |

## Limites identifiées (à corriger Phase 3)

| Issue | Cause probable |
|---|---|
| Effet mastering trop léger | Modèle v5.12 trop conservateur (dataset trop petit, pas d'augmentation, pas de loss compression) |
| Pas de compression dynamique perçue | Loss n'inclut pas la dynamique cible (crest factor delta non pénalisé) |
| EQ varie mais reste subtle | Modèle n'a pas appris à allouer ses 16 bandes efficacement (overlapping freqs) |

## Conclusion

✅ **GO** Phase 3 (réentraînement modèle) :
- **Architecture stable** validée bout-en-bout
- Process isolation fonctionnelle (TFLite NPU + mixer-pro RT99)
- EQ natif lock-free évite les problèmes de plugins LV2 sous flux dense
- GUI live opérationnel (courbe EQ, dials)
- LED reste alive en continu

Phase 3 = améliorer la **qualité du modèle ML** :
1. Train sur dataset complet (259 paires + autres datasets)
2. Augmentation données (gain/pitch/time/pre-EQ)
3. Modèle plus gros (Conv1D 128→256 channels)
4. Loss compression + perceptuelle (LUFS, MFCC)

## Référence

- ARCHI : `ARCHI/ARCHI_V9.5.12.md`
- Fiches précédentes : E1, E2, E3, E4, E5 (architecture daemon)
- Commit Step D : `73627795`
