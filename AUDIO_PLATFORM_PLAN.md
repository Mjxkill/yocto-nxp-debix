# Plan d'Implémentation — Plateforme Audio i.MX8MP + 4x TAC5212

## Vue d'ensemble

Plateforme audio professionnelle embarquée sur i.MX8MP (Debix Model AB) avec :
- 8 entrées analogiques (XLR) + 8 micros PDM (sélectionnables par canal)
- 8 sorties analogiques (XLR)
- USB Gadget composite : Audio 8x8 (PC) + Audio 2x2 (Téléphone) + Réseau
- Table de mixage web complète avec effets et mastering IA
- Deux modes : Studio Console (XLR) / Smart Mic Array (PDM Beamforming)

---

## Architecture Matérielle

```
                    ┌────────────────────────────────────────┐
                    │            i.MX8MP SoC                 │
                    │                                        │
  8x XLR IN ──┐    │  ┌──────────┐    ┌──────────┐          │
  8x PDM MIC ─┤    │  │ AudioMix │    │ DSP      │          │
              ▼    │  │          │    │ HiFi4    │          │
  ┌──────────────┐ │  │  SAI7 ◄──┼────┤ (SOF)    │          │
  │ 4x TAC5212   │◄┼──┤  TDM-8  │    │ 800MHz   │          │
  │ I2C 0x50-53  │─┼──►  48kHz  │    └────┬─────┘          │
  └──────────────┘ │  │  dsp_a   │         │                │
  8x XLR OUT ──┘   │  │          │    ┌────┴─────┐          │
                    │  │  EASRC ◄─┼────┤ NPU      │          │
                    │  │  (HW IP) │    │ 2.3 TOPS │          │
                    │  └────┬─────┘    └──────────┘          │
                    │       │                                │
                    │  ┌────┴──────────────────────┐         │
                    │  │ USB Composite Gadget       │         │
                    │  │ ├─ UAC2 Audio 8x8 (PC)    │──► USB1 │
                    │  │ ├─ RNDIS/ECM Network       │         │
                    │  │ └─ UAC2 Audio 2x2 (Phone)  │──► USB2 │
                    │  └───────────────────────────┘         │
                    └────────────────────────────────────────┘
```

### Composants validés

| Composant | Détail | Statut |
|-----------|--------|--------|
| SAI7 | TDM-8, 48kHz, 32-bit, dsp_a, TX master, RX slave sync | ✅ |
| TAC5212 x4 | I2C 0x50-0x53, slots 0-7, TX/RX_OFFSET=1 | ✅ |
| PDM | 2 micros/TAC, GPO1=PDMCLK, GPI1=data, DIN1_SEL=GPI1 | ✅ |
| Full-duplex | 8ch capture + 8ch playback simultanés | ✅ |
| Patch fsl_sai | async RX consumer + async capture TX config | ✅ |
| REGCACHE_NONE | Écriture I2C directe, pas de cache regmap | ✅ |

---

## Pipeline Audio — Topologie Studio Console

### Étage A : Entrées & Pré-traitement matériel (TAC5212)
Latence zéro, charge DSP = 0%

Par canal (x8) :
1. **Sélection source** — MUX Analog (XLR) / PDM (micro) via I2C INTF_CFG4
2. **PGA** — Gain analogique programmable (préamplification XLR)
3. **Input EQ** — Biquads ADC du TAC5212 (EQ paramétrique corrective, 3 biquads/canal)
4. **HPF** — Filtre passe-haut programmable (1Hz / 12Hz / 96Hz)
5. **Noise Gate** — Coupe le silence (DRE du TAC5212)
6. **Distortion Limiter** — Écrêteur de sécurité anti-clipping
7. **ADC Digital Volume** — Gain numérique (-80dB à +47dB, pas 0.5dB)

### Étage B : Channel Strips (DSP HiFi4 via SOF)
Traitement par voie, 8 instances

Par canal (x8) :
1. **Channel EQ Paramétrique** — 4 bandes (Low Shelf, 2x Peak, High Shelf)
2. **Compresseur de piste** — Threshold, Ratio, Attack, Release, Makeup Gain
3. **Pan** — Positionnement stéréo dans le master bus
4. **Fader** — Volume de la piste
5. **Mute / Solo** — Contrôle monitoring
6. **Aux Send 1** — Niveau d'envoi vers Bus Réverbe (pre/post fader)
7. **Aux Send 2** — Niveau d'envoi vers Bus Delay (pre/post fader)
8. **Aux Send 3** — Niveau d'envoi vers Bus Modulation (pre/post fader)

