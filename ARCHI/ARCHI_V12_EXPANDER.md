# ARCHI V12-EXP — Expandeur/gate par tranche (natif mixer-pro)

Date : 2026-07-06 · Priorité 3 roadmap V12 (dernière fonction du sprint).

## Objectif

Downward expander / noise gate **par tranche d'entrée** (16 voies réelles :
M1..M8 + USB1..USB8) : sous le seuil, le signal est atténué progressivement
(ratio) jusqu'à un plancher (range). Usage live : fermer les micros qui ne
parlent pas (bleed, larsen, bruit de fond) — synergie directe avec
l'automix Dugan (mic gaté = énergie nulle → part redistribuée) et
l'anti-larsen.

## Placement dans la chaîne

`exp_render(in_block)` dans l'audio_thread, SOUS target_lock, **juste après
le convert S32→float** et AVANT smp_render/loop_render/automix_update/
mix_block. Traitement **in-place** de in_block[0..15] :

- le gate s'applique à TOUT l'aval (sends FX, master, looper qui enregistre
  post-fader, automix qui mesure l'énergie, tap NPU) — sémantique console
  standard (dynamique de tranche pré-fader) ;
- P1/P2 (16/17, sources internes sampleur/looper) exclues — un gate sur un
  sample n'a pas de sens.

## Paramètres par tranche

| Param | Plage | Défaut | Rôle |
|---|---|---|---|
| on | 0/1 | 0 | bypass total (zéro coût si off) |
| threshold_db | −80..0 | −50 | seuil d'ouverture |
| ratio | 1..20 | 3 | pente d'expansion (élevé = gate dur) |
| attack_ms | 0.5..100 | 5 | ouverture (plancher effectif 2 ms = 1 bloc) |
| release_ms | 5..1000 | 150 | fermeture |
| range_db | 0..80 | 40 | atténuation max (plancher) |
| hold_ms | 0..500 | 50 | maintien ouvert après passage sous le seuil (anti-chatter) |

## Algorithme (par bloc de 96 frames = 2 ms)

```
pour chaque tranche i ∈ [0..15] avec exp.on :
  1. crête du bloc : p = max|in_block[i][f]|
  2. enveloppe : env += (p > env ? ka : kr) × (p − env)
     ka/kr précalculés à la config (exp(−2ms/τ)), PAS d'expf par bloc
  3. hold : si env ≥ thr_lin → hold_cnt = hold_blocks ;
            sinon si hold_cnt > 0 → hold_cnt−− (gain reste ouvert)
  4. gain cible (dB) : env ≥ thr ou hold actif → 0 dB
     sinon g_db = (env_db − thr_db) × (ratio − 1), clampé à −range_db
     (1 log10f + 1 powf par tranche active par bloc — négligeable)
  5. application : rampe LINÉAIRE de gain_prev → gain sur les 96 frames
     (zipper-free), in_block[i][f] ×= g(f)
  6. GR meter : gr_db publié (atomic float) pour la GUI
```

Coût RT : 16 × (96 MAC + ~2 transcendantes) / 2 ms — négligeable face au
mix 34×18. Tranches off : `continue` immédiat.

## Structures

```c
struct exp_ch {
    int   on;
    float thr_db, ratio, range_db;     /* config user */
    float thr_lin;                     /* précalc */
    float ka, kr;                      /* coefs attack/release précalc */
    int   hold_blocks;                 /* précalc depuis hold_ms */
    /* état audio */
    float env, gain;                   /* enveloppe crête, gain courant lin */
    int   hold_cnt;
    _Atomic uint32_t gr_mdb;           /* gain reduction publié (milli-dB, GUI) */
};
static struct exp_ch g_exp[16];
```

Config écrite par le control thread SOUS target_lock (comme input_target)
→ pas de déchirure : l'audio lit la config au bloc suivant. Les précalculs
(thr_lin, ka, kr, hold_blocks) sont faits dans le handler socket, jamais
en RT.

## Ops socket

- `set_expander {src, on?, threshold_db?, ratio?, attack_ms?, release_ms?,
  range_db?, hold_ms?}` — updates partiels, précalculs dans le handler.
- `get_expander` → `{tracks:[{src,on,threshold_db,ratio,attack_ms,
  release_ms,range_db,hold_ms,gr_db}...]}` (gr_db = réduction courante,
  pour l'affichage temps réel).

## Persistance

`mixer_state` : lignes `expander <src> <on> <thr> <ratio> <atk> <rel>
<range> <hold>` (16 lignes) dans save/load_mixer_state, même mécanisme
`g_presets_dirty` que l'automix. Reset usine → fichier purgé → défauts.

## GUI (E1b, après validation moteur)

Onglet **GATE** dans le StripFxDrawer (drawer d'effets par tranche
existant, natif) : interrupteur ON, sliders threshold/ratio/attack/
release/range/hold + **barre GR temps réel** (poll get_expander quand le
drawer est ouvert). Web = E2.

## Tests E1 (tone USB, mesures meters)

1. Tone −12 dBFS sur USB1, gate off → in[8] = 25 % FS (référence).
2. Gate on thr=−6 dB (au-dessus du signal), ratio 20, range 60 →
   in[8] chute vers ~0 (fermé) ; gr_db ≈ −60 dans get_expander.
3. thr=−20 dB (sous le signal) → in[8] revient à 25 % (ouvert, gr=0).
4. Tone pulsé (bursts) : vérifier ouverture < 10 ms et fermeture douce
   (release) sur les meters + hold anti-chatter.
5. Persistance : set_expander, restart mixer-pro, get_expander = config.
6. Delta xruns 0 avec 16 gates on.

## Invariants

- In-place sur in_block AVANT tout consommateur (sends/master/looper/
  automix/tap) — une seule vérité du signal de tranche.
- Zéro alloc/transcendante superflue en RT ; précalculs au set.
- off = zéro coût (pas de scan crête).
- Rampe de gain intra-bloc obligatoire (pas de zipper).
