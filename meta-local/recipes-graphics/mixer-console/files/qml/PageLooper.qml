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
            { track: 0, state: "empty", len_s: 0, muted: 0, src_a: 0, src_b: -1, peak: 0 },
            { track: 1, state: "empty", len_s: 0, muted: 0, src_a: 1, src_b: -1, peak: 0 },
            { track: 2, state: "empty", len_s: 0, muted: 0, src_a: 2, src_b: -1, peak: 0 },
            { track: 3, state: "empty", len_s: 0, muted: 0, src_a: 3, src_b: -1, peak: 0 },
            { track: 4, state: "empty", len_s: 0, muted: 0, src_a: 4, src_b: -1, peak: 0 },
            { track: 5, state: "empty", len_s: 0, muted: 0, src_a: 5, src_b: -1, peak: 0 }
        ]
    })

    onVisibleChanged: if (visible) refresh()
    Timer { interval: 250; running: page.visible; repeat: true; onTriggered: page.refresh() }

    function refresh() {
        mixer.call({ op: "looper_status" }, function(r) {
            if (r.ok) page.status = r;
        });
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
            Row {
                anchors.fill: parent; anchors.margins: 12; spacing: 14

                Column {
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 3
                    Text { text: "LOOPER"; color: "#e5a13c"; font.pixelSize: 13
                           font.bold: true; font.letterSpacing: 3 }
                    Text {
                        text: page.status.master_len_s > 0
                              ? "boucle " + page.status.master_len_s.toFixed(2) + " s   "
                                + page.status.pos_s.toFixed(1) + " s"
                              : "aucune boucle — enregistrez une 1re piste"
                        color: "#8b959d"; font.pixelSize: 11; font.family: "monospace"
                    }
                }

                // barre de position maître
                Rectangle {
                    width: 200; height: 10; radius: 5
                    anchors.verticalCenter: parent.verticalCenter
                    color: "#0b0e11"
                    Rectangle {
                        height: parent.height; radius: 5
                        width: page.status.master_len_s > 0
                               ? parent.width * (page.status.pos_s / page.status.master_len_s) : 0
                        color: page.status.run ? "#4cc470" : "#5c666e"
                    }
                }

                // V12-VU : vumètre MASTER looper (somme des pistes)
                Column {
                    spacing: 3
                    anchors.verticalCenter: parent.verticalCenter
                    Text { text: "MASTER"; color: "#5c666e"; font.pixelSize: 8
                           font.letterSpacing: 2 }
                    Rectangle {
                        width: 110; height: 12; radius: 6; color: "#0b0e11"
                        Rectangle {
                            height: parent.height; radius: 6
                            width: {
                                var f = (page.status.master_peak || 0) / 2147483647.0;
                                if (f <= 0) return 0;
                                var db = 20 * Math.log(f) / Math.LN10;
                                return parent.width * Math.max(0, Math.min(1, (db + 48) / 48));
                            }
                            color: (page.status.master_peak || 0) > 1932735283
                                   ? "#e05545" : "#4cc470"   /* rouge > -0,9 dBFS */
                        }
                    }
                }

                Item { width: parent.width - 744; height: 1 }

                Repeater {
                    model: [
                        ["▶ PLAY ALL",  "play_all",  "#4cc470", "#2a2214"],
                        ["■ STOP ALL",  "stop_all",  "#8b959d", "#1b2126"],
                        ["✕ CLEAR ALL", "clear_all", "#f2796a", "#2a1512"]
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
                        spacing: 14

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

                        // --- VU crête + durée ---
                        Column {
                            width: 150
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 6
                            Rectangle {
                                width: parent.width; height: 12; radius: 6
                                color: "#0b0e11"
                                Rectangle {
                                    height: parent.height; radius: 6
                                    // dB : -48..0 → 0..1 — vivant en PLAY
                                    // (restitution) ET en REC (entrée)
                                    width: {
                                        if (!tr || (!isPlay && !isRec) || (isPlay && muted)) return 0;
                                        var frac = tr.peak / 2147483647.0;
                                        if (frac <= 0) return 0;
                                        var db = 20 * Math.log(frac) / Math.LN10;
                                        var n = (db + 48) / 48;
                                        return parent.width * Math.max(0, Math.min(1, n));
                                    }
                                    color: isRec ? "#e05545" : trackRow.accent
                                }
                            }
                            Text {
                                text: (tr && tr.len_s > 0)
                                      ? tr.len_s.toFixed(2) + " s"
                                      : (isRec ? "● enregistre…"
                                         : (isArmed ? "⏳ départ au tour" : "—"))
                                color: isRec ? "#e05545"
                                       : (isArmed ? "#e8b84b" : "#8b959d")
                                font.pixelSize: 12; font.family: "monospace"
                            }
                        }

                        Item { width: parent.width - 74 - 128 - 150 - 4*14 - 380; height: 1 }

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
                            // CLEAR
                            Rectangle {
                                width: 72; height: 56; radius: 6
                                color: "#1b2126"
                                border.color: isEmpty ? "#22282e" : "#7a3b32"
                                Text { anchors.centerIn: parent; text: "✕"
                                       color: isEmpty ? "#3a434b" : "#f2796a"
                                       font.pixelSize: 20; font.bold: true }
                                TapHandler {
                                    enabled: !isEmpty && !isRec
                                    margin: 8
                                    gesturePolicy: TapHandler.ReleaseWithinBounds
                                    onTapped: page.trackCtl(index, "clear")
                                }
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
