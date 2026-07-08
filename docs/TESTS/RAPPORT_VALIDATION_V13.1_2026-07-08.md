# RAPPORT DE VALIDATION COMPLÈTE — A.L.A. V13.1 — 2026-07-08

**Branche** : `L6.6.36-2.1.0-debix_model_ab` · **Board** : 192.168.0.198 (kernel 6.6.36-rt35)
**mixer-pro** : v9.5.12-slow-smooth · **Campagne** : P0→P7 automatisée depuis le PC (API + ssh)
**Scripts** : `/root/tests/validate_p2.py`, `validate_p3.py`, `validate_p4.py`, `validate_p5.py`
**Backup préalable** : `/root/tests/backup_validation_20260708/` (mixer_state, scènes, asound.state, /var/lib/ala)

## Résultat global

| Phase | Contenu | Résultat |
|---|---|---|
| P1 | Santé de base (services, journaux, CPU) | ✅ 6 services actifs, 0 restart, journaux propres |
| P2 | 66 ops API mixer-pro (get + round-trips + robustesse) | ✅ **40/40 PASS**, xrun Δ=0 |
| P3 | Persistance restart + scènes | ✅ 13/14 → **bug sends trouvé et corrigé** |
| P4 | Fonctionnel (sampler, looper, midix, automix, bandmix, taps, larsen, routes HTTP) | ✅ **25/25 PASS** → **bug /api/meters trouvé et corrigé** |
| P5 | Charge CPU/xrun par action | ✅ **0 xrun, 0 drop dans TOUS les scénarios** |
| P6 | Reboot ×2 (persistance complète) | ✅ après **fix restore TAC** (bug majeur trouvé) |
| P7 | Rapport + checklist utilisateur | ce document |

## Bugs trouvés par la campagne (tous corrigés + validés board)

### 1. Sends (départs FX) perdus au restart — `6ed2652f`
La matrice `send_target` n'était **ni sauvée ni rechargée** dans mixer_state,
et `set_send` ne marquait pas l'état dirty. Tous les départs FX étaient perdus
à chaque restart/reboot. Fix : lignes `sends <src> <8 gains>` en fin de
mixer_state (rétro-compatible), parsées par `load_mixer_state` **et**
`scene_apply` ; `set_send` marque dirty. Validé : 0.25 survit au restart.

### 2. `/api/meters` tronqué à 2 Ko — `7107a46d`
Avec les 4 taps analyzer actifs, `get_meters` dépasse 2 Ko → JSON invalide
sur la route utilisée par les VU de la page SCÈNE. Buffer 2 Ko → 8 Ko.

### 3. Restore TAC auto-destructeur au reboot — `cf1598a9` (MAJEUR)
Chaîne fatale au boot : restore udev de la carte TAC **échoue** (exit 99,
carte/DSP pas prêts) → le restore d'`ala-fx-restore.sh` échouait aussi en
silence (sortie jetée) → le store débouncé de gui-http **écrasait
asound.state avec l'état post-tac-reset**. Conséquence : '3 Biquads/Ch',
EQ TAC et tout réglage codec **perdus à chaque reboot, définitivement**.
Fix : copie de sûreté `asound.state.boot` + retry ×5 avec **vérification
d'un contrôle témoin** + sorties journalisées. Validé sur reboot réel :
`fx-restore: alsactl vérifié (essai 1)`, 4 TACs à 3/Ch après boot.

### Corollaire découvert
**La numérotation des cartes ALSA change d'un boot à l'autre** (softac5212tdm
vue carte 4 puis carte 5 ; ADCIn a permuté). Tout accès doit se faire par
NOM (`-c softac5212tdm`), jamais par index. Les scripts/services livrés le
font ; à garder en tête pour tout diagnostic manuel (`amixer -c 4` ≠ fiable).

## P1 — Santé de base (baseline avant campagne)

- Services : mixer-pro, mixer-gui-http, mixer-console, midi-expander,
  anti-larsen, irq-prio-rt — tous `active`, `NRestarts=0` après 1j05h uptime.
- Journaux prio ≥ warning : **vides** (sauf 3 « Failed » mixer-console =
  redéploiements du jour, et dmesg « debugfs DSP D3 » connus/bénins).
