# ARCHI V13.5 — AUTOMIX LIVE (bandmix continu, sans réglage)

**Objectif** (utilisateur 2026-07-11) : « débrouille-toi pour que tout soit
automatique et tout le temps, c'est un automix en live ! ». Le bandmix
actuel exige mesure → calcule → verrouille → live (un mix préparé à la
main puis tenu par un trim ±3 dB). On veut un **vrai automix continu** :
un seul interrupteur, aucun soundcheck, aucun verrouillage, il équilibre
et tient le mix en permanence.

## Principe

La **courbe de niveaux par rôle** existe déjà (`BMX_P[].mix_db` : lead 0 dB,
choir −4, kick −2, snare −3, drums −6, bass −3, guitar −5, keys −5, line −5).
Elle définit une **part-cible de loudness par rôle**. L'automix live tire en
continu la part de chaque source vers sa part-cible, via `keeper_gain`.

- **Référence = la courbe de rôle** (calculée en continu, normalisée sur les
  sources ACTIVES), pas un instantané verrouillé.
- **Autorité large** : ±24 dB (contre ±3 dB du keeper « trim ») — il faut
  équilibrer des stems bruts qui diffèrent de 15-20 dB, faders à l'unité.
- **Contrôle proportionnel** doux : step = clamp(−0,45·err_db, ±2 dB)/tick,
  zone morte 0,8 dB. Convergence ~15-25 s puis maintien, sans pompage
  (mesure loudness EWMA 3 s → lt 10 s, tick 1 Hz).
- **Détection de silence auto** (sans soundcheck) : peak-hold lent par source
  `al_ref` (décroît 0,5 dB/s) ; source active si son loudness pré-fader est à
  moins de 25 dB de son propre peak (et > −60 dBFS). Un instrument qui se tait
  sort de la normalisation (pas de trou), revient quand il rejoue.
- **Boucle fermée** : lt_ms inclut déjà `keeper_gain` → le correcteur voit
  l'effet de ses corrections, pas de dérive aux butées.

## Répartition

| Aspect | Qui |
|---|---|
| Correcteur continu | moteur, `bmx_tick()` step 4, branche `autolive` |
| État `autolive` | moteur, op `bandmix_autolive {on}`, persisté mixer_state + scènes |
| Détection silence | moteur (peak-hold `al_ref`, pas de mesure requise) |
| UI | un interrupteur « AUTOMIX LIVE » (page AUTO MIX) + le gros bouton SCÈNE « AUTOMIX → MUSIC » l'active |

## Invariants

1. `autolive` et le keeper « verrouillé » (`live`+`ref_valid`) sont
   **exclusifs** : autolive prioritaire, courbe de rôle.
2. Activer/désactiver ne casse rien : à l'arrêt, retour doux de `keeper` à
   0 dB (comme `bandmix_live off`).
3. Le fader reste au réglage utilisateur ; l'automix agit sur `keeper_gain`
   (trim multiplicatif). En automix live, l'automix **possède l'équilibre** :
   toucher un fader est compensé (comportement automix assumé) — biaiser =
   changer les rôles.
4. N'exige NI mesure NI verrouillage. Fonctionne dès l'activation.
5. Gate/comp par rôle NON appliqués par l'automix live (leveling seul) —
   restent au réglage manuel / bmx_calc si voulu.

## Diagnostic

- `bandmix_status` → nouveau champ `autolive`, et `keeper_db[]` par tranche
  (déjà présent) montre les corrections en direct.
- Test board : jouer un stem 8 pistes déséquilibré → activer autolive →
  les `keeper_db` doivent converger pour rapprocher les parts de la courbe
  de rôle (lead au-dessus, kick/drums/bass calés), et suivre les passages.
