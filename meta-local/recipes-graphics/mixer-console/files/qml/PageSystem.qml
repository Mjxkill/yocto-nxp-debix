// Page SYSTÈME — charges (via /api/sysload de mixer-gui-http), stats audio
// (socket direct), drift USB↔DSP, RESET TAC.
import QtQuick

Item {
    id: page
    property var loads: ({ cpu: [0, 0, 0, 0], dsp: -1, npu: -1, gpu: -1 })
    property var drift: ({})
    property var stat: ({})

    onVisibleChanged: if (visible) refresh()
    Timer { interval: 1000; running: page.visible; repeat: true; onTriggered: page.refresh() }

    function xhr(method, url, cb) {
        const q = new XMLHttpRequest();
        q.onreadystatechange = function() {
            if (q.readyState === XMLHttpRequest.DONE && q.status === 200) {
                try { cb(JSON.parse(q.responseText)); } catch (e) {}
            }
        };
        q.open(method, "http://127.0.0.1:8080" + url);
        q.send();
    }

    function refresh() {
        xhr("GET", "/api/sysload", function(r) { if (r.ok) page.loads = r; });
        xhr("GET", "/api/drift", function(r) { page.drift = r; });
        mixer.call({ op: "get_state" }, function(r) { if (r.ok) page.stat = r; });
    }

    Row {
        anchors.fill: parent
        anchors.margins: 14
        spacing: 12

        Rectangle {   // charges
            width: (parent.width - 24) / 3; height: parent.height
            color: "#171c21"; radius: 8; border.color: "#060809"
            Column {
                anchors.fill: parent; anchors.margins: 14; spacing: 10
                Text { text: "CHARGES"; color: "#e5a13c"; font.pixelSize: 11; font.bold: true; font.letterSpacing: 3 }
                Repeater {
                    model: [["CPU 0", 0], ["CPU 1", 1], ["CPU 2 (audio RT)", 2], ["CPU 3", 3]]
                    Column {
                        width: parent.width; spacing: 3
                        Row {
                            width: parent.width
                            Text { text: modelData[0]; color: "#8b959d"; font.pixelSize: 11; width: parent.width - 50 }
                            Text {
                                text: (page.loads.cpu && page.loads.cpu[modelData[1]] >= 0 ? page.loads.cpu[modelData[1]] + "%" : "—")
                                color: "#e9e5da"; font.pixelSize: 11; font.family: "monospace"
                            }
                        }
                        Rectangle {
                            width: parent.width; height: 7; radius: 3; color: "#0b0e11"
                            Rectangle {
                                height: parent.height; radius: 3
                                width: parent.width * Math.min(1, (page.loads.cpu ? page.loads.cpu[modelData[1]] : 0) / 100)
                                color: "#e5a13c"
                            }
                        }
                    }
                }
                Repeater {
                    model: [["DSP HiFi4", "dsp"], ["NPU", "npu"], ["GPU", "gpu"]]
                    Column {
                        width: parent.width; spacing: 3
                        Row {
                            width: parent.width
                            Text { text: modelData[0]; color: "#8b959d"; font.pixelSize: 11; width: parent.width - 50 }
                            Text {
                                text: page.loads[modelData[1]] >= 0 ? page.loads[modelData[1]] + "%" : "—"
                                color: "#e9e5da"; font.pixelSize: 11; font.family: "monospace"
                            }
                        }
                        Rectangle {
                            width: parent.width; height: 7; radius: 3; color: "#0b0e11"
                            Rectangle {
                                height: parent.height; radius: 3
                                width: parent.width * Math.min(1, Math.max(0, page.loads[modelData[1]]) / 100)
                                color: modelData[1] === "dsp" ? "#e05545" : "#4cc470"
                            }
                        }
                    }
                }
            }
        }

        Rectangle {   // audio
            width: (parent.width - 24) / 3; height: parent.height
            color: "#171c21"; radius: 8; border.color: "#060809"
            Column {
                anchors.fill: parent; anchors.margins: 14; spacing: 9
                Text { text: "AUDIO"; color: "#e5a13c"; font.pixelSize: 11; font.bold: true; font.letterSpacing: 3 }
                Repeater {
                    model: [
                        ["VERSION", function(s) { return s.version || "—"; }],
                        ["XRUN", function(s) { return s.xrun !== undefined ? s.xrun : "—"; }],
                        ["LATENCE", function(s) { return s.latency_us_one_way !== undefined ? (s.latency_us_one_way / 1000).toFixed(1) + " ms" : "—"; }],
                        ["PROF CAP/MIX/ITER", function(s) { return s.prof_cap_us !== undefined ? s.prof_cap_us + "/" + s.prof_mix_us + "/" + s.prof_iter_us + " µs" : "—"; }],
                        ["RING DROPS", function(s) { return s.ring_drops !== undefined ? s.ring_drops : "—"; }],
                        ["RING FILL", function(s) { return s.ring_fill_frames !== undefined ? s.ring_fill_frames + " frames" : "—"; }]
                    ]
                    Row {
                        width: parent.width
                        Text { text: modelData[0]; color: "#8b959d"; font.pixelSize: 11; width: parent.width * 0.55 }
                        Text { text: modelData[1](page.stat); color: "#e9e5da"; font.pixelSize: 11; font.family: "monospace" }
                    }
                }
                Text { text: "GUI " + fpsMeter.fps + " FPS"; color: "#5c666e"; font.pixelSize: 10; font.family: "monospace" }
            }
        }

        Rectangle {   // drift + maintenance
            width: (parent.width - 24) / 3; height: parent.height
            color: "#171c21"; radius: 8; border.color: "#060809"
            Column {
                anchors.fill: parent; anchors.margins: 14; spacing: 9
                Text { text: "DRIFT USB↔DSP"; color: "#e5a13c"; font.pixelSize: 11; font.bold: true; font.letterSpacing: 3 }
                Repeater {
                    model: [
                        ["DRIFT", function(d) { return d.drift_ppm !== undefined ? d.drift_ppm.toFixed(2) + " ppm" : "—"; }],
                        ["ERR CAP", function(d) { return d.err_cap !== undefined ? d.err_cap : "—"; }],
                        ["ERR PLAY", function(d) { return d.err_play !== undefined ? d.err_play : "—"; }]
                    ]
                    Row {
                        width: parent.width
                        Text { text: modelData[0]; color: "#8b959d"; font.pixelSize: 11; width: parent.width * 0.45 }
                        Text { text: modelData[1](page.drift); color: "#e9e5da"; font.pixelSize: 11; font.family: "monospace" }
                    }
                }
                Item { width: 1; height: 12 }
                Text { text: "MAINTENANCE"; color: "#e5a13c"; font.pixelSize: 11; font.bold: true; font.letterSpacing: 3 }
                Rectangle {
                    id: tacBtn
                    width: parent.width; height: 44; radius: 5
                    property bool busy: false
                    color: busy ? "#2a2214" : "#1b2126"; border.color: "#39434b"
                    Text { anchors.centerIn: parent; text: tacBtn.busy ? "RESET EN COURS…" : "RESET TACS (i2c)"; color: tacBtn.busy ? "#e5a13c" : "#e9e5da"; font.pixelSize: 12; font.bold: true }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: {
                            if (tacBtn.busy) return;
                            tacBtn.busy = true;
                            page.xhr("POST", "/api/tac/reset", function() {});
                            resetTimer.start();
                        }
                    }
                    Timer { id: resetTimer; interval: 3000; onTriggered: tacBtn.busy = false }
                }
                Rectangle {
                    width: parent.width; height: 44; radius: 5
                    color: "#1b2126"; border.color: "#39434b"
                    Text { anchors.centerIn: parent; text: "CALIBRER LE TACTILE"; color: "#e9e5da"; font.pixelSize: 12; font.bold: true }
                    TapHandler { onTapped: calib.start() }
                }
                /* V10-N7 — reset usine : purge tous les états persistés
                 * (matrice, gains, effets, mode assistant) + reboot →
                 * défauts usine partout, y compris blobs DSP (reload
                 * firmware). Double tap de confirmation, fenêtre 5 s. */
                Rectangle {
                    id: factoryBtn
                    width: parent.width; height: 44; radius: 5
                    property bool arm: false
                    property bool busy: false
                    color: busy ? "#2a2214" : (arm ? "#3a1512" : "#1b2126")
                    border.color: arm ? "#e05545" : "#39434b"
                    Text {
                        anchors.centerIn: parent
                        text: factoryBtn.busy ? "RESET… REDÉMARRAGE"
                              : (factoryBtn.arm ? "CONFIRMER LE RESET USINE ?"
                                                : "RESET USINE (tout effacer)")
                        color: factoryBtn.busy ? "#e5a13c"
                               : (factoryBtn.arm ? "#f2796a" : "#e9e5da")
                        font.pixelSize: 12; font.bold: true
                    }
                    TapHandler {
                        onTapped: {
                            if (factoryBtn.busy) return;
                            if (!factoryBtn.arm) {
                                factoryBtn.arm = true;
                                armTimer.restart();
                                return;
                            }
                            factoryBtn.arm = false;
                            factoryBtn.busy = true;
                            const q = new XMLHttpRequest();
                            q.open("POST", "http://127.0.0.1:8080/api/factory/reset");
                            q.send(JSON.stringify({ confirm: "usine" }));
                        }
                    }
                    Timer { id: armTimer; interval: 5000; onTriggered: factoryBtn.arm = false }
                }
            }
        }
    }
}