### Étage C : Bus d'Effets Auxiliaires (DSP HiFi4)
Effets partagés, 1 instance chacun (économie MIPS)

| Bus | Effet | Paramètres |
|-----|-------|------------|
| Aux 1 | **Réverbe** (algorithmique) | Type (Room/Hall/Plate), Size, Decay, Pre-delay, Damping, Mix |
| Aux 2 | **Delay / Echo** | Time (ms ou BPM sync), Feedback, Filter, Ping-Pong, Mix |
| Aux 3 | **Modulation** | Chorus/Flanger/Phaser (sélectionnable), Rate, Depth, Feedback |

Chaque bus a un **Return Level** (volume de retour dans le master bus).

### Étage D : Master Bus (DSP HiFi4)
Traitement du mix stéréo final

1. **Exciter** — Ajout d'harmoniques hautes fréquences (Brilliance, Amount)
2. **Harmonizer** — Épaississement stéréo (Stereo Width, Detune)
3. **Compresseur Multibande** — 3-4 bandes (Low/Mid/High/Air)
   - Par bande : Threshold, Ratio, Attack, Release, Gain
4. **Glue Compressor** — Compresseur master final (Threshold, Ratio, Attack, Release, Mix)
5. **Master Limiter** — Brickwall limiter (-0.3 dBFS) protection finale
6. **Master Volume** — Fader général

### Étage E : Sortie & Adaptation (TAC5212)
Post-traitement matériel, charge DSP = 0%

1. **Master EQ** — Biquads DAC du TAC5212 (Room EQ / correction enceintes)
2. **DAC Digital Volume** — Niveau de sortie par canal
3. **Output Drive** — Configuration Line/Headphone par sortie

---

## Pipeline Audio — Topologie Smart Mic Array

```
8x PDM → TAC5212 [HPF 100Hz] → SAI7 → DSP [Beamforming 8ch→1ch]
                                              ↓
                                    DSP [Noise Gate + EQ]
                                              ↓
                                    NPU [RNNoise / DNS]
                                              ↓
                                    USB Gadget (Mono/Stéréo)
```

Paramètres Beamforming : coordonnées X,Y de chaque micro (matrice asymétrique).

---

## Clocking & PLL

### Problème identifié
Le TAC5212 a besoin de BCLK actif pour locker sa PLL. Sans clocks continues,
chaque start/stop de stream ou changement de fréquence cause un bruit blanc
pendant la stabilisation PLL (~5 secondes).

### Solution : DAIFMT_CONT
Patcher fsl_sai pour supporter `SND_SOC_DAIFMT_CONT` :
- Stocker le flag CONT dans `struct fsl_sai`
- Dans `fsl_sai_set_dai_fmt_tr()` : capturer le flag
- Dans `fsl_sai_trigger(STOP)` : ne PAS appeler `fsl_sai_config_disable()` si CONT
- Dans le DT : ajouter `continuous-clock;` dans le DAI link

Avec SOF, le DSP maintient le SAI7 actif en permanence → BCLK continu natif.

### Workaround actuel (sans CONT)
- SW reset au premier hw_params (needs_reset flag)
- Script `tac-reset` pour réinit PLL pendant un stream actif

---

## USB Composite Gadget

### Configuration ConfigFS

```
/sys/kernel/config/usb_gadget/audiomix/
├── idVendor = 0x1d6b
├── idProduct = 0x0104
├── strings/0x409/
│   ├── manufacturer = "Debix Audio"
│   ├── product = "Audio Platform 8x8"
│   └── serialnumber = "001"
├── functions/
│   ├── uac2.0/          # Audio 8x8 pour PC
│   │   ├── c_chmask = 0xFF    # 8 canaux capture
│   │   ├── c_srate = 48000
│   │   ├── c_ssize = 4        # 32-bit
│   │   ├── p_chmask = 0xFF    # 8 canaux playback
│   │   ├── p_srate = 48000
│   │   └── p_ssize = 4
│   ├── uac2.1/          # Audio 2x2 pour téléphone (USB2)
│   │   ├── c_chmask = 0x03
│   │   ├── p_chmask = 0x03
│   │   └── ...
│   └── rndis.0/         # Réseau Ethernet over USB
│       └── (config auto)
├── configs/c.1/
│   ├── uac2.0 → functions/uac2.0
│   └── rndis.0 → functions/rndis.0
└── UDC = <controller>
```

