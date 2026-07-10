// V12-LOOP-PRO — Loopstation multipiste (type RC-505).
// 6 pistes indépendantes : chacune enregistre une voie d'entrée choisie
// (M1..M8 / USB1..USB8), se restitue additionnée dans les tranches P1/P2,
// et peut être activée/désactivée (MUTE) ou effacée (CLEAR) individuellement.
// La 1re piste enregistrée définit la longueur maître ; les suivantes sont
// alignées. Poll looper_status ~4 Hz quand la page est visible.
import QtQuick

Item {
    id: page

    // 16 voies sélectionnables : 8 mics + 8 stems USB
    function srcName(idx) {
        if (idx < 0) return "—";
        if (idx < 8) return "M" + (idx + 1);
        if (idx < 16) return "USB" + (idx - 7);
        return "?";
    }
    readonly property var trackColors: [
        "#e5a13c", "#4cc470", "#5aa9e6", "#c77dff",
        "#e0678a", "#e8b84b"
    ]

    // état renvoyé par looper_status (défaut : 6 pistes vides)
    property var status: ({
        master_len_s: 0, pos_s: 0, run: 0, max_s: 40,
        tracks: [
            { track: 0, state: "empty", len_s: 0, muted: 0, src_a: 0, src_b: -1, gain_db: 0, peak: 0 },
            { track: 1, state: "empty", len_s: 0, muted: 0, src_a: 1, src_b: -1, gain_db: 0, peak: 0 },
            { track: 2, state: "empty", len_s: 0, muted: 0, src_a: 2, src_b: -1, gain_db: 0, peak: 0 },
            { track: 3, state: "empty", len_s: 0, muted: 0, src_a: 3, src_b: -1, gain_db: 0, peak: 0 },
            { track: 4, state: "empty", len_s: 0, muted: 0, src_a: 4, src_b: -1, gain_db: 0, peak: 0 },
            { track: 5, state: "empty", len_s: 0, muted: 0, src_a: 5, src_b: -1, gain_db: 0, peak: 0 }
        ]
    })

    onVisibleChanged: if (visible) refresh()
    Timer { interval: 250; running: page.visible; repeat: true; onTriggered: page.refresh() }

    // V13.2-FLUID : le poll (4 Hz) ne pose que des CIBLES ; la position est
    // extrapolée à la frame et les VU ont une ballistique attaque/retombée
    // (recette SpectrumView — jamais d'affichage au rythme du réseau).
    property real posDisp: 0
    property real mvuDisp: 0
    property real _pollPos: 0
    property double _pollT: 0

    function refresh() {
        mixer.call({ op: "looper_status" }, function(r) {
            if (r.ok) {
                page.status = r;
                page._pollPos = r.pos_s;
                page._pollT = Date.now();
            }
        });
    }

    FrameAnimation {
        running: page.visible
        onTriggered: {
            const st = page.status;
            const mlen = st.master_len_s || 0;
            if (mlen > 0 && st.run)
                page.posDisp = (page._pollPos
                                + (Date.now() - page._pollT) / 1000.0) % mlen;
            else
                page.posDisp = st.pos_s || 0;
            const dt = Math.min(frameTime, 0.1);
            const kA = 1 - Math.exp(-dt / 0.030);
            const kR = 1 - Math.exp(-dt / 0.120);
            var tgt = 0;
            var f = (st.master_peak || 0) / 2147483647.0;
            if (f > 0) {
                var db = 20 * Math.log(f) / Math.LN10;
                tgt = Math.max(0, Math.min(1, (db + 48) / 48));
            }
            page.mvuDisp += (tgt - page.mvuDisp) * (tgt > page.mvuDisp ? kA : kR);
        }
    }
    function setGain(t, db) {
        mixer.call({ op: "looper_track_cfg", track: t, gain_db: db },
                   function() {});
    }
    function trackCtl(t, action) {
        mixer.call({ op: "looper_track_ctl", track: t, action: action },
                   function() { page.refresh(); });
    }
    function setSrc(t, idx) {
        mixer.call({ op: "looper_track_cfg", track: t, src_a: idx, src_b: -1 },
                   function() { page.refresh(); });
    }
    function globalCtl(action) {
        mixer.call({ op: "looper_ctl", action: action },
                   function() { page.refresh(); });
    }

    Column {
        anchors.fill: parent
        anchors.margins: 14
        spacing: 10

        // ===================== EN-TÊTE / TRANSPORT =====================
        Rectangle {
            width: parent.width; height: 70
            color: "#171c21"; radius: 8; border.color: "#060809"
            Item {
                anchors.fill: parent; anchors.margins: 12

                Row {
                id: headLeft
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                height: parent.height
                spacing: 14

                Text {
                    text: "LOOPER"; color: "#e5a13c"; font.pixelSize: 13
                    font.bold: true; font.letterSpacing: 3
                    anchors.verticalCenter: parent.verticalCenter
                }

                // position maître : étiquette AU-DESSUS + barre 26 px
                // (même gabarit que NIVEAU/VOLUME/MASTER)
                Column {
                    spacing: 6
                    anchors.verticalCenter: parent.verticalCenter
                    Text {
                        text: page.status.master_len_s > 0
                              ? "BOUCLE " + page.status.master_len_s.toFixed(2)
                                + " S  ·  " + page.posDisp.toFixed(1) + " S"
                              : "BOUCLE  — enregistrez une 1re piste"
                        color: "#5c666e"; font.pixelSize: 9
                        font.letterSpacing: 2
                    }
                    Rectangle {
                        width: 200; height: 26; radius: 13
                        color: "#0b0e11"
                        Rectangle {
                            height: parent.height; radius: 13
                            width: page.status.master_len_s > 0
                                   ? parent.width * Math.min(1,
                                         page.posDisp / page.status.master_len_s) : 0
                            color: page.status.run ? "#4cc470" : "#5c666e"
                        }
                    }
                }

                // V12-VU : vumètre MASTER looper (somme des pistes)
                Column {
                    spacing: 3
                    anchors.verticalCenter: parent.verticalCenter
                    Text { text: "MASTER"; color: "#5c666e"; font.pixelSize: 9
                           font.letterSpacing: 2 }
                    Rectangle {
                        width: 110; height: 26; radius: 13; color: "#0b0e11"
                        Rectangle {
                            height: parent.height; radius: 13
                            width: parent.width * page.mvuDisp
                            color: (page.status.master_peak || 0) > 1932735283
                                   ? "#e05545" : "#4cc470"   /* rouge > -0,9 dBFS */
                        }
                    }
                }

                }

                Row {
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 14
                    Repeater {
                        model: [
                            ["▶ PLAY ALL",  "play_all",  "#4cc470", "#2a2214"],
                            ["■ STOP ALL",  "stop_all",  "#8b959d", "#1b2126"]
                        ]
                        Rectangle {
                            width: 120; height: 44; radius: 6
                            anchors.verticalCenter: parent.verticalCenter
                            color: modelData[3]; border.color: modelData[2]; border.width: 1
                            Text { anchors.centerIn: parent; text: modelData[0]
                                   color: modelData[2]; font.pixelSize: 12; font.bold: true }
                            TapHandler {
                                gesturePolicy: TapHandler.ReleaseWithinBounds
                                onTapped: page.globalCtl(modelData[1])
                            }
                        }
                    }

                    // CLEAR ALL : appui long avec anneau (HoldButton)
                    HoldButton {
                        anchors.verticalCenter: parent.verticalCenter
                        label: "✕ CLEAR ALL"
                        onTriggered: page.globalCtl("clear_all")
                    }
                }
            }
        }

        // ========================= PISTES =========================
        // V12-MIDIX-GUI : Flickable — 6 pistes ne tiennent pas sur l'écran,
        // scroll vertical tactile (les boutons restent des taps).
        Flickable {
            width: parent.width
            height: parent.height - 70 - 24 - 2*10   // en-tête + légende + spacing
            contentHeight: tracksCol.height
            clip: true

            Column {
            id: tracksCol
            width: parent.width
            spacing: 8
            Repeater {
                model: 6
                Rectangle {
                    id: trackRow
                    property var tr: (page.status.tracks && index < page.status.tracks.length)
                                     ? page.status.tracks[index] : null
                    property string st: tr ? tr.state : "empty"
                    property bool isRec: st === "rec"
                    property bool isPlay: st === "play"
                    property bool isEmpty: st === "empty"
                    property bool isArmed: st === "armed"
                    property bool muted: tr ? (tr.muted === 1) : false
                    property color accent: page.trackColors[index]

                    width: parent.width
                    height: 128
                    radius: 8
                    color: isRec ? "#2a1512"
                           : (isArmed ? "#241d10" : (isPlay ? "#141b16" : "#14181c"))
                    border.color: isRec ? "#e05545"
                                  : (isArmed ? "#e8b84b"
                                     : (isPlay && !muted ? accent : "#22282e"))
                    border.width: (isRec || isArmed || (isPlay && !muted)) ? 2 : 1

                    Row {
                        anchors.fill: parent
                        anchors.margins: 12
                        // largeur fixe des 5 groupes = 850 px → l'espace
                        // restant est réparti également (jamais < 14)
                        spacing: Math.max(14, (width - 850) / 4)

                        // --- badge numéro + état ---
                        Column {
                            width: 74
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 5
                            Rectangle {
                                width: 40; height: 40; radius: 20
                                color: isEmpty ? "#1b2126" : trackRow.accent
                                Text { anchors.centerIn: parent; text: (index + 1)
                                       color: isEmpty ? "#5c666e" : "#0b0e11"
                                       font.pixelSize: 18; font.bold: true }
                            }
                            Text {
                                text: isRec ? "REC"
                                      : (isArmed ? "ARMÉ"
                                         : (isPlay ? (muted ? "MUTE" : "PLAY") : "vide"))
                                color: isRec ? "#e05545"
                                       : isArmed ? "#e8b84b"
                                       : (isPlay ? (muted ? "#e8b84b" : trackRow.accent) : "#5c666e")
                                font.pixelSize: 11; font.bold: true
                                font.letterSpacing: 1
                            }
                        }

                        // --- sélecteur de voie source (‹ M3 ›) ---
                        Column {
                            width: 128
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 5
                            Text { text: "SOURCE"; color: "#5c666e"; font.pixelSize: 9
                                   font.letterSpacing: 2 }
                            Row {
                                spacing: 6
                                Rectangle {
                                    width: 32; height: 40; radius: 5
                                    color: isRec ? "#181b1e" : "#1b2126"
                                    border.color: "#39434b"
                                    Text { anchors.centerIn: parent; text: "‹"
                                           color: isRec ? "#3a434b" : "#c9c4b8"
                                           font.pixelSize: 20 }
                                    TapHandler {
                                        enabled: !isRec
                                        gesturePolicy: TapHandler.ReleaseWithinBounds
                                        onTapped: {
                                            var s = tr ? tr.src_a : index;
                                            if (s > 0) page.setSrc(index, s - 1);
                                        }
                                    }
                                }
                                Rectangle {
                                    width: 56; height: 40; radius: 5
                                    color: "#0f1216"; border.color: trackRow.accent
                                    Text { anchors.centerIn: parent
                                           text: page.srcName(tr ? tr.src_a : index)
                                           color: "#e9e5da"; font.pixelSize: 15; font.bold: true }
                                }
                                Rectangle {
                                    width: 32; height: 40; radius: 5
                                    color: isRec ? "#181b1e" : "#1b2126"
                                    border.color: "#39434b"
                                    Text { anchors.centerIn: parent; text: "›"
                                           color: isRec ? "#3a434b" : "#c9c4b8"
                                           font.pixelSize: 20 }
                                    TapHandler {
                                        enabled: !isRec
                                        gesturePolicy: TapHandler.ReleaseWithinBounds
                                        onTapped: {
                                            var s = tr ? tr.src_a : index;
                                            if (s < 15) page.setSrc(index, s + 1);
                                        }
                                    }
                                }
                            }
                        }

                        // --- VU crête + durée --- (même gabarit que VOLUME :
                        // texte AU-DESSUS, barre 26 px ; ballistique à la frame)
                        Column {
                            id: vuCol
                            width: 150
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 6
                            property real vu: 0
                            FrameAnimation {
                                running: page.visible && (isRec || (isPlay && !muted))
                                onTriggered: {
                                    const dt = Math.min(frameTime, 0.1);
                                    const kA = 1 - Math.exp(-dt / 0.030);
                                    const kR = 1 - Math.exp(-dt / 0.120);
                                    var tgt = 0;
                                    var f = (tr ? tr.peak : 0) / 2147483647.0;
                                    if (f > 0) {
                                        var db = 20 * Math.log(f) / Math.LN10;
                                        tgt = Math.max(0, Math.min(1, (db + 48) / 48));
                                    }
                                    vuCol.vu += (tgt - vuCol.vu)
                                                * (tgt > vuCol.vu ? kA : kR);
                                }
                                onRunningChanged: if (!running) vuCol.vu = 0
                            }
                            Text {
                                text: (tr && tr.len_s > 0)
                                      ? "NIVEAU  " + tr.len_s.toFixed(2) + " s"
                                      : (isRec ? "● ENREGISTRE…"
                                         : (isArmed ? "⏳ DÉPART AU TOUR" : "NIVEAU  —"))
                                color: isRec ? "#e05545"
                                       : (isArmed ? "#e8b84b" : "#5c666e")
                                font.pixelSize: 9
                                font.letterSpacing: 2
                            }
                            Rectangle {
                                width: parent.width; height: 26; radius: 13
                                color: "#0b0e11"
                                Rectangle {
                                    height: parent.height; radius: 13
                                    width: parent.width * vuCol.vu
                                    color: isRec ? "#e05545" : trackRow.accent
                                }
                            }
                        }

                        // --- volume de piste (gain lecture, -40..+6 dB) ---
                        // Écho LOCAL : pendant le drag, l'affichage suit le
                        // doigt à la frame ; le moteur/poll suivent derrière
                        // (règle de fluidité — jamais d'UI qui attend le réseau)
                        Column {
                            id: volCol
                            width: 150
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 6
                            property bool dragging: false
                            property real localDb: 0
                            property real showDb: dragging ? localDb
                                                           : (tr ? tr.gain_db : 0)
                            Text { text: "VOLUME  " + (volCol.showDb >= 0 ? "+" : "")
                                         + volCol.showDb.toFixed(1) + " dB"
                                   color: volCol.dragging ? "#e9e5da" : "#5c666e"
                                   font.pixelSize: 9
                                   font.letterSpacing: 2 }
                            Rectangle {
                                id: volTrack
                                width: parent.width; height: 26; radius: 13
                                color: "#0b0e11"
                                Rectangle {
                                    height: parent.height; radius: 13
                                    width: parent.width *
                                           Math.max(0, Math.min(1, (volCol.showDb + 40) / 46))
                                    color: volCol.dragging ? "#4a555f" : "#39434b"
                                    Rectangle { width: 4; height: parent.height
                                                anchors.right: parent.right
                                                color: trackRow.accent }
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    anchors.margins: -8
                                    preventStealing: true
                                    function dbAt(mx) {
                                        var f = Math.max(0, Math.min(1, mx / volTrack.width));
                                        return Math.round((-40 + f * 46) * 2) / 2;
                                    }
                                    onPressed: (m) => {
                                        volCol.localDb = dbAt(m.x);
                                        volCol.dragging = true;
                                        page.setGain(index, volCol.localDb);
                                    }
                                    onPositionChanged: (m) => {
                                        if (!pressed) return;
                                        var db = dbAt(m.x);
                                        if (db === volCol.localDb) return;
                                        volCol.localDb = db;
                                        page.setGain(index, db);
                                    }
                                    onReleased: volCol.dragging = false
                                    onCanceled: volCol.dragging = false
                                }
                            }
                        }


                        // --- boutons transport piste ---
                        Row {
                            spacing: 8
                            anchors.verticalCenter: parent.verticalCenter

                            // REC (vide → enregistre)
                            Rectangle {
                                width: 84; height: 56; radius: 6
                                color: isRec ? "#3a1512" : (isArmed ? "#2a2214" : "#1b2126")
                                border.color: isRec ? "#e05545"
                                              : (isArmed ? "#e8b84b"
                                                 : (isEmpty ? "#7a3b32" : "#39434b"))
                                border.width: (isRec || isArmed) ? 2 : 1
                                SequentialAnimation on opacity {
                                    running: isArmed
                                    loops: Animation.Infinite
                                    NumberAnimation { to: 0.45; duration: 420 }
                                    NumberAnimation { to: 1.0;  duration: 420 }
                                    onRunningChanged: if (!running) parent.opacity = 1
                                }
                                Text { anchors.centerIn: parent
                                       text: isArmed ? "⏳ ARMÉ" : "● REC"
                                       color: isRec ? "#ff6a5a"
                                              : (isArmed ? "#e8b84b"
                                                 : (isEmpty ? "#e05545" : "#5c666e"))
                                       font.pixelSize: 13; font.bold: true }
                                TapHandler {
                                    enabled: isEmpty || isArmed
                                    margin: 8
                                    gesturePolicy: TapHandler.ReleaseWithinBounds
                                    onTapped: page.trackCtl(index, "rec")
                                }
                            }
                            // PLAY (finalise l'enregistrement)
                            Rectangle {
                                width: 84; height: 56; radius: 6
                                color: isRec ? "#142a19" : "#1b2126"
                                border.color: isRec ? "#4cc470" : "#39434b"
                                border.width: isRec ? 2 : 1
                                Text { anchors.centerIn: parent; text: "▶ PLAY"
                                       color: isRec ? "#4cc470" : "#5c666e"
                                       font.pixelSize: 13; font.bold: true }
                                TapHandler {
                                    enabled: isRec
                                    margin: 8
                                    gesturePolicy: TapHandler.ReleaseWithinBounds
                                    onTapped: page.trackCtl(index, "play")
                                }
                            }
                            // MUTE / UNMUTE (activer/désactiver la couche)
                            Rectangle {
                                width: 84; height: 56; radius: 6
                                color: muted ? "#2a2214" : "#1b2126"
                                border.color: muted ? "#e8b84b" : (isPlay ? "#39434b" : "#22282e")
                                border.width: muted ? 2 : 1
                                Text { anchors.centerIn: parent
                                       text: muted ? "MUTED" : "ON"
                                       color: muted ? "#e8b84b" : (isPlay ? "#c9c4b8" : "#3a434b")
                                       font.pixelSize: 13; font.bold: true }
                                TapHandler {
                                    enabled: isPlay
                                    margin: 8
                                    gesturePolicy: TapHandler.ReleaseWithinBounds
                                    onTapped: page.trackCtl(index, muted ? "unmute" : "mute")
                                }
                            }
                            // CLEAR piste : appui long avec anneau
                            HoldButton {
                                width: 72; height: 56
                                label: "✕"
                                fontSize: 20
                                bg: "#1b2126"
                                accent: "#f2796a"
                                enabled: !isEmpty && !isRec
                                onTriggered: page.trackCtl(index, "clear")
                            }
                        }
                    }
                }
            }
            }
        }

        Text {
            text: "● REC (piste vide) → ▶ PLAY fige la couche · la 1re piste donne la longueur · "
                  + "ON/MUTE active/désactive · ✕ efface · sortie sur les tranches P1/P2 du MIXER"
            color: "#5c666e"; font.pixelSize: 10; font.letterSpacing: 1
            width: parent.width; wrapMode: Text.WordWrap
        }
    }
}