- Threads : mixer-pro RT ~28 % d'un core, QSGRenderThread (console) ~28 %
  nice 10, mémoire 619 Mo/3,6 Go.
- `get_state` : latence 8 ms one-way, prof_cap 1201 µs (attente bloquante),
  prof_mix 452 µs, prof_play 10 µs.

## P2 — API mixer-pro (40 PASS / 0 FAIL, xrun Δ=0)

- **23 ops get** : réponse `ok:true` + champ attendu.
- **11 round-trips set** non destructifs (écrire → relire → comparer →
  restaurer → relire) : mute, input_gain, output_gain, tap, expander,
  comp, vfocus, automix_cfg, insert_bypass (MASTERING), send, bandmix_live.
- **6 tests de robustesse** : src hors bornes, kind invalide, arg manquant,
  slot invalide, scène vide, op inconnue → tous refusés proprement
  (`ok:false`), aucun crash, aucun xrun.

Pièges d'API documentés au passage : gains **linéaires** partout
(`gain`, `gains`×1000) ; `set_insert_bypass` prend `on` (sémantique
MASTERING) ; `set_automix` = adhésion par tranche, enable global =
`set_automix_cfg` ; fin d'enregistrement looper = action `play`.

## P3 — Persistance (après fix sends : 14/14)

- Restart mixer-pro : mute, gain, send, out_gain, expander, vfocus,
  automix résistants (targets → slew sans clic au reload).
- Scène complète (slot 5 temporaire) : save → modification → recall →
  valeurs revenues, nom OK. Slot nettoyé en fin de campagne.