### Réseau USB
- IP fixe : 192.168.10.1 (côté carte)
- DHCP serveur : dnsmasq (range 192.168.10.100-200)
- Le PC obtient automatiquement une IP et accède au site web

---

## Interface Web — Table de Mixage

### Stack technique
- **Serveur** : lighttpd (léger, embarqué)
- **Backend API** : Python Flask ou Node.js
- **Frontend** : HTML5 + CSS3 + JavaScript (vanilla ou Vue.js léger)
- **Communication** : WebSocket pour temps réel, REST pour config

### API REST

```
# Canaux d'entrée (1-8)
GET/POST /api/channel/{n}/source         # "analog" | "pdm"
GET/POST /api/channel/{n}/gain           # -80.0 à +47.0 dB
GET/POST /api/channel/{n}/phantom        # true/false (MICBIAS)
GET/POST /api/channel/{n}/hpf            # "off" | "1hz" | "12hz" | "96hz"
GET/POST /api/channel/{n}/gate           # {enabled, threshold, release}
GET/POST /api/channel/{n}/eq/input       # {band1..3: {freq, gain, q}} (TAC Biquads)
GET/POST /api/channel/{n}/eq/channel     # {band1..4: {type, freq, gain, q}} (DSP)
GET/POST /api/channel/{n}/compressor     # {threshold, ratio, attack, release, gain}
GET/POST /api/channel/{n}/pan            # -100 (L) à +100 (R)
GET/POST /api/channel/{n}/fader          # -inf à +10 dB
GET/POST /api/channel/{n}/mute           # true/false
GET/POST /api/channel/{n}/solo           # true/false
GET/POST /api/channel/{n}/send/{1-3}     # {level, pre_post}

# Bus d'effets auxiliaires
GET/POST /api/fx/reverb                  # {type, size, decay, predelay, damping, mix}
GET/POST /api/fx/delay                   # {time_ms, feedback, filter, pingpong, mix}
GET/POST /api/fx/modulation              # {type, rate, depth, feedback, mix}
GET/POST /api/fx/{1-3}/return            # niveau de retour dans le master

# Master Bus
GET/POST /api/master/exciter             # {enabled, frequency, amount}
GET/POST /api/master/harmonizer          # {enabled, width, detune}
GET/POST /api/master/multiband           # {bands: [{freq, threshold, ratio, attack, release, gain}]}
GET/POST /api/master/compressor          # {threshold, ratio, attack, release, mix}
GET/POST /api/master/limiter             # {ceiling, release}
GET/POST /api/master/fader               # -inf à 0 dB
GET/POST /api/master/eq                  # {band1..3: {freq, gain, q}} (TAC DAC Biquads)

# USB Audio
GET/POST /api/usb/pc/routing             # Matrice de routage 8x8
GET/POST /api/usb/phone/routing          # Matrice de routage 2x2

# NPU Auto-Mastering
GET/POST /api/npu/auto_master            # {enabled, target_lufs, style}
GET/POST /api/npu/noise_suppression      # {enabled, aggressiveness}

# Système
GET/POST /api/system/topology            # "studio" | "smartmic"
GET/POST /api/system/preset              # Sauvegarder/charger un preset complet
GET      /api/system/meters              # VU-mètres temps réel (WebSocket)
GET      /api/system/status              # Statut PLL, clock errors, DSP load
```

### Interface graphique (Vue utilisateur)

