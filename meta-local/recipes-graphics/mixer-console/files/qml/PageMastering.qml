// Page MASTERING — mode/source, grande enveloppe ML L/R, readouts
// exciter/limiteur, bypass par étage.
import QtQuick

Item {
    id: page
    property string mode: "?"
    property string source: "?"
    property var exc: ({})
    property var lim: ({})
    property bool excByp: false
    property bool limByp: false

    onVisibleChanged: if (visible) refresh()
    // V10-N8 : 500 ms → 1 s (2 ops socket par tick, réveils mixer-pro)
    Timer { interval: 1000; running: page.visible; repeat: true; onTriggered: page.refresh() }

    function refresh() {
        mixer.call({ op: "get_assistant" }, function(r) {
            if (r.ok) { page.mode = r.mode; page.source = r.source; }
        });
        mixer.call({ op: "get_insert" }, function(r) {
            if (!r.chain) return;
            for (const c of r.chain) {
                if (c.type === "exciter_native") { page.exc = c; page.excByp = c.bypass === 1; }
                if (c.type === "limiter_native") { page.lim = c; page.limByp = c.bypass === 1; }
            }
        });
    }

    function setMode(m, s) {
        mixer.call({ op: "set_assistant_mode", mode: m, source: s }, function(){ page.refresh(); });
    }
    function setBypass(slot, byp) {
        mixer.call({ op: "set_insert_param", slot: slot, param: "bypass",
                     value: byp ? 1 : 0 }, function(){ page.refresh(); });
    }

    Row {
        anchors.fill: parent
        anchors.margins: 14
        spacing: 12

        // ---- colonne gauche : FFT live (haut) + enveloppe ML (bas) ----
        Rectangle {
            width: parent.width * 0.62; height: parent.height
            color: "#171c21"; radius: 8; border.color: "#060809"
            Column {
                anchors.fill: parent; anchors.margins: 12; spacing: 6
                // V10-N8b : spectre temps réel du master (demande utilisateur
                // — la page mastering n'avait que l'enveloppe)
                SpectrumView {
                    width: parent.width
                    height: parent.height * 0.46
                }
                Text { text: "ENVELOPPE SPECTRALE ML · 64 PT · ±12 dB"; color: "#e5a13c"; font.pixelSize: 11; font.bold: true; font.letterSpacing: 3 }
                Rectangle {
                    width: parent.width; height: parent.height - parent.height * 0.46 - 42
                    color: "#0b0e11"; radius: 6; border.color: "#05070a"
                    Canvas {
                        id: envCv
                        anchors.fill: parent; anchors.margins: 4
                        onPaint: {
                            const ctx = getContext("2d");
                            ctx.reset();
                            // ligne 0 dB
                            ctx.strokeStyle = "rgba(255,255,255,.08)";
                            ctx.beginPath(); ctx.moveTo(0, height/2); ctx.lineTo(width, height/2); ctx.stroke();
                            const draw = (pts, col) => {
                                if (!pts || pts.length < 2) return;
                                ctx.beginPath();
                                for (let i = 0; i < pts.length; i++) {
                                    const x = i / (pts.length - 1) * width;
                                    const y = height/2 - (pts[i] / 12.0) * (height/2 - 6);
                                    if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
                                }
                                ctx.lineWidth = 2; ctx.strokeStyle = col; ctx.stroke();
                            };
                            draw(mixer.mlEnvL, "#4cc470");
                            draw(mixer.mlEnvR, "#e5a13c");
                        }
                        Connections {
                            target: mixer
                            function onInsertChanged() { if (page.visible) envCv.requestPaint(); }
                        }
                    }
                    Text {
                        anchors.top: parent.top; anchors.right: parent.right; anchors.margins: 8
                        text: mixer.mlActive ? "● L vert · R ambre" : "ML INACTIF"
                        color: mixer.mlActive ? "#8b959d" : "#e05545"
                        font.pixelSize: 10
                    }
                }
            }
        }

        // ---- colonne droite : mode + étages ----
        Column {
            width: parent.width * 0.38 - 12; height: parent.height
            spacing: 12

            Rectangle {
                width: parent.width; height: 120
                color: "#171c21"; radius: 8; border.color: "#060809"
                Column {
                    anchors.fill: parent; anchors.margins: 12; spacing: 8
                    Text { text: "MODE · " + page.mode.toUpperCase() + " · " + page.source.toUpperCase(); color: "#e5a13c"; font.pixelSize: 11; font.bold: true; font.letterSpacing: 2 }
                    Row {
                        spacing: 8
                        Repeater {
                            model: [["MASTERING", "mastering"], ["PASSTHROUGH", "passthrough"]]
                            Rectangle {
                                width: 130; height: 34; radius: 5
                                color: page.mode === modelData[1] ? "#2a2214" : "#1b2126"
                                border.color: page.mode === modelData[1] ? "#e5a13c" : "#39434b"
                                Text { anchors.centerIn: parent; text: modelData[0]; color: page.mode === modelData[1] ? "#e5a13c" : "#8b959d"; font.pixelSize: 11; font.bold: true }
                                MouseArea { anchors.fill: parent; onClicked: page.setMode(modelData[1], page.source) }
                            }
                        }
                    }
                    Row {
                        spacing: 8
                        Repeater {
                            model: [["SOURCE USB", "usb"], ["SOURCE HW", "hw"]]
                            Rectangle {
                                width: 130; height: 30; radius: 5
                                color: page.source === modelData[1] ? "#2a2214" : "#1b2126"
                                border.color: page.source === modelData[1] ? "#e5a13c" : "#39434b"
                                Text { anchors.centerIn: parent; text: modelData[0]; color: page.source === modelData[1] ? "#e5a13c" : "#8b959d"; font.pixelSize: 10 }
                                MouseArea { anchors.fill: parent; onClicked: page.setMode(page.mode, modelData[1]) }
                            }
                        }
                    }
                }
            }

            Rectangle {
                width: parent.width; height: 150
                color: "#171c21"; radius: 8; border.color: "#060809"
                Column {
                    anchors.fill: parent; anchors.margins: 12; spacing: 6
                    Row {
                        spacing: 10
                        Text { text: "EXCITER"; color: "#e5a13c"; font.pixelSize: 11; font.bold: true; font.letterSpacing: 3; anchors.verticalCenter: parent.verticalCenter }
                        Rectangle {
                            width: 56; height: 24; radius: 12
                            color: !page.excByp ? "#2a2214" : "#1b2126"
                            border.color: !page.excByp ? "#e5a13c" : "#39434b"
                            Text { anchors.centerIn: parent; text: !page.excByp ? "ACTIF" : "OFF"; color: !page.excByp ? "#e5a13c" : "#5c666e"; font.pixelSize: 9; font.bold: true }
                            MouseArea { anchors.fill: parent; onClicked: page.setBypass(1, !page.excByp) }
                        }
                    }
                    Text { text: "AMOUNT " + (page.exc.amount !== undefined ? page.exc.amount.toFixed(3) : "—")
                                 + "   ·   DRIVE " + (page.exc.drive !== undefined ? page.exc.drive.toFixed(2) : "—")
                                 + "   ·   FREQ " + (page.exc.freq !== undefined ? (page.exc.freq / 1000).toFixed(1) + " kHz" : "—")
                           color: "#8b959d"; font.pixelSize: 11; font.family: "monospace" }
                }
            }

            Rectangle {
                width: parent.width; height: 170
                color: "#171c21"; radius: 8; border.color: "#060809"
                Column {
                    anchors.fill: parent; anchors.margins: 12; spacing: 6
                    Row {
                        spacing: 10
                        Text { text: "LIMITEUR"; color: "#e5a13c"; font.pixelSize: 11; font.bold: true; font.letterSpacing: 3; anchors.verticalCenter: parent.verticalCenter }
                        Rectangle {
                            width: 56; height: 24; radius: 12
                            color: !page.limByp ? "#2a2214" : "#1b2126"
                            border.color: !page.limByp ? "#e5a13c" : "#39434b"
                            Text { anchors.centerIn: parent; text: !page.limByp ? "ACTIF" : "OFF"; color: !page.limByp ? "#e5a13c" : "#5c666e"; font.pixelSize: 9; font.bold: true }
                            MouseArea { anchors.fill: parent; onClicked: page.setBypass(2, !page.limByp) }
                        }
                    }
                    Text { text: "SEUIL " + (page.lim.th !== undefined ? page.lim.th.toFixed(3) : "—")
                                 + "   ·   ATT " + (page.lim.at !== undefined ? page.lim.at.toFixed(1) + " ms" : "—")
                                 + "   ·   REL " + (page.lim.rt !== undefined ? page.lim.rt.toFixed(0) + " ms" : "—")
                           color: "#8b959d"; font.pixelSize: 11; font.family: "monospace" }
                    // réduction de gain FLUIDE (g_in via poll insert 5 Hz,
                    // ballistique à la frame — gabarit standard)
                    Column {
                        id: grCol
                        width: parent.width
                        spacing: 6
                        property real gr: 0
                        FrameAnimation {
                            running: page.visible
                            onTriggered: {
                                const dt = Math.min(frameTime, 0.1);
                                const kA = 1 - Math.exp(-dt / 0.030);
                                const kR = 1 - Math.exp(-dt / 0.200);
                                var tgt = 0;
                                if (page.lim.g_in !== undefined && page.lim.g_in > 0)
                                    tgt = Math.max(0, Math.min(1,
                                        (-20 * Math.log(page.lim.g_in) / Math.LN10) / 12.0));
                                grCol.gr += (tgt - grCol.gr)
                                            * (tgt > grCol.gr ? kA : kR);
                            }
                        }
                        Text { text: "RÉDUCTION  " + (grCol.gr * 12).toFixed(1) + " dB"
                               color: "#5c666e"; font.pixelSize: 9; font.letterSpacing: 2 }
                        Rectangle {
                            width: parent.width; height: 26; radius: 13; color: "#0b0e11"
                            Rectangle {
                                height: parent.height; radius: 13
                                anchors.right: parent.right
                                width: parent.width * grCol.gr
                                color: "#e05545"
                            }
                        }
                    }
                }
            }
        }
    }
}
