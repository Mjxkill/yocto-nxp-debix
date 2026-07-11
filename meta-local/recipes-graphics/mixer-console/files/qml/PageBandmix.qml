// V13-BANDMIX — Page AUTO MIX : assistant de sonorisation de groupe.
// 1) rôle par tranche  2) MESURER chaque source (12 s)  3) CALCULER LE MIX
// (gains/gates/comps/faders posés par rôle)  4) VERROUILLER L'ÉQUILIBRE
// (référence 30 s, retouches incluses)  5) LIVE : keeper lent ±3 dB.
import QtQuick

Item {
    id: page
    signal duganSettings()   // V13 : ouvre le panneau réglages Dugan (main.qml)

    readonly property var roleKeys: ["off","lead","choir","kick","snare",
                                     "drums","bass","guitar","keys","line"]
    readonly property var roleNames: ["—","VOIX LEAD","CHŒURS","GR. CAISSE",
                                      "C. CLAIRE","BATTERIE","BASSE",
                                      "GUITARE","CLAVIER","LIGNE"]
    function stripName(i) { return i < 8 ? "M" + (i + 1) : "U" + (i - 7); }

    property var st: null      // bandmix_status
    property var vf: null      // get_vfocus (place à la voix)
    property bool vfDrag: false
    onVisibleChanged: if (visible) refresh()
    Timer { interval: 500; running: page.visible; repeat: true
            onTriggered: page.refresh() }
    Timer { interval: 250; running: page.visible; repeat: true
            onTriggered: mixer.call({ op: "get_vfocus" }, function(r) {
                if (r.ok && !page.vfDrag) page.vf = r; }) }
    function refresh() {
        mixer.call({ op: "bandmix_status" }, function(r) {
            if (r.ok) page.st = r;
        });
    }
    function setRole(src, dir) {
        if (!st) return;
        var cur = roleKeys.indexOf(st.chans[src].role);
        var nx = (cur + dir + roleKeys.length) % roleKeys.length;
        mixer.call({ op: "bandmix_role", src: src, role: roleKeys[nx] },
                   function() { page.refresh(); });
    }

    Column {
        anchors.fill: parent
        anchors.margins: 14
        spacing: 8

        // ================= BANDEAU PILOTAGE =================
        Rectangle {
            width: parent.width; height: 74
            color: "#171c21"; radius: 8; border.color: "#060809"
            Item {
                anchors.fill: parent; anchors.margins: 12
                Column {
                    spacing: 3
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    Text { text: "AUTO MIX"; color: "#e5a13c"; font.pixelSize: 13
                           font.bold: true; font.letterSpacing: 3 }
                    Text {
                        text: {
                            if (!page.st) return "…";
                            if (page.st.measuring >= 0)
                                return "mesure " + page.stripName(page.st.measuring)
                                       + "  " + page.st.meas_elapsed + "/12 s";
                            if (page.st.locking) return "capture de l'équilibre… (30 s)";
                            if (page.st.live) return "LIVE — équilibre tenu";
                            return page.st.ref_valid ? "équilibre verrouillé, LIVE prêt"
                                                     : "assigne les rôles puis mesure";
                        }
                        color: "#8b959d"; font.pixelSize: 10
                    }
                }
                Row {
                id: bmxBtns
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                spacing: 12
                Rectangle {
                    width: 130; height: 46; radius: 6
                    anchors.verticalCenter: parent.verticalCenter
                    color: "#2a2214"; border.color: "#e5a13c"
                    Text { anchors.centerIn: parent; text: "CALCULER\nLE MIX"
                           horizontalAlignment: Text.AlignHCenter
                           color: "#e5a13c"; font.pixelSize: 11; font.bold: true }
                    TapHandler {
                        gesturePolicy: TapHandler.ReleaseWithinBounds
                        onTapped: mixer.call({ op: "bandmix_calc" },
                                             function() { page.refresh(); })
                    }
                }
                Rectangle {
                    width: 130; height: 46; radius: 6
                    anchors.verticalCenter: parent.verticalCenter
                    color: page.st && page.st.locking ? "#2a2c10" : "#1b2126"
                    border.color: "#8b959d"
                    Text { anchors.centerIn: parent; text: "VERROUILLER\nL'ÉQUILIBRE"
                           horizontalAlignment: Text.AlignHCenter
                           color: "#c9c4b8"; font.pixelSize: 11; font.bold: true }
                    TapHandler {
                        gesturePolicy: TapHandler.ReleaseWithinBounds
                        onTapped: mixer.call({ op: "bandmix_lock" },
                                             function() { page.refresh(); })
                    }
                }
                // V13.5 : AUTOMIX LIVE — un seul bouton, aucun réglage,
                // continu. Équilibre auto référencé sur la voix.
                Rectangle {
                    width: 150; height: 46; radius: 6
                    anchors.verticalCenter: parent.verticalCenter
                    property bool on: page.st !== null && page.st.autolive === 1
                    color: on ? "#142a19" : "#1b2126"
                    border.color: on ? "#4cc470" : "#4cc470"
                    border.width: on ? 2 : 1
                    Text { anchors.centerIn: parent
                           text: parent.on ? "● AUTOMIX LIVE" : "AUTOMIX LIVE"
                           color: parent.on ? "#4cc470" : "#8b959d"
                           font.pixelSize: 12; font.bold: true }
                    TapHandler {
                        gesturePolicy: TapHandler.ReleaseWithinBounds
                        onTapped: mixer.call({ op: "bandmix_autolive",
                                               on: parent.on ? 0 : 1 },
                                             function() { page.refresh(); })
                    }
                }
                Rectangle {
                    width: 96; height: 46; radius: 6
                    anchors.verticalCenter: parent.verticalCenter
                    property bool on: page.st !== null && page.st.live === 1
                    color: on ? "#142a19" : "#1b2126"
                    border.color: on ? "#4cc470" : "#39434b"
                    border.width: on ? 2 : 1
                    Text { anchors.centerIn: parent
                           text: parent.on ? "LIVE ON" : "LIVE OFF"
                           color: parent.on ? "#4cc470" : "#8b959d"
                           font.pixelSize: 11; font.bold: true }
                    TapHandler {
                        gesturePolicy: TapHandler.ReleaseWithinBounds
                        onTapped: mixer.call({ op: "bandmix_live",
                                               on: parent.on ? 0 : 1 },
                                             function() { page.refresh(); })
                    }
                }
                // V13 : le Dugan (parole) vit ici désormais — retiré du master
                Rectangle {
                    width: 96; height: 46; radius: 6
                    anchors.verticalCenter: parent.verticalCenter
                    color: mixer.amxOn ? "#2a2214" : "#1b2126"
                    border.color: mixer.amxOn ? "#e5a13c" : "#39434b"
                    border.width: mixer.amxOn ? 2 : 1
                    Text { anchors.centerIn: parent
                           text: mixer.amxOn ? "DUGAN ON" : "DUGAN OFF"
                           color: mixer.amxOn ? "#e5a13c" : "#8b959d"
                           font.pixelSize: 11; font.bold: true }
                    TapHandler {
                        gesturePolicy: TapHandler.ReleaseWithinBounds
                        onTapped: mixer.call({ op: "set_automix_cfg",
                                               on: mixer.amxOn ? 0 : 1 },
                                             function() {})
                    }
                }
                Rectangle {
                    width: 40; height: 46; radius: 6
                    anchors.verticalCenter: parent.verticalCenter
                    color: "#1b2126"; border.color: "#39434b"
                    Text { anchors.centerIn: parent; text: "⚙"
                           color: "#8b959d"; font.pixelSize: 15 }
                    TapHandler {
                        gesturePolicy: TapHandler.ReleaseWithinBounds
                        onTapped: page.duganSettings()
                    }
                }
                }
            }
        }

        // ========= V13-VFOCUS : « PLACE À LA VOIX » (unmasking) =========
        Rectangle {
            width: parent.width; height: 56
            color: "#171c21"; radius: 8; border.color: "#060809"
            Row {
                anchors.fill: parent; anchors.margins: 10; spacing: 14
                Rectangle {
                    width: 168; height: 36; radius: 5
                    anchors.verticalCenter: parent.verticalCenter
                    property bool on: page.vf !== null && page.vf.on === 1
                    color: on ? "#2a2214" : "#1b2126"
                    border.color: on ? "#e5a13c" : "#39434b"
                    border.width: on ? 2 : 1
                    Text { anchors.centerIn: parent
                           text: "PLACE À LA VOIX"
                           color: parent.on ? "#e5a13c" : "#8b959d"
                           font.pixelSize: 10; font.bold: true
                           font.letterSpacing: 2 }
                    TapHandler {
                        gesturePolicy: TapHandler.ReleaseWithinBounds
                        onTapped: mixer.call({ op: "set_vfocus",
                                               on: parent.on ? 0 : 1 },
                                             function() {})
                    }
                }
                Column {
                    spacing: 3
                    anchors.verticalCenter: parent.verticalCenter
                    Text { text: "AMOUNT  " + (page.vf ? page.vf.amount : 50)
                           color: "#5c666e"; font.pixelSize: 8
                           font.letterSpacing: 2 }
                    Rectangle {
                        width: 200; height: 20; radius: 10; color: "#0b0e11"
                        Rectangle {
                            height: parent.height; radius: 10
                            width: parent.width * (page.vf ? page.vf.amount : 50) / 100
                            color: "#39434b"
                            Rectangle { width: 4; height: parent.height
                                        anchors.right: parent.right
                                        color: "#e5a13c" }
                        }
                        MouseArea {
                            anchors.fill: parent
                            preventStealing: true
                            onPressed: page.vfDrag = true
                            onReleased: page.vfDrag = false
                            onCanceled: page.vfDrag = false
                            onPositionChanged: (m) => {
                                if (!pressed) return;
                                var v = Math.round(Math.max(0, Math.min(1,
                                        m.x / width)) * 100);
                                var d = page.vf; if (d) { d.amount = v; page.vf = d; }
                                mixer.call({ op: "set_vfocus", amount: v },
                                           function() {});
                            }
                        }
                    }
                }
                // 5 barres de cut temps réel (la musique s'écarte)
                Row {
                    spacing: 5
                    anchors.verticalCenter: parent.verticalCenter
                    Repeater {
                        model: ["250", "500", "1k", "2k", "4k"]
                        Column {
                            spacing: 2
                            Rectangle {
                                width: 26; height: 22; radius: 3
                                color: "#0b0e11"
                                Rectangle {
                                    anchors.bottom: parent.bottom
                                    width: parent.width; radius: 3
                                    height: parent.height * Math.min(1,
                                        (page.vf && page.vf.cuts_db
                                         ? page.vf.cuts_db[index] : 0) /
                                        (page.vf ? Math.max(1, page.vf.max_cut_db) : 4.5))
                                    color: "#e8b84b"
                                    Behavior on height { NumberAnimation { duration: 220; easing.type: Easing.OutQuad } }
                                }
                            }
                            Text { text: modelData
                                   anchors.horizontalCenter: parent.horizontalCenter
                                   color: "#5c666e"; font.pixelSize: 7 }
                        }
                    }
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: page.vf && page.vf.active === 1 ? "♪ voix détectée" : ""
                    color: "#4cc470"; font.pixelSize: 10
                }
            }
        }

        // ================= 16 TRANCHES =================
        Flickable {
            width: parent.width
            height: parent.height - 74 - 56 - 26 - 3*8
            contentHeight: bmxCol.height
            clip: true
            Column {
                id: bmxCol
                width: parent.width
                spacing: 5
                Repeater {
                    model: 16
                    Rectangle {
                        property var ch: page.st && index < page.st.chans.length
                                         ? page.st.chans[index] : null
                        property bool active: ch !== null && ch.role !== "off"
                        property bool measuring: page.st !== null
                                                 && page.st.measuring === index
                        width: parent.width; height: 52; radius: 6
                        color: measuring ? "#2a1512" : (active ? "#171c21" : "#14181c")
                        border.color: measuring ? "#e05545"
                                      : (active ? "#39434b" : "#22282e")

                        Row {
                            anchors.fill: parent; anchors.margins: 7; spacing: 8
                            Rectangle {
                                width: 52; height: 38; radius: 5
                                anchors.verticalCenter: parent.verticalCenter
                                color: "#1b2126"; border.color: "#39434b"
                                Text { anchors.centerIn: parent
                                       text: page.stripName(index)
                                       color: "#e5a13c"; font.pixelSize: 13
                                       font.bold: true }
                            }
                            Rectangle {
                                width: 36; height: 38; radius: 5
                                anchors.verticalCenter: parent.verticalCenter
                                color: "#1b2126"; border.color: "#39434b"
                                Text { anchors.centerIn: parent; text: "‹"
                                       color: "#c9c4b8"; font.pixelSize: 18 }
                                TapHandler {
                                    gesturePolicy: TapHandler.ReleaseWithinBounds
                                    onTapped: page.setRole(index, -1)
                                }
                            }
                            Rectangle {
                                width: 118; height: 38; radius: 5
                                anchors.verticalCenter: parent.verticalCenter
                                color: "#0f1216"
                                border.color: active ? "#e5a13c" : "#22282e"
                                Text { anchors.centerIn: parent
                                       text: {
                                           if (!ch || !ch.role) return "—";
                                           var k = page.roleKeys.indexOf(ch.role);
                                           return k >= 0 ? page.roleNames[k]
                                                         : String(ch.role);
                                       }
                                       color: active ? "#e9e5da" : "#3a434b"
                                       font.pixelSize: 11; font.bold: true }
                            }
                            Rectangle {
                                width: 36; height: 38; radius: 5
                                anchors.verticalCenter: parent.verticalCenter
                                color: "#1b2126"; border.color: "#39434b"
                                Text { anchors.centerIn: parent; text: "›"
                                       color: "#c9c4b8"; font.pixelSize: 18 }
                                TapHandler {
                                    gesturePolicy: TapHandler.ReleaseWithinBounds
                                    onTapped: page.setRole(index, 1)
                                }
                            }
                            // MESURER / résultat
                            Rectangle {
                                width: 128; height: 38; radius: 5
                                anchors.verticalCenter: parent.verticalCenter
                                visible: active
                                color: measuring ? "#3a1512"
                                       : (ch && ch.done ? "#142a19" : "#2a2214")
                                border.color: measuring ? "#e05545"
                                       : (ch && ch.done ? "#4cc470" : "#e5a13c")
                                Text {
                                    anchors.centerIn: parent
                                    text: measuring
                                          ? "● " + page.st.meas_elapsed + "/12 s"
                                          : (ch && ch.done
                                             ? "✓ " + ch.rms_db.toFixed(1) + " dB"
                                             : "MESURER")
                                    color: measuring ? "#ff6a5a"
                                           : (ch && ch.done ? "#4cc470" : "#e5a13c")
                                    font.pixelSize: 11; font.bold: true
                                    font.family: measuring || (ch && ch.done)
                                                 ? "monospace" : ""
                                }
                                TapHandler {
                                    enabled: !measuring
                                    gesturePolicy: TapHandler.ReleaseWithinBounds
                                    onTapped: mixer.call({ op: "bandmix_measure",
                                                           src: index },
                                                         function() { page.refresh(); })
                                }
                            }
                            // keeper ±3 dB (centre = 0)
                            Column {
                                visible: active
                                spacing: 2
                                anchors.verticalCenter: parent.verticalCenter
                                Text { text: "KEEPER "
                                             + (ch ? (ch.keeper_db >= 0 ? "+" : "")
                                               + ch.keeper_db.toFixed(1) : "0")
                                             + " dB"
                                       color: "#5c666e"; font.pixelSize: 9
                                       font.letterSpacing: 2 }
                                Rectangle {
                                    width: 150; height: 10; radius: 5
                                    color: "#0b0e11"
                                    Rectangle {   /* repère central */
                                        x: parent.width / 2 - 1; width: 2
                                        height: parent.height; color: "#39434b"
                                    }
                                    Rectangle {
                                        property real k: ch ? ch.keeper_db : 0
                                        x: k >= 0 ? parent.width / 2
                                           : parent.width / 2 + k / 3 * (parent.width / 2)
                                        width: Math.abs(k) / 3 * (parent.width / 2)
                                        height: parent.height; radius: 5
                                        color: k >= 0 ? "#4cc470" : "#e8b84b"
                                        // le keeper corrige par pas (1 Hz) :
                                        // glissement doux entre deux pas
                                        Behavior on x { NumberAnimation { duration: 350; easing.type: Easing.OutCubic } }
                                        Behavior on width { NumberAnimation { duration: 350; easing.type: Easing.OutCubic } }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        Text {
            text: "1· rôle par tranche  2· MESURER chaque source seule (12 s)  3· CALCULER  4· le groupe joue, ajuste si besoin, VERROUILLER  5· LIVE ON — la console tient l'équilibre (±3 dB, priorité voix)"
            color: "#5c666e"; font.pixelSize: 10; font.letterSpacing: 1
            width: parent.width; wrapMode: Text.WordWrap
        }
    }
}
