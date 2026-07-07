# ARCHI V10-NATIVE — Console écran en app native Qt6/QML (sans wayland)

Date : 2026-07-05 · Décision utilisateur : « réécrire une interface graphique
complète native, les CPU sont trop élevés » + « éviter de démarrer wayland
complètement ».

## 1. Constat mesuré (TESTS_V10_P4a + session P4)

- Kiosk Chromium : cores 0-1 à **85-93 %** en continu (0 xrun grâce aux
  priorités, mais conso/chaleur inacceptables pour un produit).
- weston : ~40-60 Mo + compositing permanent, uniquement pour porter chromium.
- Le GPU Vivante est inutilisé aujourd'hui.

## 2. Décision

- **App native `mixer-console` en Qt6/QML sur plateforme `eglfs`** :
  plein écran direct DRM/KMS, rendu GPU Vivante (scenegraph), AUCUN
  compositeur. `packagegroup-qt6-imx` est déjà dans l'image.
- weston.service et mixer-kiosk (chromium) **désactivés du boot** ;
  chromium/weston restent installés en phase de transition (fallback debug),
  retrait de l'image quand la native est validée.
- **La GUI web /beta reste** pour l'accès PC/tablette (serveur mixer-gui-http
  inchangé). L'app native est LA surface écran.

## 3. Cibles chiffrées (critères GO/NO-GO de la V1)

- CPU : **≤ 15 % d'un core** (moyenne, page MIXER animée 60 fps, lecture en
  cours) — mesure `top` 60 s comme pour chromium.
- RAM (RSS) : ≤ 120 Mo.
- Xruns : 0 sur 3×60 s de lecture (protocole identique TESTS_V10_P4a).
- Latence tactile→fader : perçue immédiate (< 50 ms).
- Cold boot → console affichée : ≤ 25 s (pas pire que chromium).

## 4. Architecture app

```
mixer-console (C++/QML, un binaire)
├── MixerClient (C++) : socket Unix /run/mixer-pro.sock DIRECT
│    · commandes JSON existantes (set_master, set_send, set_fx_*, …)
│    · polling get_meters 30 Hz + get_insert/get_assistant 5 Hz + froid 1 Hz
│      (mêmes cadences que le producteur SSE — pas de nouveau protocole)
│    · exposé à QML via propriétés/signaux (types C++, pas de JSON en QML)
├── QML scenegraph (GPU) :
│    · thème = tokens de la maquette validée (anthracite #14181c, ambre
│      #e5a13c, mono tabulaire)
│    · composants : Knob (Canvas/Shape), Fader tactile, MeterBar (30 Hz),
│      VUNeedle (aiguilles), Spectrum+Waterfall (QQuickPaintedItem ou
│      ShaderEffect), Toggle, EnumSelect, Card, TabBar, BankBar
│    · pages : MIXER (banques 8 tranches + master), EFFETS (rack get_fx),
│      MASTERING, ROUTING, SYSTÈME, drawer EFFETS PISTE (ALSA via
│      /api/alsa/* de mixer-gui-http en HTTP localhost — réutilise la
│      logique blobs DRC déjà éprouvée, pas de réécriture libasound)
├── rotation : écran portrait 800x1280 → QT_QPA_EGLFS_ROTATION=90 (paysage)
├── tactile : GT911 via libinput (QT_QPA_EGLFS_ALWAYS_SET_MODE, evdev auto)
└── service systemd mixer-console : After=mixer-pro, Restart=on-failure,
    CPUAffinity=0-1, Nice=10 (le GPU fait le gros du travail)
```

## 5. Phasage (validation board à chaque étape)

- **N0** : recette Yocto squelette + fenêtre eglfs plein écran tournée,
  fond châssis, FPS overlay — mesure CPU à vide. GO/NO-GO eglfs/Vivante.
- **N1** : MixerClient (socket, meters 30 Hz) + page MIXER (8 tranches
  banque IN DSP + master VU aiguilles) — mesure CPU/xruns → décision
  « chromium éteint au boot ».
- **N2** : banques complètes + sends + mute + master fader (parité MIXER).
- **N3** : SPECTRE + waterfall (tap analyzer 3) + enveloppe ML.
- **N4** : EFFETS (rack LV2 dynamique), MASTERING, ROUTING, SYSTÈME.
- **N5** : drawer effets piste (réutilise /api/alsa + /api/dsp/blob de
  mixer-gui-http en localhost).
- **N6** : image finale sans weston au boot ; fiche TESTS + écoute.

## 6. Risques

- eglfs + Vivante sur ce BSP : à valider à N0 (le plugin qt6 imx
  `eglfs_viv` est fourni par meta-qt6/NXP ; sinon fallback eglfs_kms).
- Deux clients socket mixer-pro (app native + mixer-gui-http) : déjà
  supporté (pool de sockets côté gui-http, protocole sans état).
- Le drawer ALSA dépend de mixer-gui-http : dépendance actée (le service
  HTTP reste au boot pour la GUI PC de toute façon).
- Rotation eglfs : QT_QPA_EGLFS_ROTATION tourne la SORTIE ; le tactile
  suit via le mapping evdev de Qt (à valider à N0 avec le GT911).

## 7. Ce qui ne change PAS

- mixer-pro, daemon ML, DSP, kernel : intouchés.
- mixer-gui-http + /beta : la surface PC.
- Le design validé : mêmes tokens, mêmes écrans — en natif.
