# TESTS V14.0 — Étape 8 : StripFxDrawer.qml → 3 délégués extraits

Date : 2026-08-02. Dernier chantier de la restructuration V14.0.

`StripFxDrawer.qml` 1 093 → **818** lignes. Les 3 délégués `Component`
inline extraits :

| Fichier | Lignes | Contenu |
|---|---:|---|
| `FxPlainRow.qml` | 70 | contrôle simple (INTEGER→knob dB, BOOLEAN→switch, ENUM→cycle) |
| `FxBqRow.qml` | 54 | ligne biquad RBJ (type + freq + Q + gain → blob TAC) |
| `FxDrcCard.qml` | 168 | carte DRC / MULTIBAND (blob SOF, sliders par bande) |

**Pattern wrapper** : le drawer garde `Component { id: plainRow;
FxPlainRow {} }` — la chaîne de contextes QML est INCHANGÉE : `ctl`
(propriété du Loader délégué) et `drawer` (id du document parent) se
résolvent exactement comme avant. Aucun site d'usage modifié.

## Validation

| Test | Résultat |
|---|---|
| Équilibre accolades des 4 fichiers (2 incidents de découpe attrapés : `}` racine manquante, `}` Component en trop dans FxDrcCard) | ✓ |
| Build bitbake : **qmlcachegen compile les 4 QML** (vraie validation syntaxe/résolution statique) | 0 erreur ✓ |
| Déploiement board + restart mixer-console | actif, **zéro erreur QML au journal** (« cursor DSI1 » = message eglfs bénin habituel) ✓ |

## Test utilisateur : **REQUIS** (les délégués ne s'instancient qu'à
l'ouverture du drawer au TACTILE — hors de portée à distance)

À vérifier sur le LCD : ouvrir le drawer FX d'une tranche →
1. onglet contrôles simples : knobs dB / switches / enums réagissent ;
2. BIQUADS BRUTS : lignes biquad (type/freq/Q/gain) fonctionnelles ;
3. carte DRC / MULTIBAND : sliders par bande + apply.
Toute erreur d'instanciation apparaîtrait dans
`journalctl -u mixer-console` au moment de l'ouverture.

## Chantier V14.0 : TERMINÉ (8 étapes)

Restent ouverts : arbitrage `beta_fx.js` 1 125 (closure unique, fiche
étape 7) ; passe 2 = audit de re-privatisation des externs ; écoute de
validation globale (8 fiches EN ATTENTE).
