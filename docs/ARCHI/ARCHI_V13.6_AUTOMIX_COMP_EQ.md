# ARCHI V13.6 — AUTOMIX LIVE : compresseur + EQ par voix

**Objectif** (utilisateur 2026-07-11, priorités « 2 et 3 ») : l'AUTOMIX LIVE
ne doit pas seulement NIVELER, mais aussi **compresser** et **égaliser**
chaque voie pour *placer* les instruments — comme un ingé son. Les voies
USB n'avaient ni comp actif ni EQ (l'EQ des mics = TAC matériel, entrée
micro seule). Répond aux points : trop-fort atténué vite, mémoire du
risque, voix qui se distingue.

## (2) Compresseur auto par rôle — seuil ADAPTATIF

En AUTOMIX LIVE, chaque voie active (rôle ≠ off) reçoit le compresseur de
son rôle (presets BMX_P déjà présents : ratio/attaque/release), mais avec
un **seuil relatif au loudness courant de la source**, pas absolu :

    thr_db = al_ref[i] + COMP_OFFSET[role]

- `al_ref` = peak-hold lissé du loudness pré-fader (déjà maintenu par
  l'automix) → **c'est la mémoire du risque** : une source souvent forte a
  un al_ref haut, son comp la tient sur la crête. Le comp agit au sample
  (attaque rapide) → **atténuation immédiate** du trop-fort, mieux que le
  niveleur à 1 Hz.
- COMP_OFFSET par rôle (dB au-dessus du loudness moyen) : lead −2 (tient la
  voix serrée), kick/snare/drums +2/+3 (crêtes seules), bass −3, guitar/
  keys −1. Ratio/attaque/release = presets de rôle.
- Le comp tourne PRÉ-keeper (chaîne gate→comp→vfocus→…→mix×keeper) : c'est
  pourquoi le seuil doit être relatif au niveau BRUT de la source, sinon un
  seuil absolu (−18 dBFS) ne mordrait que les stems déjà chauds.
- Recalculé à chaque tick (1 Hz) sous target_lock. À l'arrêt de l'AUTOMIX
  LIVE : comps des voies à rôle remis à off (état propre).

## (3) EQ logiciel par voix (eqx) + creusement voix renforcé

Nouveau bloc DSP `eqx` : cascade de 2 biquads RBJ **par tranche** (0-15),
coefs par RÔLE, états par tranche (comme vfocus). Appliqué **entre gate et
comp** (HPF avant compression). Coût : 32 biquads/sample = négligeable
NEON. Coefs recalculés seulement au changement de rôle.

Presets de rôle (HPF + 1 cloche de placement) :

| Rôle | HPF | Cloche |
|---|---|---|
| lead | 90 Hz | +3 dB @ 3,5 kHz (présence, la voix ressort) |
| choir | 120 | +1,5 @ 4 k |
| kick | — | +2 @ 70 (poids) |
| snare | 120 | +2 @ 4 k (claquant) |
| drums | 200 | +1 @ 6 k (air) |
| bass | 30 | −2 @ 3,5 k (dégage la voix) |
| guitar | 120 | **−3 @ 3 k** (creuse pour la voix) |
| keys | 120 | **−3 @ 3 k** (creuse pour la voix) |
| line | 80 | — |

→ HPF nettoie la boue, la voix gagne en présence, guitare/claviers sont
creusés dans la bande de la voix : **la voix se distingue** même sans
chanter fort. Statique (place), complété par le **vfocus dynamique**
(creuse plus quand la voix chante) — AUTOMIX LIVE allume vfocus d'office,
amount renforcé (0,7, max_cut 6 dB).

Appliqué uniquement quand AUTOMIX LIVE actif (sinon EQ neutre — les voies
gardent leur réglage manuel). À l'arrêt : états EQ vidés.

## Invariants

1. Comp + EQ auto UNIQUEMENT sous AUTOMIX LIVE ; hors mode, réglages
   manuels intacts.
2. eqx entre gate et comp ; vfocus après comp (inchangé).
3. Coefs eqx recalculés au changement de rôle seulement (pas par bloc).
4. Zéro alloc / zéro transcendante par sample dans l'audio (coefs
   précalculés, biquads en forme II transposée comme vfocus).
5. Le point (1) — asymétrie/gel du niveleur — reste TODO (le comp adaptatif
   couvre déjà « trop-fort » et « mémoire »).

## Test (à la reprise de la carte)

Jouer un stem 8 pistes déséquilibré, AUTOMIX LIVE ON :
- la voix doit ressortir (présence + creusement guitare/keys) ;
- une trompette qui s'arrête ne doit plus être boostée (comp + gel) ;
- un instrument qui saute doit être rattrapé vite (comp attaque rapide) ;
- réglages par oreille : COMP_OFFSET, presets EQ, vfocus amount.
