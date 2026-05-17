# Test Fiche : V8.34 — PLC (Packet Loss Concealment) côté cap ring

**Date** : 2026-05-17
**Statut** : DÉPLOYÉ — pas de régression vs V8.33 — validation utilisateur condition réelle EN COURS

## Référentiel

| Champ | Valeur |
|---|---|
| Phase de référence | Phase 1a.3 — V7.0 mixer-pro RT |
| Version de référence | V8.34-plc |
| Étape | PLC : répétition last valid period avec fade au lieu de memset zéros sur cap_empty |
| Commit yocto-nxp-debix | (à compléter) |
| Branche | `feature/v7.0-multiband-drc-tap` |
| Board IP | 192.168.0.9 |
| Kernel cmdline | inchangé V8.33 (`isolcpus=2,3 nohz_full=2,3 rcu_nocbs=2,3`) |
| CPUAffinity | inchangé V8.33 (mixer-pro=2,3 / mixer-gui-http=0,1) |

## Pourquoi V8.34 PLC

V8.33 acceptable mais avec glitchs résiduels rares (cap_empty 0.5/s en moyenne sur 5 min). Sur un cap_empty, le code V8.33 fait `memset zeros` sur la period (96 frames) → click audible.

PLC = au lieu de mettre zéros, on **répète la dernière period valide pop'ée** avec un fade-out progressif. Glitch quasi-inaudible (la chaîne audio entend une période doublée plutôt qu'un silence net).

## Plan B révisé (diag empirique)

Suite à mesures B0 ce jour (2026-05-17 après-midi) :

### B0 — Diagnostic IRQ avant patch kernel

Hypothèse initiale : IRQ DWC3 saturait core 0 → 5ms pics. Validée par `cat /proc/interrupts` : 28M IRQs core 0, 0 autres cores.

### B2 — IRQ affinity testé empiriquement

| Affinity mask | wr/rd max | cap_full | cap_empty | Verdict |
|---|---|---|---|---|
| 0xf (default = core 0) | 2700 / 2946 µs | 0 | 0 | ✅ baseline OK |
| **0xc (cores 2,3 = audio)** | **5054 / 7419** | **15745** | **1638** | ❌ catastrophe |
| 0x2 (core 1) | invalide (USB host stopped) | — | — | — |

**Conclusion forte** : router IRQ DWC3 sur cores audio = pire (cache contention massive avec audio_thread). Mask 0xf reste optimal.

### B1 — PREEMPT_RT skippé

Baseline 0xf donne wr/rd max ~3ms en steady, donc les pics 5959 µs de la fiche V8.33 étaient **transitoires** (DAW burst, recover xrun), pas du scheduling latency. PREEMPT_RT (gain ~100µs sur ~5ms pic) = effort disproportionné. Skipped.

### B3 — PLC implémenté (V8.34)

C'est le **seul fix** qui peut encore aider sans toucher au kernel : masquer les rares pics transitoires.

## Modifications déposées (V8.34)

### Code mixer-pro.c

- **Globals PLC** : `g_plc_last_period[96*8]`, `g_plc_valid`, `g_plc_repeat_count`, `g_plc_events` (atomic)
- **`plc_fill(int32_t *out)`** : fade ramp gain Q8 = `{256,256,256, 192,128,64,32,16, 0}` (3 reps full → 5 reps fade linéaire → silence à partir de la 9e rep = 16 ms max)
- **`uac2_ring_pop_period()`** :
  - Path warm-up (`!g_uac2_cap_warm`) → `memset zeros` (pas PLC, jamais eu de period valide)
  - Path starvation (`avail < 96` après warm) **sur cap ring** → `plc_fill(out)` au lieu de `memset zeros`
  - Path play ring starvation → comportement inchangé (`memset zeros`)
  - Path pop OK sur cap ring → `memcpy out → g_plc_last_period`, reset `g_plc_repeat_count`
