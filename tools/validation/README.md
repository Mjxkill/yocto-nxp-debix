# Suite de validation A.L.A.

Campagne automatisée créée le 2026-07-08 (voir
`docs/TESTS/RAPPORT_VALIDATION_V13.1_2026-07-08.md` pour le rapport de
référence et les bugs qu'elle a trouvés).

## Rejouer la campagne complète

```bash
tools/validation/run_all.sh            # board 192.168.0.198
```

Fait un backup de l'état board, puis enchaîne P2→P5. **Non destructif** :
tous les tests set font lire→écrire→vérifier→restaurer ; le looper
n'enregistre que du silence sur la piste 5 puis la vide ; la scène de
test utilise le slot 5 (libéré ensuite).

| Script | Phase | Contenu |
|---|---|---|
| `validate_p2.py` | P2 | 23 ops get + 11 round-trips set + 6 tests de robustesse |
| `validate_p3.py` | P3 | persistance restart mixer-pro + scène complète slot 5 |
| `validate_p4.py` | P4 | sampler, looper, midix, automix, bandmix, taps, larsen, routes HTTP |
| `validate_p5.py` | P5 | CPU/xrun par scénario de charge (tableau) |
| `validate_p8_full.py` | P8 | TOUT ACTIVÉ 60 s : gates+comps ×16, EQ ×24, automix, vfocus, looper ×6, sampler, 4 pollers — restauration intégrale vérifiée |
| `soak.py` | — | endurance : CSV 1 échantillon/min + burst stress/10 min (`nohup python3 soak.py &` sur la carte) |

## P6 (reboot) — manuel

```bash
ssh root@IP "alsactl store; systemctl reboot"
# après boot :
ssh root@IP "journalctl -b -u ala-fx-restore | grep fx-restore"
#   attendu : « fx-restore: alsactl vérifié (essai N) »
ssh root@IP "amixer -c softac5212tdm cget \"iface=MIXER,name='TAC0 ADC Biquad Config'\""
#   attendu : values=3
```

## Pièges d'API (établis par la campagne)

- Gains **linéaires** partout : `set_input_gain {"gain":0.5}`,
  `set_send {"gain":0.1}`, `get_output_gain` renvoie `gains` ×1000.
- `set_insert_bypass` prend `{"on":1}` (sémantique bouton MASTERING).
- `set_automix` = adhésion par tranche (`src` requis) ; enable global =
  `set_automix_cfg {"on":1}`.
- Looper : fin d'enregistrement = action `play` (fige la longueur).
- `bandmix_measure` exige `src` (ou `src:-1` pour annuler) et lance une
  mesure 12 s — ne pas l'appeler dans un smoke test.
- Cartes ALSA : numérotation instable au boot → toujours
  `-c softac5212tdm`, jamais `-c 4`.