- xrun après restart : 2 (transitoires d'init connus, stable ensuite).

## P4 — Fonctionnel (25 PASS / 0 FAIL, xrun Δ=0)

- **Sampler** : trigger → playing=1, position avance, stop, reload sans
  perte du slot.
- **Looper** : rec piste 5 (silence) → play fige la boucle (1,51 s),
  play_all/stop_all, clear → piste vide + horloge maître réinitialisée.
- **Midix/synthé** : get + proxy ctl status vers midi-expander OK.
- **Automix** : enable global on/off + adhésion par tranche round-trip.
- **Bandmix** : status, refus propre de measure sans src, cancel OK.
- **Taps** : 4 taps simultanés actifs et cohérents, libération + retour
  du tap 3 master.
- **Anti-larsen** : GET status OK (`enable` + notches actifs).
- Routes HTTP : /api/state, /api/sysload, /api/meters (après fix), /api/drift.

## P5 — Charge CPU / xrun par action

| Scénario | cpu0 | cpu1 | cpu2 | cpu3 | prof_iter | xrun Δ | drops Δ |
|---|---|---|---|---|---|---|---|
| idle | 50 % | 34 % | 41 % | 11 % | 2212 µs | 0 | 0 |
| 4 taps analyzer | 54 % | 34 % | 41 % | 12 % | 1702 µs | 0 | 0 |
| sampler 2,5 Hz | 54 % | 33 % | 42 % | 11 % | 1712 µs | 0 | 0 |
| looper 3 pistes | 62 % | 32 % | 38 % | 16 % | 2604 µs | 0 | 0 |
| automix 8 membres | 54 % | 33 % | 29 % | 14 % | 2237 µs | 0 | 0 |
| voix devant ON | 55 % | 31 % | 10 % | 16 % | 2232 µs | 0 | 0 |
| 4× get_meters 10 Hz | 63 % | 49 % | 1 % | 14 % | 1709 µs | 0 | 0 |
| **COMBINÉ pire cas** | 59 % | 39 % | 1 % | 12 % | 2465 µs | **0** | **0** |

Notes : `prof_iter` inclut l'attente bloquante de capture (~1200 µs) — le
travail réel mix+play reste ≈ 500 µs sur le budget de 2000 µs (marge ×4).
La mesure /proc/stat des cores isolés 2-3 (NO_HZ) est indicative. Aucune
casse audio sous stress API, y compris le scénario combiné.

## P8 — TOUT ACTIVÉ (60 s, ajouté à la demande)

Simultanément : **gates ON ×16 + comps ON ×16 + EQ TAC 3 bandes ×8 mics
(24 blobs) + automix Dugan 8 membres + voix devant + bandmix live +
mastering + 4 taps FFT + looper 6 pistes en lecture + sampler 0,8 s +
4 clients GUI get_meters 10 Hz** :

| Fenêtre | cpu0 | cpu1 | cpu2 | cpu3 |
|---|---|---|---|---|
| 0-20 s | 81 % | 61 % | 5 % | 13 % |
| 20-40 s | 81 % | 62 % | 27 % | 14 % |
| 40-60 s | 82 % | 62 % | 33 % | 14 % |

- **xrun Δ = 0, drops Δ = 0** sur les 60 s.
- prof audio : cap 1461 µs (attente) + **mix 513 µs** + play 12 µs —
  tout le traitement additionnel (gates+comps+automix+vfocus+looper+
  sampler) ne coûte que **~60 µs de plus** que l'idle (452 µs). Marge
  restante ≈ ×3,8 sur le budget 2 ms.
- Restauration intégrale vérifiée : 16 gates, 16 comps, 24 blobs EQ
  (comparaison octet à octet), automix/vfocus/taps/looper/sampler.

Script : `tools/validation/validate_p8_full.py`.

## P6 — Reboot (×2)

- Reboot 1 : services ✓, scènes ✓, mixer_state (sends incl.) ✓, xrun 0…
  mais **config TAC perdue** → bug 3 ci-dessus, corrigé.
- Reboot 2 (avec fix) : `fx-restore: alsactl vérifié (essai 1)` au journal,
  4 TACs à '3 Biquads/Ch', services ✓, scènes ✓, xrun 3 (init, connu).
- Slot shift SAI RX : **non vérifiable sans signal micro** → checklist.

## Binaires déployés en fin de campagne

| Fichier | md5 |
|---|---|
| /usr/bin/mixer-pro | `d02ca7d1f14d3ecfc1d84a541fb19c37` |
| /usr/bin/mixer-gui-http | `3380c6599c58dabee3b30ba4b8bd7096` |
| /usr/bin/ala-fx-restore.sh | script `cf1598a9` |
| /usr/bin/mixer-console | `84241d918bbd520da7cba88b4b527580` |

## CHECKLIST TESTS UTILISATEUR (oreilles/doigts requis)

Ce que l'automatisation ne peut PAS vérifier depuis le PC :

1. **Slot shift SAI RX** : au boot, parler dans mic 1/2 → les VU M1/M2
   doivent bouger (pas M7/M8). Si décalé : restart mixer-pro (workaround
   connu, fix durable = sprint priming RX).
2. **Qualité audio générale** : son propre sur PA après la campagne
   (la campagne n'a rien changé aux réglages : état restauré).
3. **EQ TAC audible** : tirer une bande sur M1 en parlant → FFT + oreille.
4. **Looper micro réel** : enregistrer une vraie boucle (la campagne n'a
   enregistré que du silence), overdub 2ᵉ piste alignée.
5. **PADS** : déclenchement au toucher, latence perçue.
6. **Synthé M1 via MIDI** : notes depuis le clavier USB (l'injection MIDI
   n'est pas automatisable à distance), patches, PANIC.
7. **Anti-larsen** : monter le gain jusqu'au larsen → notch en < 1 s
   (bouton SCÈNE ON/OFF).
8. **Scènes** : recall « Test A »/« Concert » depuis la page SCÈNE →
   changement instantané et complet (faders + TAC + synthé).
9. **Voix devant + Automix musique** : test d'écoute avec stems.
10. **Interfaces** : drag fluide des poignées EQ au doigt (LCD) et à la
    souris (web), VU des gros boutons SCÈNE animés.

## Restes connus (hors périmètre campagne)

- Boost > 0 dB impossible dans les biquads TAC (format Q1.31) — option
  EQ logiciel mixer-pro si besoin.
- Slot shift SAI RX au boot (workaround restart) — sprint SOF dédié.
- tar hôte Ubuntu cassé pour pseudo — workaround en place (voir mémoire
  `host_tar_pseudo_fix`), à refaire si `tmp/` est réinitialisé.
