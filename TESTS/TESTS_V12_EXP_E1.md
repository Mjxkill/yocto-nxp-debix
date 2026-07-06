# TESTS V12-EXP E1 — Expandeur/gate par tranche (moteur)

Date : 2026-07-06 · ARCHI : ARCHI_V12_EXPANDER.md (validé critic, approved)
Priorité 3 roadmap V12 — dernière fonction du sprint live.

## Implémentation

- mixer-pro : `exp_render()` in-place sur in_block[0..15], SOUS target_lock,
  juste après le convert S32→float et AVANT smp/loop/automix/mix → le gate
  s'applique à tout l'aval (sends, master, looper, automix, tap NPU).
  P1/P2 exclues. Par tranche : on, threshold −80..0 dB, ratio 1..20,
  attack 0.5..100 ms, release 5..1000 ms, range 0..80 dB, hold 0..500 ms.
- Algo par bloc (2 ms) : crête → enveloppe asymétrique (coefs ka/kr
  PRÉCALCULÉS au set, zéro expf en RT) → hold anti-chatter (granularité
  1 bloc) → gain (loi (env−thr)×(ratio−1), clamp −range) → rampe linéaire
  intra-bloc (zipper-free) → GR publié en atomic milli-dB.
- Ops : `set_expander` (updates partiels, précalculs dans le handler sous
  target_lock), `get_expander` (16 tranches + gr_db temps réel).
- Persistance : 16 lignes `expander …` en fin de mixer_state, chargées
  via exp_configure (re-clamp + re-précalc). Défauts off au boot.

## ⚠ RÉGRESSION V12-AMX DÉCOUVERTE ET CORRIGÉE (fscanf desync)

La boucle `automix_weights` de load_mixer_state lit exactement
N_INPUT_REAL floats et laisse le `\n` non consommé ; le format suivant
`"mute_mask %u\n"` commençait par un littéral SANS skip d'espace → échec
silencieux, désynchronisation de tout le reste du parse → **faders, mutes,
routing master ET expander jamais restaurés au boot depuis V12-AMX**
(le save réécrivait ensuite le fichier avec les défauts → config user
perdue au fil des restarts de la journée). Fix : espace de tête
`" mute_mask %u\n"`. Diagnostic : repro parseur cross-compilé exécuté sur
le board (16 lignes OK) vs daemon (goto done à master s=0 o=0) + ftell.

## Validation board (tone USB 440 Hz −12 dBFS sur USB1/strip 8)

| Test | Résultat |
|---|---|
| Gate off (référence) | in[8] = 25,119 % FS |
| Gate on thr=−6 (au-dessus), ratio 20, range 60 | in[8] = 0,025 % FS = **−60,0 dB exact**, gr_db=−60.0 ✓ |
| thr=−20 (sous le signal) → ouvert | in[8] = 25,119 % (retour exact), gr=0 ✓ |
| Tone coupé → gate se referme (release) | gr_db → −60 sur bruit résiduel ✓ |
| 16 gates ON pendant 30 s | xrun **0 nouveau**, prof_mix 363→435 µs (+72 µs ≈ 3,6 % du budget 2 ms) ✓ |
| Persistance (config distinctive src 3 + restart) | on/thr/ratio/atk/rel/range/hold restaurés à l'identique ✓ |
| Persistance faders/mute (post-fix) | gain 0.4242 + mute_mask 128 restaurés après restart ✓ |

## Reste

E1b : GUI onglet GATE dans le drawer d'effets par tranche (natif) avec
barre GR temps réel. E2 : web. V2 : sidechain HPF.

## Test utilisateur : EN ATTENTE (écoute gate sur micro réel)
