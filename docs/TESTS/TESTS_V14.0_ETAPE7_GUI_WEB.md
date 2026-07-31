# TESTS V14.0 — Étape 7 : beta.html → HTML + CSS + 5 JS locaux

Date : 2026-07-31. `beta.html` (4 100 lignes, JS inline 3 237) → fichiers
locaux servis par la route `/static/` existante (zéro CDN inchangé, aucun
changement C).

| Fichier | Lignes | Contenu |
|---|---:|---|
| `beta.html` | 478 | structure seule + `<link>` + 5 `<script src>` |
| `beta.css` | 355 | styles |
| `js/beta_core.js` | 761 | état + construction UI + **holdify (réparée)** |
| `js/beta_bridge.js` | 268 | LIVE BRIDGE (SSE état + ops) |
| `js/beta_pages.js` | 487 | pages EFFETS + MASTERING/ROUTING/SYSTÈME |
| `js/beta_fx.js` | **1 125** | drawer effets piste + EQ pro — voir arbitrage |
| `js/beta_perf.js` | 649 | PADS / LOOPER / EXPANDEUR |

Méthode : tranches CONTIGUËS aux frontières d'IIFE top-level (appariement
d'accolades vérifié) → ordre de chargement = ordre d'origine, sémantique
identique au script unique. Analyse préalable : aucun appel top-level vers
une définition d'une tranche ultérieure. `node --check` sur chaque fichier.

## BUG PRÉEXISTANT découvert et corrigé

`function holdify(...)` (appui long V13.2, scènes/pads) était définie **dans
le bloc `<style>`** de beta.html (ligne 223, en plein CSS) — le navigateur la
jetait : `ReferenceError` au chargement, boutons SAUVER/RAPPEL de scènes et
clear des pads web cassés DEPUIS V13.2. Exposé par le test navigateur du
découpage ; fonction déplacée dans `beta_core.js` (corps identique).

## ARBITRAGE DEMANDÉ : `beta_fx.js` à 1 125 lignes

Le bloc « effets par piste + EQ paramétrique PRO » est UNE SEULE closure
(IIFE 2339–3452) au couplage bidirectionnel massif (applyBlob, loadBlob,
blobs, groupsFor… partagés dans les deux sens). La scinder = chirurgie de
scope sur une GUI validée, PAS une extraction pure. Options : (a) exception
documentée en attendant un sprint refactor dédié ; (b) GO pour la scission
avec re-validation navigateur complète du drawer + EQ. **Michael tranche.**

## Validation navigateur réel (Chrome, LAN → 192.168.0.198:8080)

| Test | Résultat |
|---|---|
| Chargement / (tous assets /static/ en 200) | ✓ |
| Console JS | **0 erreur** après fix holdify (1re erreur = cache navigateur, levée au hard-reload) ✓ |
| Page MIXER | 8 tranches, VU, master, mastering ML rendus ✓ |
| Page AUTO MIX | rôles/keepers live (GR.CAISSE −1.2 … VOIX LEAD +0.7), EQ master, BALANCE (−14/+3/+1.5), tous panneaux ✓ |
| Page SCÈNE | 6 slots dont « v14test » (étape 2e), automatismes, boutons holdify rendus ✓ |
| Page EFFETS | catalogue LV2 complet (Calf/B.*…), FX1-4, catégories ✓ |
| Recette | packaging static/ + js/ OK ✓ |

## Test utilisateur : EN ATTENTE (parcours complet + appui long scènes à
re-valider — il était cassé avant, il doit marcher maintenant)