```
┌─────────────────────────────────────────────────────────────────┐
│                    DEBIX AUDIO PLATFORM                         │
│  [Studio Console]  [Smart Mic]                    [Settings ⚙]  │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  CH1      CH2      CH3      CH4      CH5      CH6    CH7   CH8 │
│ ┌─────┐ ┌─────┐ ┌─────┐ ┌─────┐ ┌─────┐ ┌─────┐ ┌────┐ ┌────┐│
│ │[PDM]│ │[PDM]│ │[XLR]│ │[XLR]│ │[XLR]│ │[XLR]│ │XLR │ │XLR ││
│ │     │ │     │ │     │ │     │ │     │ │     │ │    │ │    ││
│ │[48V]│ │[48V]│ │[48V]│ │[48V]│ │[48V]│ │[48V]│ │48V │ │48V ││
│ │     │ │     │ │     │ │     │ │     │ │     │ │    │ │    ││
│ │ EQ  │ │ EQ  │ │ EQ  │ │ EQ  │ │ EQ  │ │ EQ  │ │ EQ │ │ EQ ││
│ │[|||]│ │[|||]│ │[|||]│ │[|||]│ │[|||]│ │[|||]│ │||||│ │||||││
│ │     │ │     │ │     │ │     │ │     │ │     │ │    │ │    ││
│ │Gate │ │Gate │ │Gate │ │Gate │ │Gate │ │Gate │ │Gate│ │Gate││
│ │Comp │ │Comp │ │Comp │ │Comp │ │Comp │ │Comp │ │Comp│ │Comp││
│ │     │ │     │ │     │ │     │ │     │ │     │ │    │ │    ││
│ │Rvb○─│ │Rvb○─│ │Rvb○─│ │Rvb○─│ │Rvb○─│ │Rvb○─│ │Rvb○│ │Rvb○││
│ │Dly○─│ │Dly○─│ │Dly○─│ │Dly○─│ │Dly○─│ │Dly○─│ │Dly○│ │Dly○││
│ │Mod○─│ │Mod○─│ │Mod○─│ │Mod○─│ │Mod○─│ │Mod○─│ │Mod○│ │Mod○││
│ │     │ │     │ │     │ │     │ │     │ │     │ │    │ │    ││
│ │◄PAN►│ │◄PAN►│ │◄PAN►│ │◄PAN►│ │◄PAN►│ │◄PAN►│ │PAN │ │PAN ││
│ │     │ │     │ │     │ │     │ │     │ │     │ │    │ │    ││
│ │ [M] │ │ [M] │ │ [M] │ │ [M] │ │ [M] │ │ [M] │ │[M] │ │[M] ││
│ │ [S] │ │ [S] │ │ [S] │ │ [S] │ │ [S] │ │ [S] │ │[S] │ │[S] ││
│ │     │ │     │ │     │ │     │ │     │ │     │ │    │ │    ││
│ │ ▓▓░ │ │ ▓░░ │ │ ▓▓▓ │ │ ▓░░ │ │ ▓▓░ │ │ ▓░░ │ │ ▓▓ │ │ ▓░ ││
│ │ ║║║ │ │ ║║║ │ │ ║║║ │ │ ║║║ │ │ ║║║ │ │ ║║║ │ │║║║ │ │║║║ ││
│ │ ║█║ │ │ ║█║ │ │ ║█║ │ │ ║█║ │ │ ║█║ │ │ ║█║ │ │║█║ │ │║█║ ││
│ │ ║║║ │ │ ║║║ │ │ ║║║ │ │ ║║║ │ │ ║║║ │ │ ║║║ │ │║║║ │ │║║║ ││
│ └─────┘ └─────┘ └─────┘ └─────┘ └─────┘ └─────┘ └────┘ └────┘│
│                                                                 │
├──────────────────────┬──────────────────────────────────────────┤
│  FX RETURNS          │         MASTER BUS                       │
│ ┌──────┐┌──────┐┌──┐│ ┌──────────────────────────────────────┐ │
│ │Reverb││Delay ││Mo││ │[Exciter] [Harmonizer]                │ │
│ │Room ▼││300ms ││Ch││ │[MultiBand Comp]  [Glue Comp]         │ │
│ │      ││      ││  ││ │[Limiter -0.3dB]                      │ │
│ │ ║█║  ││ ║█║  ││║█││ │[Master EQ]                           │ │
│ │ ║║║  ││ ║║║  ││║║││ │                                      │ │
│ └──────┘└──────┘└──┘│ │ [NPU Auto-Master: ● ON]              │ │
│                      │ │                                      │ │
│                      │ │  L ▓▓▓▓▓▓▓▓▓▓▓▓░░░░  -6dB          │ │
│                      │ │  R ▓▓▓▓▓▓▓▓▓▓▓░░░░░  -7dB          │ │
│                      │ │                         ║║║          │ │
│                      │ │                         ║█║ MASTER   │ │
│                      │ │                         ║║║          │ │
│                      │ └──────────────────────────────────────┘ │
├─────────────────────────────────────────────────────────────────┤
│  USB PC: [■ Connected 8x8]    Phone: [□ Disconnected]          │
│  Clock: 48kHz ● Locked        DSP Load: 62%     [Save Preset]  │
└─────────────────────────────────────────────────────────────────┘
```

