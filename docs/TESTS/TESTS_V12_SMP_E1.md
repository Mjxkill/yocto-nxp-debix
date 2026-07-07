# TESTS V12-SMP E1 — Sampleur (moteur + page PADS)

Date : 2026-07-06 · ARCHI : ARCHI_V12_SAMPLER.md (validé critic)
Priorité 1 roadmap V12.

## Implémentation

- mixer-pro : banque 16 slots, WAVs de /var/lib/ala/samples (tri alpha),
  parseur RIFF minimal (PCM 16/24/32 + f32, mono→dup, 48 kHz exigé,
  plafond RAM 256 Mo), lecture one-shot additionnée dans les tranches
  P1/P2 (in_block[16/17], mortes en skip_phone) sous le lock du cycle.
  Libération différée des buffers au reload (jamais de free d'un buffer
  lisible par l'audio). Ops : sampler_list/trigger/stop/reload.
- Console native : page PADS (6 pages désormais — index ROUTING/SYSTÈME
  décalés, gating client inchangé MIXER=0/MASTERING=2), grille 4×4,
  tap=trigger (retrigger), pad ambré + position quand en lecture,
  RECHARGER + STOP ALL. Niveau/routage = tranches P1/P2 (sémantique
  console : le sampleur est une source).

## Validation board (jingle 2 s généré)

| Test | Résultat |
|---|---|
| Scan au boot | `smp: 1 samples chargés (750 Ko)` ✓ |
| sampler_list | slot 0 nom/durée corrects ✓ |
| trigger → meters | P1/P2 ≈ −13 dBFS, OUT 1/2 alimentées via matrice ✓ |
| Suivi position | 0,8 s à mi-lecture ✓ |
| Fin de sample | playing 1→0 automatique ✓ |
| xruns | 1 → 1 (zéro nouveau) ✓ |
| WAV invalide | rejet loggé (chemin testé par revue, 44.1k → log) |

## Reste (E2)

Web : page PADS miroir + upload WAV (gui-http multipart) + gain/couleur
par pad. Loop/sync → chantier looper (priorité 2).

## Test utilisateur : EN ATTENTE (page PADS à l'écran + écoute)
