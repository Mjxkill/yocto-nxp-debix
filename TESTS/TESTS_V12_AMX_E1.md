# TESTS V12-AMX E1 — Automix Dugan (moteur mixer-pro)

Date : 2026-07-06 · ARCHI : ARCHI_V12_AUTOMIX.md (validé critic)
Priorité 0 de la roadmap V12 (ordre utilisateur).

## Implémentation

- mixer-pro : automix_update() par bloc 2 ms — énergie POST-fader
  (E_i × ig², raffinement critic : une tranche baissée ne vole pas de
  part), enveloppe asymétrique 10/200 ms, gain amplitude = sqrt(part
  d'énergie pondérée) (Dugan standard : 2 micros égaux = −3 dB chacun),
  plancher −15 dB, slew resp_ms 100 ms, OFF → retour doux à 1.0.
- Application : ig × automix_gain dans les phases A (sends) et C
  (master) — compose avec le fader, ne touche jamais input_target.
- Ops : set_automix {src,on,weight_db}, set_automix_cfg
  {on,resp_ms,floor_db}, get_automix (members + gains_db pour GUI).
- Persistance mixer_state (bloc automix, compat fichiers antérieurs).

## Validation board (échantillonneur embarqué 2 s, tones USB G=123 Hz/D=1 kHz)

| Phase | gains_db 8/9 | verdict |
|---|---|---|
| 2 sources | −2,2 / −3,9 (somme des parts = 1,00) | NOM constant ✓ |
| Gauche seule | **−0,0 / −15,0** | ouverture pleine / plancher ✓ |
| Retour 2 sources | −2,3 / −3,9 | réponse ~100 ms ✓ |
| Silence | −9,0 / −9,0 (parts de repos 1/8) | math exact ✓ |
| xruns pendant tout le test | **4 → 4 (zéro nouveau)** | RT sain ✓ |

Note : asymétrie ≈1,6 dB entre 123 Hz et 1 kHz à amplitude égale —
ondulation d'enveloppe des blocs 2 ms sur les graves (2 ms = ¼ de
période à 123 Hz), sans incidence pour la parole. À revoir si besoin
(fenêtre d'énergie plus longue).

## Reste (E2)

GUI : badge/toggle AMX par tranche + poids (drawer) + barre de gain
auto sous les faders (get_automix 10 Hz, gated page MIXER), miroir web.
Automix laissé OFF sur la board en attendant la GUI.

## Test utilisateur : moteur validé par mesures ; écoute réelle avec la GUI E2