---

## Plan d'Implémentation Détaillé

### Phase 1 : Stabilisation base audio ✅ FAIT
- [x] Driver TAC5212 ASoC (TDM, dsp_a, TX/RX_OFFSET=1)
- [x] Patch fsl_sai (async RX consumer, async capture TX config)
- [x] Full-duplex 8in/8out validé (44.1k, 48k, 96k)
- [x] PDM fonctionnel (fix DIN1/DIN2, SW reset avec BCLK)
- [x] REGCACHE_NONE, script tac-reset

### Phase 2 : SOF + Clocks continues
- [ ] 2.1 Investiguer `sof-audio-of-imx8m probe failed -22`
- [ ] 2.2 Implémenter DAIFMT_CONT dans fsl_sai (3 modifs)
- [ ] 2.3 Ajouter `continuous-clock` dans le DT
- [ ] 2.4 Créer topologie SOF nocodec pour SAI7 TDM-8
- [ ] 2.5 Valider : BCLK permanent, PDM propre dès le boot, changement de fréquence sans bruit
- [ ] 2.6 Adapter le driver tac5212.c comme codec SOF (retirer machine driver imx-card)

### Phase 3 : Effets TAC5212 (contrôle I2C)
- [ ] 3.1 Exposer coefficients Biquads ADC (Input EQ, 3 biquads/canal)
- [ ] 3.2 Exposer coefficients Biquads DAC (Master/Output EQ)
- [ ] 3.3 Contrôles fins : Noise Gate (DRE threshold), AGC
- [ ] 3.4 Outil CLI pour calculer et charger des presets EQ (freq, gain, Q → coefficients Biquad)
- [ ] 3.5 Valider : EQ d'entrée + EQ de sortie fonctionnels via amixer/I2C

### Phase 4 : Effets DSP (SOF Processing Modules)
- [ ] 4.1 Channel Strip : EQ paramétrique 4 bandes (SOF builtin gain + eq-iir)
- [ ] 4.2 Channel Compressor (SOF DRC module)
- [ ] 4.3 Bus Aux 1 : Réverbe algorithmique (custom processing module ou SOF builtin)
- [ ] 4.4 Bus Aux 2 : Delay / Echo (custom module)
- [ ] 4.5 Bus Aux 3 : Modulation Chorus/Flanger/Phaser (custom module)
- [ ] 4.6 Aux Send/Return : routing par canal vers chaque bus (niveau + pre/post fader)
- [ ] 4.7 Master Bus : Exciter, Harmonizer, Compresseur Multibande, Glue Compressor, Limiter
- [ ] 4.8 Topologie complète (.tplg) avec matrice de routage
- [ ] 4.9 Exposer tous les paramètres comme kcontrols ALSA
- [ ] 4.10 Valider : chaîne complète channel strip → FX → master → sortie
- [ ] 4.11 Mesurer charge DSP, optimiser si nécessaire

### Phase 5 : USB Audio Gadget + EASRC
- [ ] 5.1 USB Composite Gadget ConfigFS (UAC2 8x8 + RNDIS)
- [ ] 5.2 Script systemd pour configuration automatique au boot
- [ ] 5.3 Intégration EASRC : routage DMA des flux USB vers ASRC hardware
- [ ] 5.4 Topologie SOF étendue : endpoints USB dans la matrice de routage
- [ ] 5.5 2ème port USB : UAC2 2x2 pour téléphone
- [ ] 5.6 Valider : PC voit carte son 8x8 + interface réseau, téléphone 2x2

