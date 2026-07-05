// V10-NATIVE N1 — page MIXER : banques 8 tranches + master (meters réels).
// Scène logique 1280x800 tournée 270° sur la dalle physique 800x1280 @44 Hz.
import QtQuick

Window {
    id: root
    visible: true
    visibility: Window.FullScreen
    color: "#0c0f12"

    CalibrationOverlay { }

    Item {
        id: scene
        width: 1280
        height: 800
        anchors.centerIn: parent
        rotation: 270

        property int currentPage: 0
        // V10-N8 : informe le client — les pollers meters/analyzer/insert
        // ne tournent que sur les pages qui les affichent
        onCurrentPageChanged: mixer.activePage = currentPage
        // ---- banques (layers console, identiques au web) ----
        property int currentBank: 0
        property var banks: [
            { label: "IN DSP",  strips: mkStrips("M", "MIC", "in", 0, 8) },
            { label: "IN USB",  strips: mkStrips("U", "USB", "in", 8, 8) },
            { label: "TÉLÉPHONE", strips: [
                { n: "P1", sub: "TEL IN",  t: "in",  idx: 16 },
                { n: "P2", sub: "TEL IN",  t: "in",  idx: 17 },
                { n: "P1", sub: "TEL OUT", t: "out", idx: 16 },
                { n: "P2", sub: "TEL OUT", t: "out", idx: 17 } ] },
            { label: "OUT DSP", strips: mkStrips("S", "DSP OUT", "out", 0, 8) },
            { label: "OUT USB", strips: mkStrips("U", "USB OUT", "out", 8, 8) }
        ]
        function mkStrips(prefix, sub, type, base, count) {
            const a = [];
            for (let i = 0; i < count; i++)
                a.push({ n: prefix + (i + 1), sub: sub, t: type, idx: base + i });
            return a;
        }

        property int tickN: 0
        /* SYNC bidirectionnelle (retour utilisateur N4) : recharge l'état
         * réel des tranches — à l'entrée de banque et toutes les 2 s
         * (les réglages faits sur la GUI web se répercutent ici). Les
         * tranches en cours de manipulation sont épargnées. */
        function syncBank() {
            if (scene.currentPage !== 0) return;
            const strips = scene.banks[scene.currentBank].strips;
            for (let i = 0; i < 8; i++) {
                const d = i < strips.length ? strips[i] : null;
                const item = stripRep.itemAt(i);
                if (!d || !item) continue;
                if (d.t === "in") {
                    (function(it) {
                        mixer.call({ op: "get_strip_routing", src: d.idx },
                                   function(r) { if (r.ok) it.syncFromRouting(r); });
                    })(item);
                }
            }
            mixer.call({ op: "get_output_gain" }, function(r) {
                if (!r.gains) return;
                for (let i = 0; i < 8; i++) {
                    const d = i < strips.length ? strips[i] : null;
                    const item = stripRep.itemAt(i);
                    if (d && item && d.t === "out" && r.gains[d.idx] !== undefined)
                        item.syncOutGain(r.gains[d.idx] > 0
                            ? 20 * Math.log10(r.gains[d.idx] / 1000.0) : -72);
                }
            });
        }
        onCurrentBankChanged: syncBank()
        Timer { interval: 2000; running: scene.currentPage === 0; repeat: true
                triggeredOnStart: true; onTriggered: scene.syncBank() }

        /* diag réactivité tactile : si le GUI thread se fige > 300 ms,
         * trace la durée au journal (retour utilisateur : « le changement
         * de page bloque le tactile ») */
        property double hbLast: 0
        Timer { interval: 100; running: true; repeat: true
                onTriggered: {
                    const now = Date.now();
                    if (scene.hbLast > 0 && now - scene.hbLast > 300)
                        console.warn("GUI-THREAD-BLOCKED", now - scene.hbLast, "ms, page", scene.currentPage);
                    scene.hbLast = now;
                } }
        // Horloge = le VSYNC lui-même (FrameAnimation) — modèle rAF du web.
        // Ballistique par frame avec dt réel : attack 30 ms, release 110 ms,
        // aiguilles 150 ms. Un QTimer n'est JAMAIS en phase avec le vsync.
        FrameAnimation {
            running: scene.currentPage === 0
            onTriggered: {
                scene.tickN++;
                const dt = Math.min(frameTime, 0.1);
                const kA = 1 - Math.exp(-dt / 0.030);
                const kR = 1 - Math.exp(-dt / 0.110);
                const kN = 1 - Math.exp(-dt / 0.150);
                const iv = mixer.inLevels, ov = mixer.outLevels;
                const strips = scene.banks[scene.currentBank].strips;
                for (let i = 0; i < 8; i++) {
                    const item = stripRep.itemAt(i);
                    if (!item) continue;
                    const d = i < strips.length ? strips[i] : null;
                    const t = d ? ((d.t === "in" ? iv : ov)[d.idx] || 0) : 0;
                    item.level += (t - item.level) * (t > item.level ? kA : kR);
                }
                const l = ov.length > 0 ? ov[0] : 0, r = ov.length > 1 ? ov[1] : 0;
                masterL.level += (l - masterL.level) * (l > masterL.level ? kA : kR);
                masterR.level += (r - masterR.level) * (r > masterR.level ? kA : kR);
                vuL.level += (l - vuL.level) * kN;
                vuR.level += (r - vuR.level) * kN;
                // textes en ROUND-ROBIN : 1 élément par frame (une rafale
                // de 10 re-shapes tous les 8 ticks faisait pomper le
                // QSGRenderThread — observation utilisateur)
                const ph = scene.tickN % 10;
                if (ph < 8) {
                    const item = stripRep.itemAt(ph);
                    if (item) item.updateDbro();
                } else if (ph === 8) {
                    peakLTxt.text = "PEAK L  " + (l > 0.003 ? (l*60-60).toFixed(1) : "-∞");
                } else {
                    peakRTxt.text = "PEAK R  " + (r > 0.003 ? (r*60-60).toFixed(1) : "-∞");
                }
            }
        }

        IntroOverlay { anchors.fill: parent }

        StripFxDrawer {
            id: fxDrawer
            anchors.fill: parent
            anchors.margins: 10
            anchors.topMargin: 66
            anchors.bottomMargin: 54
        }

        Rectangle {   // châssis
            anchors.fill: parent
            radius: 10
            border.color: "#060809"
            gradient: Gradient {
                GradientStop { position: 0.0; color: "#181d22" }
                GradientStop { position: 0.22; color: "#14181c" }
                GradientStop { position: 1.0; color: "#12161a" }
            }

            Column {
                anchors.fill: parent

                // ===== topbar =====
                Rectangle {
                    width: parent.width; height: 60
                    color: "#1a2026"
                    Row {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.leftMargin: 18
                        spacing: 22
                        Column {
                            Text { text: "A.L.A."; color: "#e9e5da"; font.pixelSize: 20; font.bold: true; font.letterSpacing: 5 }
                            Text { text: "AUDIO LIVE ASSISTANT · ELECTROSENS R&D"; color: "#e5a13c"; font.pixelSize: 8; font.letterSpacing: 2 }
                        }
                        Column {
                            Text { text: "LATENCE"; color: "#8b959d"; font.pixelSize: 9; font.letterSpacing: 2 }
                            Text { text: mixer.latencyMs.toFixed(1) + " ms"; color: "#e9e5da"; font.pixelSize: 15; font.family: "monospace" }
                        }
                        Column {
                            Text { text: "XRUN"; color: "#8b959d"; font.pixelSize: 9; font.letterSpacing: 2 }
                            Text { text: mixer.xrun; color: mixer.xrun ? "#e5a13c" : "#4cc470"; font.pixelSize: 15; font.family: "monospace" }
                        }
                    }
                    Row {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.right: parent.right
                        anchors.rightMargin: 18
                        spacing: 10
                        Rectangle {
                            width: 9; height: 9; radius: 5
                            anchors.verticalCenter: parent.verticalCenter
                            color: mixer.connected ? "#4cc470" : "#e05545"
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: mixer.connected ? "LIVE" : "HORS LIGNE"
                            color: "#8b959d"; font.pixelSize: 11; font.letterSpacing: 3
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: fpsMeter.fps + " FPS"
                            color: "#5c666e"; font.pixelSize: 10; font.family: "monospace"
                        }
                    }
                }

                // ===== pages 1-4 (visible-switch, état conservé) =====
                Item {
                    width: parent.width
                    height: parent.height - 60 - 46
                    visible: scene.currentPage !== 0
                    PageEffects   { anchors.fill: parent; visible: scene.currentPage === 1 }
                    PageMastering { anchors.fill: parent; visible: scene.currentPage === 2 }
                    PageRouting   { anchors.fill: parent; visible: scene.currentPage === 3 }
                    PageSystem    { anchors.fill: parent; visible: scene.currentPage === 4 }
                }

                // ===== barre de banques =====
                Rectangle {
                    visible: scene.currentPage === 0
                    width: parent.width; height: 38
                    color: "#161b20"
                    Row {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.leftMargin: 14
                        spacing: 6
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: "BANQUE"; color: "#5c666e"; font.pixelSize: 9; font.letterSpacing: 2
                        }
                        Repeater {
                            model: scene.banks.length
                            Rectangle {
                                width: bkTxt.width + 26; height: 27; radius: 4
                                color: scene.currentBank === index ? "#2a2214" : "#1b2126"
                                border.color: scene.currentBank === index ? "#e5a13c" : "#39434b"
                                Text {
                                    id: bkTxt
                                    anchors.centerIn: parent
                                    text: scene.banks[index].label
                                    color: scene.currentBank === index ? "#e5a13c" : "#8b959d"
                                    font.pixelSize: 11; font.bold: true; font.letterSpacing: 1.5
                                }
                                TapHandler { onTapped: scene.currentBank = index }
                            }
                        }
                    }
                }

                // ===== plan de travail (page MIXER) =====
                Row {
                    visible: scene.currentPage === 0
                    width: parent.width
                    height: parent.height - 60 - 38 - 46

                    // -- 8 tranches --
                    Row {
                        id: bankRow
                        width: parent.width - 320
                        height: parent.height
                        Repeater {
                            id: stripRep
                            model: 8
                            Strip {
                                width: bankRow.width / 8
                                height: bankRow.height
                                property var def: index < scene.banks[scene.currentBank].strips.length
                                                  ? scene.banks[scene.currentBank].strips[index] : null
                                visible: def !== null
                                name: def ? def.n : ""
                                sub: def ? def.sub : ""
                                chanType: def ? def.t : "in"
                                chanIndex: def ? def.idx : 0
                                // level poussé imperativement par onMetersChanged (1 conversion/tick)
                                onNameTapped: { if (def) fxDrawer.open(def.idx, def.t === "out"); }
                                onFaderMoved: (db) => {
                                    if (!def) return;
                                    // V10-N7 : fader IN = gain de tranche
                                    // (sémantique E7.2 restaurée) — la
                                    // matrice appartient à la page routing
                                    if (def.t === "in") {
                                        mixer.setInputGain(def.idx, db);
                                    } else {
                                        mixer.setOutputGain(def.idx, db);
                                    }
                                }
                                onMuteToggled: (m) => { if (def && def.t === "in") mixer.setMute(def.idx, m); }
                                onSendToggled: (bus, on) => {
                                    if (!def || def.t !== "in") return;
                                    mixer.setSend(def.idx, bus * 2, on ? 0 : -72);
                                    mixer.setSend(def.idx, bus * 2 + 1, on ? 0 : -72);
                                }
                            }
                        }
                    }

                    // -- master --
                    Rectangle {
                        width: 320
                        height: parent.height
                        color: "#171c21"
                        border.color: "#060809"

                        Column {
                            anchors.fill: parent
                            anchors.margins: 12
                            spacing: 10

                            Text { text: "MASTER"; color: "#e5a13c"; font.pixelSize: 12; font.bold: true; font.letterSpacing: 4 }

                            Row {
                                width: parent.width
                                height: 91      // retour utilisateur : x0.7
                                spacing: 6
                                VUNeedle {
                                    id: vuL
                                    width: (parent.width - 6) / 2; height: parent.height
                                    channel: "LEFT"
                                }
                                VUNeedle {
                                    id: vuR
                                    width: (parent.width - 6) / 2; height: parent.height
                                    channel: "RIGHT"
                                }
                            }

                            SpectrumView {
                                width: parent.width
                                height: 170
                            }

                            Row {
                                width: parent.width
                                height: parent.height - 91 - 170 - 70
                                spacing: 12
                                Fader {
                                    id: masterFader
                                    height: parent.height
                                    value: 0.857
                                    onMoved: (v) => {
                                        const db = v * 84 - 72;
                                        mixer.setOutputGain(0, db);
                                        mixer.setOutputGain(1, db);
                                    }
                                }
                                MeterBar {
                                    id: masterL
                                    width: 15; height: parent.height
                                }
                                MeterBar {
                                    id: masterR
                                    width: 15; height: parent.height
                                }
                                Column {
                                    spacing: 8
                                    anchors.verticalCenter: parent.verticalCenter
                                    Text { id: peakLTxt; text: "PEAK L  -∞"; color: "#8b959d"; font.pixelSize: 11; font.family: "monospace" }
                                    Text { id: peakRTxt; text: "PEAK R  -∞"; color: "#8b959d"; font.pixelSize: 11; font.family: "monospace" }
                                    Text { text: "FW " + mixer.version; color: "#5c666e"; font.pixelSize: 9; font.family: "monospace" }
                                }
                            }
                        }
                    }
                }

                // ===== navbar =====
                Rectangle {
                    width: parent.width; height: 46
                    color: "#1a2026"
                    Row {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.leftMargin: 14
                        spacing: 6
                        Repeater {
                            model: ["MIXER", "EFFETS", "MASTERING", "ROUTING", "SYSTÈME"]
                            Rectangle {
                                width: nvTxt.width + 34; height: 32; radius: 5
                                color: scene.currentPage === index ? "#2a2214" : "transparent"
                                border.color: scene.currentPage === index ? "#e5a13c" : "transparent"
                                Text {
                                    id: nvTxt
                                    anchors.centerIn: parent
                                    text: modelData
                                    color: scene.currentPage === index ? "#e5a13c" : "#5c666e"
                                    font.pixelSize: 12; font.letterSpacing: 2
                                }
                                TapHandler { onTapped: scene.currentPage = index }
                            }
                        }
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.right: parent.right
                        anchors.rightMargin: 16
                        text: "CONSOLE NATIVE N1 · " + scene.width + "×" + scene.height
                        color: "#5c666e"; font.pixelSize: 10; font.family: "monospace"
                    }
                }
            }
        }
    }
}
