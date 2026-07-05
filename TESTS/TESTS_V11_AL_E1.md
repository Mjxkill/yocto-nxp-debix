# TESTS V11-AL E1 — anti-larsen automatique (daemon AFS)

Date : 2026-07-05 · ARCHI : ARCHI_V11_ANTILARSEN.md (critic 2 itérations)

## Versions

| Artefact | Référence |
|---|---|
| anti-larsen 1.0 | daemon C (recette meta-local/recipes-audio/anti-larsen), déployé board |
| Config | /etc/mixer-pro/anti-larsen.conf — enable=1 board (défaut image : 0) |
| Slots | BQ TAC DAC **7-11** (5/paire) — BQ12 inaccessible (bug driver, cf. bas) |
| Statut | socket /run/anti-larsen.sock (JSON : notchs actifs) |

## Architecture déployée

Tap FX (/dev/imx-audio-tap-out, mmap RO curseur privé) → fenêtres 8192 @
48 kHz (boucle 100 ms, CONTRAINTE ring 170 ms) → gate RMS par canal (pas
de FFT sur le silence) → FFT fftwf par canal actif → détection : seuil
−45 dBFS + PNR > 25 dB + persistance 4 analyses + non-harmonicité 2f/3f →
notch RBJ Q30 −9 dB (approfondi jusqu'à −18), libération 60 s.

## Mesures board (2026-07-05 soir)

| Test | Résultat |
|---|---|
| Daemon actif, socket statut | `{"ok":true,"enable":1,"notches":[]}` ✓ |
| CPU au silence (20 s) | **6 %** d'un core (première version : 60 % → gate RMS + conversion différée) |
| xruns pendant fonctionnement | 0 nouveau ✓ |
| Slots AFS rendus flat au start/stop | ✓ (journal) |

## Bugs trouvés/corrigés pendant E1

1. Parser conf : fscanf s'arrêtait au premier commentaire → fgets+sscanf.
2. socket() : SOCK_NONBLOCK passé en protocole → type.
3. **Driver kernel : TAC5212_MAX_REG=0x7E rejette le dernier octet du BQ12
   (EIO)** — fix apply-tac5212-bq12-maxreg.py (0x7F) pour le prochain
   Image ; en attendant le daemon utilise 5 slots (BQ 7-11). NB : le BQ12
   est aussi cassé pour l'édition manuelle du drawer aujourd'hui.

## Tests à faire (utilisateur, avec HP + micro)

- **E0(a)** : musique → `python3 /root/tests/al_tap_fft.py` → pics cohérents
- **E0(b)** : pose manuelle `al_notch.py set 0 7 1000` pendant musique →
  pas de plop ; `clear 0 7`
- **E0(c)** : tone 1 kHz → `set 0 7 1000` vs `set 0 7 1000 30 --halved` →
  laquelle atténue vraiment ? → fixer coef_halved dans la conf
- **E1 larsen réel** : micro devant HP, monter le gain → larsen →
  suppression < 1 s ? statut : `socat - UNIX-CONNECT:/run/anti-larsen.sock`
  → notch listé à la bonne fréquence ; musique non dégradée ensuite.

## E1b — révision per-channel (datasheet) + VALIDATION UTILISATEUR

Nuit du 2026-07-05→06, tests tone 123 Hz injecté par USB depuis le PC de
dev (paplay → gadget UAC2 → USB IN → OUT 1/2), écoute casque utilisateur.

**Découvertes (datasheet TAC5212 SLASF23A Table 7-48 + A/B à l'oreille) :**
- Allocation biquads DAC MODULO 4 CANAUX (famille TAC5x1x 4ch) : sur
  TAC5212, canal 1 = BQ1/5/9, canal 2 = BQ2/6/10, **BQ3/4/7/8/11/12 =
  canaux inexistants (morts)**. Le plan initial « slots 7-12 » était nul.
- Format coefficients : **N1/D1 divisés par 2** (validé : format plein
  sature D1 des notchs graves → filtre inopérant). coef_halved=1 défaut.
- Écriture à chaud OK (clignotement 5 s/5 s audible), y compris le
  passage 2→3 Biquads/Ch (BQ10 actif sans power-cycle DAC).
- 2e occurrence de l'off-by-one kernel MAX_REG=0x7E : **BQ6 (P16_R108-127)
  EIO comme BQ12** → slots canal B ordonnés {BQ10, BQ6}, fix kernel
  buildé (Image prête, reboot à planifier).

**Daemon E1b** : slots PAR CANAL {5,9}/{10,6}, détection par canal (plus
de max par paire), force '3 Biquads/Ch' au start, BQ1/BQ2 laissés au
panneau BIQUADS utilisateur.

**Validation bout-en-bout** : tone 123 Hz continu = larsen synthétique
(raie pure, PNR>50 dB, non-décroissant, sans harmoniques) → détection ~1 s,
notchs posés ch0=BQ5 / ch1=BQ10, **« ils sont atténués tous les 2 »
(utilisateur, casque)**. Libération auto 60 s après arrêt du tone.

Reste : reboot kernel (BQ6/12), larsen acoustique réel à l'occasion,
E2 (persistance notchs fixes) + E3 (GUI) + slots morts grisés drawer.

## Test utilisateur : VALIDÉ (atténuation entendue sur les 2 canaux)