### Phase 6 : NPU Auto-Mastering
- [ ] 6.1 Modèle d'analyse spectrale (TFLite sur NPU via eIQ)
- [ ] 6.2 Boucle de contrôle : analyse master → décisions → commandes I2C + ALSA
- [ ] 6.3 Auto-EQ entrées : NPU détecte résonances → corrige Biquads TAC5212 via I2C
- [ ] 6.4 Auto-Compressor : ajuste seuils/ratios DSP en temps réel
- [ ] 6.5 Presets de mastering : courbes cibles (voix, musique live, podcast)
- [ ] 6.6 Deep Noise Suppression (RNNoise) pour topologie Smart Mic
- [ ] 6.7 Valider : mastering IA en boucle fermée, A/B test avec/sans NPU

### Phase 7 : Interface Web — Table de Mixage
- [ ] 7.1 USB RNDIS : serveur DHCP (dnsmasq), IP fixe 192.168.10.1
- [ ] 7.2 Serveur web : lighttpd + Flask API
- [ ] 7.3 API REST complète (voir spec ci-dessus)
- [ ] 7.4 Backend audio : bridge amixer/libasound ↔ API REST
- [ ] 7.5 WebSocket pour VU-mètres et mises à jour temps réel
- [ ] 7.6 Frontend : interface table de mixage (faders, knobs, EQ, sends)
- [ ] 7.7 Page effets : réverbe, delay, modulation avec paramètres complets
- [ ] 7.8 Page master : exciter, harmonizer, multiband comp, glue comp, limiter, EQ
- [ ] 7.9 Gestion des presets : sauvegarder/charger configurations complètes
- [ ] 7.10 Page système : status PLL, DSP load, USB, sélection topologie
- [ ] 7.11 Design responsive (PC, tablette, téléphone)
- [ ] 7.12 Valider : contrôle complet de la console depuis un navigateur

### Phase 8 : Intégration & Production
- [ ] 8.1 Build image Yocto complète avec tous les composants
- [ ] 8.2 Tests de stress : 8ch full-duplex + USB + web pendant 24h
- [ ] 8.3 Mesures audio : latence bout-en-bout, THD+N, SNR, bruit de fond
- [ ] 8.4 Documentation utilisateur
- [ ] 8.5 Packaging : sélection topologie au boot (GPIO ou config file)

---

## Fichiers clés du projet

| Fichier | Rôle |
|---------|------|
| `meta-local/recipes-kernel/linux/files/tac5212.c` | Driver ASoC codec TAC5212 |
| `meta-local/recipes-kernel/linux/files/tac5212.h` | Registres TAC5212 |
| `meta-local/recipes-kernel/linux/files/apply-tac5212-dt.py` | Modifications Device Tree |
| `meta-local/recipes-kernel/linux/linux-imx_%.bbappend` | Intégration Yocto + patches fsl_sai |
| `meta-local/recipes-kernel/linux/files/tac-reset.sh` | Script reset PLL runtime |
| `sof-imx8mp-tac5212-studio.tplg` | Topologie SOF Studio Console (à créer) |
| `sof-imx8mp-tac5212-smartmic.tplg` | Topologie SOF Smart Mic (à créer) |
| `webapp/` | Interface web table de mixage (à créer) |

---

## Notes techniques importantes

1. **Bus Auxiliaires** : Les effets lourds (Réverbe, Delay, Modulation) sont en bus partagés,
   pas en insert par canal. Chaque canal a un Send (niveau d'envoi). Le DSP calcule UNE instance
   de chaque effet. C'est la méthode standard des consoles pro pour économiser la puissance DSP.

2. **EASRC** : L'IP hardware EASRC resynchronise les flux USB (horloge PC/téléphone) sur
   l'horloge locale 48kHz. Sans EASRC, il y aurait des clics/craquements toutes les minutes
   à cause de la dérive d'horloge USB.

3. **Fréquence fixe 48kHz** : Le bus SAI7 tourne à 48kHz constant. Le SRC de SOF convertit
   vers d'autres fréquences si nécessaire (16kHz pour NPU, etc.).

4. **NPU Auto-Mastering** : Le NPU ne traite pas l'audio directement. Il analyse le spectre
   et pilote les paramètres des TAC5212 (I2C) et du DSP (ALSA kcontrols).
   C'est un "ingénieur du son virtuel" qui tourne les potards.

5. **Séparation Control/Data** : Le data path passe par le DSP (SOF).
   Le control path reste sur Linux (I2C pour TAC5212, ALSA kcontrols pour DSP).