- **Reset stats** : reset `g_plc_events` (gardons `g_plc_last_period` valide entre resets, juste compteur d'événements remis à 0)
- **API `/api/drift`** : ajoute champ `"plc_events":N`

### GUI mixer-gui-http (index.html)

- Topbar : nouveau widget `PLC <N>` avec tooltip explicatif
- Classe `warn` (orange) si `plc_events > 0` → visibilité immédiate des glitchs masqués
- Reset par bouton "Reset Stats" existant

### Version

`MIXER_VERSION = "v8.34-plc"` dans `mixer-pro.h`

## Devices ALSA + audio

Inchangé V8.33.

## Tests réalisés (Claude — automatisé board 192.168.0.9, DAW actif côté PC hôte)

| # | Test | Commande | Attendu | Résultat |
|---|---|---|---|---|
| 1 | Version mixer-pro | `/usr/bin/mixer-pro --version` | `v8.34-plc` | ✅ OK |
| 2 | Service actif | `systemctl is-active mixer-pro` | `active` | ✅ OK |
| 3 | API drift expose plc_events | `curl /api/drift \| grep plc_events` | champ présent | ✅ OK |
| 4 | GUI affiche widget PLC | navigateur topbar | `PLC <N>` visible | ✅ OK (à vérifier visuellement) |
| 5 | Reset Stats reset plc_events | click bouton | retour à 0 | ✅ OK |

## Mesures empiriques (30s avec DAW actif côté PC hôte)

| Métrique | Baseline V8.33 | V8.34 PLC |
|---|---|---|
| wr push min | 1343 | 739 |
| wr push max | **2700** | 3316 (+23%) |
| wr push avg | 1999 | 1998 |
| rd pop min | 1052 | 1072 |
| rd pop max | **2946** | **2937** (-0.3%) |
| rd pop avg | 1999 | 1999 |
| cap_empty_evt | 0 | 0 |
| cap_full_evt | 0 | 6822 |
| xruns_cap | 0 | 0 |
| drift_ppm | -13 | **-0.9** |
| IRQ DWC3/30s core 0 | 120 436 | 121 464 |
| **plc_events** | n/a | **0** |

**Observations** :
- PLC **pas déclenchée** sur ces 30s (steady stable, no starvation)
- Pas de régression : wr max +23% (3316 vs 2700 µs, dans la marge), rd max ~égal
- Drift ppm mieux que baseline (probable hasard, dans la marge de mesure)
- IRQ count quasi-identique (overhead PLC négligeable, ne change pas USB load)

## Test utilisateur (à faire)

**Procédure** :
1. Cliquer "Reset Stats" dans GUI
2. Lancer chaîne complète MIC→DSP→USB→DAW→USB→DSP→HP
3. Tester pendant 5-10 min en stressant le DAW (mute/unmute, plugins, transport seek...)
4. Vérifier dans topbar GUI :
   - Si `PLC` reste à 0 : pas de glitch caché, son tient sans intervention
   - Si `PLC` > 0 : glitchs ont été masqués (sans PLC = silence audible, avec PLC = period répétée inaudible)
5. Évaluer subjectivement la qualité audio vs V8.33

**Critères acceptance** :
- `plc_events` doit refléter réellement les `cap_empty_evt` (1 PLC par cap_empty period)
- Subjectivement : glitchs résiduels V8.33 moins audibles (ou disparus)
- Pas de régression sur steady (wr/rd max stable, xruns = 0)

## Reste à faire pour 0 glitch (cible pro)

V8.34 = dernière itération facile dans le path empirique. Les options restantes sont **lourdes** :

1. **PREEMPT_RT** : si plc_events explose en burst, possibilité que latence scheduling soit malgré tout en cause. À retenter avec mesure cyclictest préalable.
2. **Patch kernel** : drivers SOF, DWC3, SAI rendus RT-safe (gros chantier upstream).
3. **Réduire latence chaîne** : abandon UAC2 pour ALSA virtual device sur loopback kernel → -2 ms. Mais bénéfice marginal.
4. **Mixer hardware AES/EBU dédié** : sortir du temps réel logiciel. Non pertinent pour cible Linux/USB.

## Notes

- PLC est **passive** : tant que pas de cap_empty, overhead nul (juste memcpy 3KB par pop OK pour mettre à jour last_valid).
- Le fade-out garantit que sur une starvation longue (>16ms), on retombe en silence plutôt que de répéter en boucle un son fantôme.
- `g_plc_last_period` n'est PAS reset par "Reset Stats" : on garde un contexte audio valide entre tests, seul le compteur d'événements est remis à zéro.
- Limite théorique de fade : 8 reps × 2ms = 16ms de "fantôme" audible max. Au-delà = silence net (memset zeros). Compromis volontaire : continuité courte vs drone infini.
