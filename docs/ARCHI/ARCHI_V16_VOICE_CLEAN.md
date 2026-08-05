# ARCHI V16 — VOICE CLEAN : bouton BRUT / DTLN / GTCRN / SPECSUB par voie voix

Date : 2026-08-06. Validé Michael (étude ETUDE_V16_VOIX_PROPRE.md) :
« par CPU on essaie, un bouton qui switch brut/DTLN/GTCRN/soustraction
spectrale — il faut que j'entende dans la musique et en live — peut-être
sur CPU3 ». Fonctionnalité complète, R&D d'écoute.

## 1. Périmètre

- Modes par voie FLAGUÉE voix (rôles LEAD/CHŒURS) :
  0 BRUT (bypass total, chemin identique, zéro coût)
  1 DTLN (TFLite CPU, modèle 16 kHz officiel)
  2 GTCRN (si export ai-edge-torch abouti — sinon le mode répond
    « indisponible » proprement, jamais silencieux)
  3 SPECSUB (soustraction spectrale de puissance 48 kHz native,
    référence = mix musique, α adaptatif dans les trous de chant)
- Un seul canal traité à la fois (R&D — l'échelle multi-voies attendra
  les mesures CPU).

## 2. Topologie temps réel

```
audio_thread (core 2)                      voice-clean daemon (CPU3, FIFO 60)
  in_block[src] après exp_render ──► ring SHM TX (voix + réf musique)
  in_block[src] ◄── ligne à retard D ◄── ring SHM RX (voix traitée)
```

- **Ring SHM** `/dev/shm/ala-voice-clean` (pattern midix, 2 rings dans un
  même segment : TX 2 canaux float [voix, réf-musique], RX 1 canal).
- **Réf musique** = somme des voies rôle musique actives (déjà sommables
  dans l'audio_thread, coût 1 mac_block).
- **Latence FIXE D = 24 périodes (48 ms)** identique pour les 3 modes
  traités — comparaison A/B équitable, budget qui couvre DTLN
  (resample 48→16, hops 8 ms, retour) et le jitter daemon. BRUT = zéro
  délai (vrai bypass). Le passage brut↔traité fait un saut temporel de
  48 ms sur la voix : assumé en R&D (fondu 10 ms au switch pour éviter
  le clic). LA VOIX EST EN RETARD de 48 ms sur le groupe en mode traité —
  c'est précisément ce que Michael veut juger à l'oreille.
- **Famine daemon** : si RX vide au moment du pop → on sort du SILENCE sur
  la voie (jamais le brut non traité d'un coup) + compteur publié. Le
  daemon en retard se voit, ne s'entend pas en larsen de mode.

## 3. Placement CPU (demande « peut-être CPU3 »)

CPU3 est isolé (isolcpus=2,3) et héberge cap/play_uac2 (SCHED_FIFO 95,
I/O bloquant, faible CPU). Le daemon s'y épingle en **SCHED_FIFO 60** :
préempté net par l'UAC2, seul le reste du temps. Mesure de garde :
xruns UAC2 delta AVANT/APRÈS activation d'un mode (critère GO : 0).
Repli documenté si contention mesurée : CPU1.

## 4. Moteur (mixer-pro) — module `voice_clean.c/h`

- État : `mode` par voie (atomic), ligne à retard D par voie active,
  position d'insertion : après exp_render, avant al_render/eqx (la voix
  nettoyée alimente TOUTE la suite : automix, vfocus, mesures, mix).
- Ops (pattern V14) : `voice_clean {src, mode}`, `voice_clean_status`
  (mode, latence, famines, présence daemon). Persistance du mode : NON
  (R&D — repart en BRUT au boot, invariant opt-in).
- Bypass strict : mode 0 → aucun accès ring, chemin d'origine.

## 5. Daemon `voice-clean` (nouvelle recette)

- C, TFLite C API (pattern mixer-ml-inference : -ltensorflow-lite).
- Pipeline DTLN : pop 48 k → resample 48→16 (polyphase simple 3:1) →
  DTLN 2 étages (états LSTM persistants) → 16→48 → push.
- Pipeline SPECSUB : STFT 512/50 % @48 k, P_voix−α·P_réf par bande
  (plancher spectral β), α appris par bande quand la voix est absente
  (détection : énergie voix < seuil relatif), reconstruction OLA.
- Pipeline GTCRN : idem DTLN si modèle dispo (interface caches).
- Watchdog : daemon absent → moteur bascule BRUT tout seul + status.

## 6. GUI (web — parité LCD notée pour après)

Bouton cyclique sur la tranche voix (drawer FX de la tranche, section
dédiée « VOIX PROPRE ») : BRUT / DTLN / GTCRN / SPECSUB + état (latence,
famines, daemon). Op via /api/cmd.

## 7. Validation

1. Build + batterie ops (switch 4 modes, refus voie non-voix, watchdog).
2. Morceau KatWright : par mode — delta xrun (=0), delta xruns UAC2 (=0),
   charge CPU3, famines (=0), et VU/keepers cohérents.
3. Écoute Michael : les 4 positions pendant la musique (le but).
4. Fiche TESTS_V16 + revert facile (mode boot = BRUT).
