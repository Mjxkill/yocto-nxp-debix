# TESTS V13-VFOCUS E1 — « Place à la voix » (unmasking spectral sidechainé)

Date : 2026-07-07 · ARCHI : ARCHI_V13_VOICEFOCUS.md (validé critic).
Demande : « que la musique soit creusée aux fréquences de la voix pour
que la voix ressorte ».

## Implémentation

- duck_render() (audio_thread, après gate→comp, avant automix/mix) :
  - Sidechain = somme post-fader des tranches rôle bandmix lead/choir.
  - 5 bandes RBJ fixes 250/500/1k/2k/4k Hz (Q 1,4) : passe-bande
    d'analyse (coefs précalculés à l'init) → enveloppes par bande
    (att 5 ms / rel 180 ms) + activité large bande (seuil −45 dBFS).
  - Cuts cibles = max_cut × amount × (énergie bande / bande dominante),
    lissés att 10 ms / rel 200 ms (anti-chatter).
  - Application : MÊMES 5 gains pour toutes les tranches musique (rôles
    kick..line) → coefs peaking recalculés 1×/bloc (cos/sin précalculés,
    1 powf/bande), 5 biquads cascade par tranche (états tranche×bande),
    bandes à cut < 0,05 dB sautées. Zéro alloc, zéro transcendante/sample.
- Ops set_vfocus (on/amount 0-100/max_cut_db 0-12, partiels) /
  get_vfocus (+ active + cuts_db[5] temps réel). Persistance mixer_state.
- GUI page AUTO MIX : bandeau PLACE À LA VOIX — interrupteur, slider
  AMOUNT, 5 mini-barres de cut par bande (on VOIT la musique s'écarter),
  indicateur « ♪ voix détectée ».

## Validation board (groupe synthétique, voix = 440 Hz tremolo sur rôle lead)

| Test | Résultat |
|---|---|
| Musique seule (basse+batterie, pas de voix) | active=0, cuts **0/0/0/0/0** — musique intacte ✓ |
| Mix complet (voix chante) | active=1, cuts **2,6 / 4,8 / 1,9 / 0,8 / 0,4 dB** — le creux culmine dans la bande de la voix (440 Hz → 500), les aigus épargnés ✓ |
| Stabilité | cuts stables sur 10 s (2 lectures identiques à 0,02 dB) ✓ |
| Voix stoppée | retour à 0 partout (release 200 ms) ✓ |
| RT | xrun 0 nouveau, prof_mix 391 µs (inchangé) ✓ |
| GUI | 0 erreur QML ✓ |

## Reste

E2 : miroir web (bandeau dans la page AUTO MIX web), bandes/Q réglables,
option sidechain choir on/off. Écoute utilisateur avec vrais stems.

## Test utilisateur : EN ATTENTE (écoute : rôles assignés, PLACE À LA
VOIX ON, AMOUNT ~60-80 — la voix doit « flotter » au-dessus sans que la
musique semble éteinte)
