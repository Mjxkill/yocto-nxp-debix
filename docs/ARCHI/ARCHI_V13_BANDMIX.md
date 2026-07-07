# ARCHI V13-BANDMIX — auto-mix musique (assistant groupe)

Date : 2026-07-07 · Demande : « je suis nul en son et je dois sonoriser
mon groupe ». Périmètre validé : E1 (soundcheck + calcul) + suivi live
d'un coup, sur les 16 tranches réelles (DSP IN + USB IN — stems WAV de
test par l'USB).

## Philosophie

PAS un Dugan (partage d'énergie = pompage en musique). Un assistant en
3 temps : (1) soundcheck mesuré par tranche, (2) calcul déterministe par
RÔLE (gains, gates, compresseurs, faders de départ), (3) suivi live LENT
(thermostat ±3 dB, échelle 10 s) qui tient l'équilibre verrouillé, avec
priorité voix. Zéro NPU en E1 (classification auto des rôles = V2).

## Brique nouvelle : compresseur natif par tranche (V13-COMP)

Les tranches USB n'ont aucun traitement dynamique par canal (le DRC est
dans le DSP, chemin micros). Ajout d'un compresseur downward natif dans
mixer-pro, même patron que le gate V12-EXP :
- `comp_render(in_block)` in-place sur 0..15, APRÈS exp_render (gate
  puis comp, ordre console standard), AVANT smp/loop/automix/mix.
- Par tranche : on, threshold_db (−60..0), ratio (1..20), attack_ms
  (0.5..250), release_ms (5..2000), makeup_db (0..24).
- Enveloppe crête par bloc 2 ms, coefs précalculés au set, loi :
  au-dessus du seuil g_db = (thr − env_db) × (1 − 1/ratio) ; rampe
  linéaire intra-bloc ; GR publié (atomic milli-dB).
- Ops set_comp / get_comp (updates partiels), persistance mixer_state
  (lignes « comp … »), off = zéro coût.
- Réutilisable à la main (drawer GATE → onglet DYNAMIQUE plus tard).

## Mesure : RMS par tranche toujours actif

La boucle de peaks (audio_thread) scanne déjà tous les samples : ajout
d'un accumulateur x² par tranche → RMS par bloc publié (atomic), coût
≈ 1 MAC/sample. Le plan de contrôle lit ces RMS à 10 Hz et entretient :
- long_rms (lissé 3 s) par tranche — pour le keeper live ;
- pendant un soundcheck : rms_avg, rms_max, peak_max, floor (percentile
  bas des RMS de bloc) — pour le calcul.

## Rôles et presets (E1, fixes)

| Rôle | Cible mix (rel. voix = 0 dB) | Gate | Comp |
|---|---|---|---|
| voix lead | 0 | doux (ratio 2, seuil bas) | 3:1, att 15 ms, rel 150 |
| chœurs | −4 | doux | 3:1 |
| grosse caisse | −2 | dur (ratio 20, hold 60) | 4:1, att 5, rel 80 |
| caisse claire | −3 | dur | 3:1, att 5 |
| batterie (OH/autre) | −6 | aucun | 2:1 doux |
| basse | −3 | aucun | 4:1, att 20, rel 250 |
| guitare | −5 | aucun | 2.5:1 |
| clavier/piano | −5 | aucun | 2:1 |
| ligne/autre | −5 | aucun | aucun |
| — (off) | tranche ignorée | — | — |

Gain staging : input_target ajusté pour rms_avg ≈ −20 dBFS (crête gardée
< −6 dBFS). Gate threshold = floor mesuré + 8 dB (jamais au-dessus de
rms_avg − 10). Faders (master vers OUT DSP 0/1) posés selon les cibles.

## Séquence utilisateur (page AUTO MIX, 9e page)

1. Chaque tranche : sélecteur de RÔLE (‹ ›).
2. « MESURER » sur une tranche → le musicien joue 12 s (barre de
   progression + VU) ; re-mesurable à volonté.
3. « CALCULER LE MIX » → applique gains/gates/comps/faders (uniquement
   sur les tranches mesurées + rôle ≠ off).
4. Le groupe joue, l'utilisateur ajuste s'il veut, puis « VERROUILLER
   L'ÉQUILIBRE » → capture des parts de loudness de référence par
   tranche (30 s de moyenne) — la référence inclut les retouches user.
5. « LIVE ON » → keeper : toutes les 1 s, compare long_rms aux parts de
   référence, corrige par keeper_gain[] ±3 dB max, slew 0,5 dB/s.
   Priorité voix : si la part voix lead chute sous sa référence de
   > 2 dB alors que le groupe joue, la correction voix est doublée et
   les autres bornées à −1 dB. Les tranches silencieuses (< floor+6)
   ne sont PAS corrigées (pause d'un instrument ≠ déséquilibre).

## Implémentation

- mixer-pro : keeper_gain[16] appliqué dans la même chaîne que
  automix_gain (ig = input_gain × automix_gain × keeper_gain, slew dans
  smooth_gains, OFF → retour 1.0). État bandmix (roles, mesures, réf,
  live_on) dans le control plane, tick keeper dans persistence_thread
  (1 Hz, déjà là). Soundcheck : collecte via les RMS atomics, fenêtre
  gérée par le control thread (timestamps CLOCK_MONOTONIC).
- Ops : bandmix_role {src,role}, bandmix_measure {src,action:start|stop},
  bandmix_calc, bandmix_lock, bandmix_live {on}, bandmix_status
  (rôles, état mesures, gains calculés, keeper_gain, parts réf/courantes).
- Persistance : rôles + réf + live dans mixer_state (fin de fichier).
- GUI native : page AUTO MIX (nav 9) — liste 16 tranches (rôle,
  VU, bouton MESURER + état, résultat), bandeau CALCULER / VERROUILLER /
  LIVE + indicateurs keeper (barres ±3 dB). Web : miroir (page AUTO MIX).

## Tests E1 (stems WAV par USB gadget)

1. 6 stems (drums, bass, gtr, piano, lead, chœurs) sur U1..U6, rôles
   assignés, mesure de chaque stem seul, CALCULER → gains/gates/comps
   posés, mix de départ cohérent (voix devant), écoute.
2. VERROUILLER pendant le morceau complet → LIVE ON → rejouer le
   morceau avec la batterie +6 dB (sox gain sur le stem) → le keeper
   ramène la part batterie ≈ référence (±1 dB) en < 20 s, sans pompage
   audible ; la voix ne bouge pas de sa part.
3. Stem silencieux (pause) → keeper ne touche pas la tranche.
4. Delta xruns 0 ; comp 16× on : mesure prof_mix_us.
5. Persistance : reboot → rôles/réf/live restaurés.

## Invariants

- Chemin RT : comp_render = même coût que le gate ; RMS = 1 MAC/sample ;
  keeper appliqué par multiplication déjà existante. AUCUNE logique
  lourde dans l'audio_thread — tout le cerveau est dans le control plane.
- Le keeper ne touche JAMAIS les faders (trim séparé visible, comme le
  gain automix) ; désactivable instantanément (retour doux à 1.0).
- Correction bornée ±3 dB, vitesse bornée 0,5 dB/s → physiquement
  incapable de pomper.
