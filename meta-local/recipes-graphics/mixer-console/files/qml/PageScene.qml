// V13-SCENES — Page SCÈNE : poste de pilotage des automatismes + profils.
// 4 gros boutons ACTIFS (état + infos live + VU) : ANTI-LARSEN, AUTOMIX
// (tri-état OFF/MUSIQUE/VOIX), MASTERING, VOIX DEVANT. Puis 6 slots de
// scène : SAUVER = snapshot complet de la table, RAPPEL = bascule
// instantanée sans coupure audio.
import QtQuick

Item {
    id: page

    // ---- états live ----
    property bool alOn: false
    property int  alNotches: 0
    property string alLast: ""
    property bool duganOn: false
    property bool liveOn: false
    property real amxCorr: 0        // correction max courante (dB)
    property bool mastOn: false
    property bool mastChain: false
    property bool vfOn: false
    property bool vfActive: false
    property real vfTotal: 0        // somme des cuts (dB)
    property var scenes: []
    // V13-E2 : confirmation RAPPEL (anti-fausse-manip live) + nommage
    property int armedRecall: -1
    property int nameSlot: -1
    property string nameEdit: ""
    Timer { id: disarm; interval: 3000; onTriggered: page.armedRecall = -1 }

    onVisibleChanged: if (visible) { poll(); pollLarsen(); loadScenes(); }
    Timer { interval: 400; running: page.visible; repeat: true
            onTriggered: page.poll() }
    Timer { interval: 1000; running: page.visible; repeat: true
            onTriggered: page.pollLarsen() }

    function poll() {
        mixer.call({ op: "get_automix" }, function(r) {
            if (r.ok) {
                page.duganOn = r.on === 1;
                var m = 0;
                for (var i = 0; i < r.gains_db.length; i++)
                    if (r.members[i] === 1 && r.gains_db[i] < m)
                        m = r.gains_db[i];
                if (page.duganOn) page.amxCorr = m;
            }
        });
        mixer.call({ op: "bandmix_status" }, function(r) {
            if (r.ok) {
                page.liveOn = r.live === 1;
                if (page.liveOn) {
                    var m = 0;
                    for (var i = 0; i < r.chans.length; i++)
                        if (Math.abs(r.chans[i].keeper_db) > Math.abs(m))
                            m = r.chans[i].keeper_db;
                    page.amxCorr = m;
                }
            }
        });
        mixer.call({ op: "get_insert_bypass" }, function(r) {
            if (r.ok) { page.mastOn = r.mastering_on === 1;
                        page.mastChain = r.chain === 1; }
        });
        mixer.call({ op: "get_vfocus" }, function(r) {
            if (r.ok) {
                page.vfOn = r.on === 1;
                page.vfActive = r.active === 1;
                var t = 0;
                for (var i = 0; i < r.cuts_db.length; i++) t += r.cuts_db[i];
                page.vfTotal = t;
            }
        });
    }

    function xhr(method, url, body, cb) {
        var q = new XMLHttpRequest();
        q.onreadystatechange = function() {
            if (q.readyState !== XMLHttpRequest.DONE) return;
            try { cb(JSON.parse(q.responseText)); } catch (e) { cb({ ok: false }); }
        };
        q.open(method, "http://127.0.0.1:8080" + url);
        if (body) q.setRequestHeader("Content-Type", "application/json");
        q.send(body || undefined);
    }
    function pollLarsen() {
        xhr("GET", "/api/larsen", null, function(r) {
            if (!r.ok) { page.alOn = false; page.alNotches = 0; return; }
            page.alOn = r.enable === 1;
            page.alNotches = r.notches.length;
            page.alLast = r.notches.length > 0
                          ? r.notches[r.notches.length - 1].freq + " Hz" : "";
        });
    }
    function loadScenes() {
        mixer.call({ op: "scene_list" }, function(r) {
            if (r.ok) page.scenes = r.scenes;
        });
    }
    function cycleAutomix() {
        // OFF → MUSIQUE → VOIX → OFF (exclusif)
        if (!page.liveOn && !page.duganOn) {
            mixer.call({ op: "bandmix_live", on: 1 }, function() { page.poll(); });
            mixer.call({ op: "set_automix_cfg", on: 0 }, function() {});
        } else if (page.liveOn) {
            mixer.call({ op: "bandmix_live", on: 0 }, function() {});
            mixer.call({ op: "set_automix_cfg", on: 1 }, function() { page.poll(); });
        } else {
            mixer.call({ op: "set_automix_cfg", on: 0 }, function() { page.poll(); });
        }
    }

    // gros bouton actif réutilisable
    component BigBtn: Rectangle {
        id: bb
        property string title: ""
        property string state_: "OFF"
        property string info: ""
        property bool on: false
        property color accent: "#e5a13c"
        property real vu: 0          // 0..1 (barre du bas)
        signal tapped()
        width: 100; height: 116; radius: 10
        color: on ? Qt.darker(accent, 5.5) : "#14181c"
        border.color: on ? accent : "#39434b"
        border.width: on ? 2 : 1
        Column {
            anchors.centerIn: parent
            spacing: 5
            Text { anchors.horizontalCenter: parent.horizontalCenter
                   text: bb.title; color: bb.on ? bb.accent : "#8b959d"
                   font.pixelSize: 12; font.bold: true; font.letterSpacing: 2 }
            Text { anchors.horizontalCenter: parent.horizontalCenter
                   text: bb.state_; color: bb.on ? "#e9e5da" : "#5c666e"
                   font.pixelSize: 17; font.bold: true }
            Text { anchors.horizontalCenter: parent.horizontalCenter
                   text: bb.info; color: "#8b959d"; font.pixelSize: 10
                   font.family: "monospace" }
        }
        Rectangle {   // mini-VU d'activité
            anchors.bottom: parent.bottom; anchors.bottomMargin: 6
            anchors.horizontalCenter: parent.horizontalCenter
            width: parent.width - 24; height: 6; radius: 3
            color: "#0b0e11"
            Rectangle { height: parent.height; radius: 3
                        width: parent.width * Math.max(0, Math.min(1, bb.vu))
                        color: bb.accent }
        }
        TapHandler {
            gesturePolicy: TapHandler.ReleaseWithinBounds
            onTapped: bb.tapped()
        }
    }

    Column {
        anchors.fill: parent
        anchors.margins: 14
        spacing: 12

        Text { text: "SCÈNE · AUTOMATISMES"; color: "#e5a13c"
               font.pixelSize: 13; font.bold: true; font.letterSpacing: 3 }

        // ================= 4 GROS BOUTONS =================
        Row {
            width: parent.width
            spacing: 12
            BigBtn {
                width: (parent.width - 3*12) / 4
                title: "ANTI-LARSEN"
                on: page.alOn
                accent: "#e05545"
                state_: page.alOn ? "ON" : "OFF"
                info: page.alNotches > 0
                      ? page.alNotches + " notch · " + page.alLast
                      : (page.alOn ? "veille — rien" : "désactivé")
                vu: Math.min(1, page.alNotches / 6)
                onTapped: page.xhr("POST", "/api/larsen",
                                   JSON.stringify({ enable: page.alOn ? 0 : 1 }),
                                   function() { page.pollLarsen(); })
            }
            BigBtn {
                width: (parent.width - 3*12) / 4
                title: "AUTOMIX"
                on: page.liveOn || page.duganOn
                accent: page.duganOn ? "#e5a13c" : "#4cc470"
                state_: page.liveOn ? "MUSIQUE" : (page.duganOn ? "VOIX" : "OFF")
                info: (page.liveOn || page.duganOn)
                      ? "corr " + (page.amxCorr >= 0 ? "+" : "")
                        + page.amxCorr.toFixed(1) + " dB"
                      : "tap : musique/voix"
                vu: Math.min(1, Math.abs(page.amxCorr) / 6)
                onTapped: page.cycleAutomix()
            }
            BigBtn {
                width: (parent.width - 3*12) / 4
                title: "MASTERING"
                on: page.mastOn
                accent: "#5aa9e6"
                state_: page.mastOn ? "ON" : "OFF"
                info: page.mastChain ? "chaîne prête" : "aucune chaîne"
                vu: {
                    var e = mixer.mlEnvL;
                    if (!page.mastOn || !e || e.length === 0) return 0;
                    return Math.max(0, Math.min(1, e[e.length - 1]));
                }
                onTapped: mixer.call({ op: "set_insert_bypass",
                                       on: page.mastOn ? 0 : 1 },
                                     function() { page.poll(); })
            }
            BigBtn {
                width: (parent.width - 3*12) / 4
                title: "VOIX DEVANT"
                on: page.vfOn
                accent: "#b3a5f0"
                state_: page.vfOn ? (page.vfActive ? "♪ ACTIF" : "ON") : "OFF"
                info: page.vfOn ? "creuse " + page.vfTotal.toFixed(1) + " dB"
                                : "place à la voix"
                vu: Math.min(1, page.vfTotal / 15)
                onTapped: mixer.call({ op: "set_vfocus",
                                       on: page.vfOn ? 0 : 1 },
                                     function() { page.poll(); })
            }
        }

        Text { text: "PROFILS  ·  SAUVER fige toute la table — RAPPEL bascule instantanément"
               color: "#5c666e"; font.pixelSize: 10; font.letterSpacing: 1 }

        // ================= 6 SCÈNES =================
        Flickable {
            width: parent.width
            height: parent.height - 20 - 116 - 16 - 4*12
            contentHeight: scnCol.height
            clip: true
            Column {
                id: scnCol
                width: parent.width
                spacing: 7
                Repeater {
                    model: 6
                    Rectangle {
                        property var sc: index < page.scenes.length
                                         ? page.scenes[index] : null
                        property bool used: sc !== null && sc.used === 1
                        width: parent.width; height: 62; radius: 8
                        color: used ? "#171c21" : "#14181c"
                        border.color: used ? "#39434b" : "#22282e"
                        Row {
                            anchors.fill: parent; anchors.margins: 9
                            spacing: 12
                            Rectangle {
                                width: 44; height: 44; radius: 22
                                anchors.verticalCenter: parent.verticalCenter
                                color: used ? "#e5a13c" : "#1b2126"
                                Text { anchors.centerIn: parent
                                       text: index + 1
                                       color: used ? "#0b0e11" : "#5c666e"
                                       font.pixelSize: 18; font.bold: true }
                            }
                            Text {
                                width: parent.width - 44 - 150 - 150 - 4*12
                                anchors.verticalCenter: parent.verticalCenter
                                text: sc ? sc.name : "Scène " + (index + 1)
                                color: used ? "#e9e5da" : "#3a434b"
                                font.pixelSize: 15; font.bold: true
                                elide: Text.ElideRight
                            }
                            Rectangle {
                                property bool armed: page.armedRecall === index
                                width: 150; height: 44; radius: 6
                                anchors.verticalCenter: parent.verticalCenter
                                color: armed ? "#3a1512" : (used ? "#2a2214" : "#181b1e")
                                border.color: armed ? "#e05545"
                                              : (used ? "#e5a13c" : "#22282e")
                                border.width: armed ? 2 : 1
                                Text { anchors.centerIn: parent
                                       text: parent.armed ? "CONFIRMER ?" : "RAPPEL"
                                       color: parent.armed ? "#ff6a5a"
                                              : (used ? "#e5a13c" : "#3a434b")
                                       font.pixelSize: 12; font.bold: true }
                                TapHandler {
                                    enabled: used
                                    gesturePolicy: TapHandler.ReleaseWithinBounds
                                    onTapped: {
                                        // V13-E2 : 2 taps (anti-fausse-manip)
                                        if (page.armedRecall !== index) {
                                            page.armedRecall = index;
                                            disarm.restart();
                                            return;
                                        }
                                        page.armedRecall = -1;
                                        disarm.stop();
                                        page.xhr("POST", "/api/scene/recall",
                                            JSON.stringify({ slot: index }),
                                            function() { page.poll(); page.loadScenes(); });
                                    }
                                }
                            }
                            Rectangle {
                                width: 150; height: 44; radius: 6
                                anchors.verticalCenter: parent.verticalCenter
                                color: "#142a19"; border.color: "#2c5c3c"
                                Text { anchors.centerIn: parent; text: "SAUVER"
                                       color: "#4cc470"; font.pixelSize: 12
                                       font.bold: true }
                                TapHandler {
                                    gesturePolicy: TapHandler.ReleaseWithinBounds
                                    onTapped: {
                                        // V13-E2 : nommage avant sauvegarde
                                        page.nameSlot = index;
                                        page.nameEdit = sc && sc.used === 1
                                                        ? sc.name : "";
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // ========= V13-E2 : clavier de nommage de scène (overlay) =========
    Rectangle {
        visible: page.nameSlot >= 0
        anchors.fill: parent
        color: "#0e1114"
        z: 300

        Column {
            anchors.centerIn: parent
            spacing: 12
            width: parent.width - 60

            Text {
                text: "NOM DE LA SCÈNE " + (page.nameSlot + 1)
                color: "#e5a13c"; font.pixelSize: 13; font.bold: true
                font.letterSpacing: 3
            }
            Rectangle {
                width: parent.width; height: 52; radius: 6
                color: "#0b0e11"; border.color: "#e5a13c"
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left; anchors.leftMargin: 14
                    text: page.nameEdit + "▏"
                    color: "#e9e5da"; font.pixelSize: 20; font.bold: true
                }
            }
            // rangées de touches
            Repeater {
                model: [
                    "1234567890",
                    "AZERTYUIOP",
                    "QSDFGHJKLM",
                    "WXCVBN-"
                ]
                Row {
                    anchors.horizontalCenter: parent.horizontalCenter
                    spacing: 6
                    Repeater {
                        model: modelData.length
                        Rectangle {
                            property string ch: modelData.charAt(index)
                            width: 62; height: 52; radius: 6
                            color: "#1b2126"; border.color: "#39434b"
                            Text { anchors.centerIn: parent; text: parent.ch
                                   color: "#e9e5da"; font.pixelSize: 17
                                   font.bold: true }
                            TapHandler {
                                gesturePolicy: TapHandler.ReleaseWithinBounds
                                onTapped: if (page.nameEdit.length < 20)
                                              page.nameEdit += parent.ch
                            }
                        }
                    }
                }
            }
            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 8
                Rectangle {
                    width: 170; height: 52; radius: 6
                    color: "#1b2126"; border.color: "#39434b"
                    Text { anchors.centerIn: parent; text: "ESPACE"
                           color: "#8b959d"; font.pixelSize: 12; font.bold: true }
                    TapHandler { gesturePolicy: TapHandler.ReleaseWithinBounds
                        onTapped: if (page.nameEdit.length < 20)
                                      page.nameEdit += " " }
                }
                Rectangle {
                    width: 110; height: 52; radius: 6
                    color: "#1b2126"; border.color: "#39434b"
                    Text { anchors.centerIn: parent; text: "⌫"
                           color: "#c9c4b8"; font.pixelSize: 18 }
                    TapHandler { gesturePolicy: TapHandler.ReleaseWithinBounds
                        onTapped: page.nameEdit =
                            page.nameEdit.slice(0, -1) }
                }
                Rectangle {
                    width: 130; height: 52; radius: 6
                    color: "#2a1512"; border.color: "#7a3b32"
                    Text { anchors.centerIn: parent; text: "ANNULER"
                           color: "#f2796a"; font.pixelSize: 12; font.bold: true }
                    TapHandler { gesturePolicy: TapHandler.ReleaseWithinBounds
                        onTapped: page.nameSlot = -1 }
                }
                Rectangle {
                    width: 170; height: 52; radius: 6
                    color: "#142a19"; border.color: "#4cc470"
                    Text { anchors.centerIn: parent; text: "SAUVER"
                           color: "#4cc470"; font.pixelSize: 13; font.bold: true }
                    TapHandler {
                        gesturePolicy: TapHandler.ReleaseWithinBounds
                        onTapped: {
                            var nm = page.nameEdit.trim();
                            if (nm === "") nm = "Scène " + (page.nameSlot + 1);
                            page.xhr("POST", "/api/scene/save",
                                JSON.stringify({ slot: page.nameSlot,
                                                 name: nm }),
                                function() { page.loadScenes(); });
                            page.nameSlot = -1;
                        }
                    }
                }
            }
        }
    }
}
