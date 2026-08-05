# TESTS V16 — VOICE CLEAN : bouton BRUT/DTLN/GTCRN/SPECSUB sur la voie voix

Date : 2026-08-06. ARCHI_V16_VOICE_CLEAN.md (validée : CPU, bouton 4
positions, CPU3). `MIXER_VERSION` = v16.0-voice-clean. But : Michael
écoute les traitements DANS la musique et en live.

## Livré (fonctionnalité complète)

1. **Moteur** : module `voice_clean.c/h` + `voice_clean_shm.h` (source
   unique du layout SHM). Push [voix, réf musique] après le gate, retour
   traité REMPLACE la voie avant tout consommateur (anti-larsen, automix,
   mesures, mix). Amorçage 12 périodes (marge jitter), famine = silence +
   compteur + ré-amorçage. Ops `voice_clean {src,mode}` (refus explicites :
   voie non-voix, mode non chargé) + `voice_clean_status`. BRUT au boot
   (jamais persisté). Ring 8192 frames (2^n) + compteurs LIBRES uint32.
2. **Daemon `voice-clean`** (nouvelle recette, image) : CPU3 SCHED_FIFO 60
   (sous UAC2 95), mlockall. DTLN = TFLite C API 1 thread + resampler
   polyphase 48↔16 k maison ; SPECSUB = soustraction spectrale de
   puissance 48 k native (α par bande appris quand la voix se tait) ;
   GTCRN = absent (modes_avail), 3 chemins de conversion en échec
   (onnx2tf ×2 : graphe invalide/MatMul ; litert-torch : ConvTranspose
   groupée non supportée) — sprint export dédié si souhaité.
3. **GUI web** : bouton cyclique sur les lignes lead/chœurs de la page
   AUTO MIX (BRUT→DTLN→SPEC, vert quand actif, tooltip latence).
4. **LCD** (commit c1592355) : même bouton sur la page AUTO MIX de la
   console Qt6, mêmes ops, GTCRN sauté via modes_avail. Déployé board
   (md5 ff67bfc6), 0 erreur QML — validation visuelle/tactile : Michael.

## Validation board

| Test | Résultat |
|---|---|
| Refus voie non voix (src 8 kick) | « pas une voie voix » ✓ |
| Refus GTCRN | « mode 2 indisponible (daemon) » ✓ |
| DTLN 16 s en morceau | **famines 0**, signal traité au VU, xrun 0 ✓ |
| SPECSUB 10 s | famines 0, xrun 0 ✓ |
| xruns UAC2 (CPU3 partagé) | **0/0** ✓ |
| Charge CPU3 daemon | ~12,5 % (DTLN) ✓ |
| Débit daemon instrumenté | tx=rx (delta = latence en vol constante), hop_max 9 ms absorbé par l'amorçage ✓ |
| GUI E2E | clic → moteur mode 1 → label DTLN → clic → SPEC (cycle saute GTCRN) ✓, 0 erreur console |

## Bugs trouvés/corrigés PENDANT la campagne (consignés)

1. **Ring non-2^n + indices modulo** : `(wr−rd) % 6144` casse au wrap
   (2³² mod 6144 = 4096) → relectures corrompues, 75-86 famines. Règle
   gravée dans voice_clean_shm.h : ring 2^n + compteurs libres, TOUJOURS.
2. **Amorçage manquant** (latence fixe spécifiée non implémentée) →
   famines de jitter. VC_PRIME 12 périodes.
3. **GUI ×3** : ancres `str.replace` sans assert (bmxStrip/bmxPoll
   inexistants sous cette forme) → `vcSt` jamais défini, page AUTO MIX
   morte ; handler niché dans le handler solo ; gate de poll sur
   « AUTOMIX » alors que la page s'appelle « BANDMIX ». Leçon : TOUT
   replace JS a son assert, et vérité = test navigateur.

## Test utilisateur : REQUIS — c'est le BUT de la fonctionnalité

Protocole d'écoute (page AUTO MIX, morceau au choix, voie U7) :
1. BRUT → DTLN : juger la disparition de repisse, l'état du chant, et le
   RETARD de 48 ms de la voix sur le groupe (assumé, R&D) ;
2. DTLN → SPEC : comparer le caractère (SPECSUB = plus doux, apprend la
   salle en quelques secondes de jeu sans chant) ;
3. familles → silence sur la voie = famine (voir voice_clean_status) —
   à signaler si entendu.
En live réel : mêmes essais avec vrais micros (la réf musique = les
autres tranches actives).
