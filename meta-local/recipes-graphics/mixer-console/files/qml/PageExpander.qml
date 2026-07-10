// V12-SYNTH — Page EXPANDEUR : module de sons multi-timbral à 2 moteurs.
// Par canal MIDI 1-16 : GM (fluidsynth, 128 programmes) OU M1 (moteur
// « AI Synthesis » maison : 2 OSC PCM + VDF sans résonance + VDA, EG
// ADBSSR, LFO — patches 0-99 éditables live dans le panneau ÉDIT).
// Dialogue via l'op proxy midix_ctl de mixer-pro (cmd historiques +
// passthrough "line" pour engine/inst_list/patch_*).
import QtQuick

Item {
    id: page

    // Noms de programmes General MIDI (standard GM level 1, 0-127)
    readonly property var gmNames: [
        "Grand Piano", "Bright Piano", "Electric Grand", "Honky-tonk",
        "E.Piano 1", "E.Piano 2", "Harpsichord", "Clavinet",
        "Celesta", "Glockenspiel", "Music Box", "Vibraphone",
        "Marimba", "Xylophone", "Tubular Bells", "Dulcimer",
        "Drawbar Organ", "Percussive Organ", "Rock Organ", "Church Organ",
        "Reed Organ", "Accordion", "Harmonica", "Tango Accordion",
        "Nylon Guitar", "Steel Guitar", "Jazz Guitar", "Clean Guitar",
        "Muted Guitar", "Overdrive Guitar", "Distortion Guitar", "Harmonics",
        "Acoustic Bass", "Finger Bass", "Pick Bass", "Fretless Bass",
        "Slap Bass 1", "Slap Bass 2", "Synth Bass 1", "Synth Bass 2",
        "Violin", "Viola", "Cello", "Contrabass",
        "Tremolo Strings", "Pizzicato", "Harp", "Timpani",
        "String Ens. 1", "String Ens. 2", "Synth Strings 1", "Synth Strings 2",
        "Choir Aahs", "Voice Oohs", "Synth Voice", "Orchestra Hit",
        "Trumpet", "Trombone", "Tuba", "Muted Trumpet",
        "French Horn", "Brass Section", "Synth Brass 1", "Synth Brass 2",
        "Soprano Sax", "Alto Sax", "Tenor Sax", "Baritone Sax",
        "Oboe", "English Horn", "Bassoon", "Clarinet",
        "Piccolo", "Flute", "Recorder", "Pan Flute",
        "Blown Bottle", "Shakuhachi", "Whistle", "Ocarina",
        "Square Lead", "Saw Lead", "Calliope", "Chiff Lead",
        "Charang", "Voice Lead", "Fifths Lead", "Bass+Lead",
        "New Age Pad", "Warm Pad", "Polysynth Pad", "Choir Pad",
        "Bowed Pad", "Metallic Pad", "Halo Pad", "Sweep Pad",
        "Rain FX", "Soundtrack", "Crystal", "Atmosphere",
        "Brightness", "Goblins", "Echoes", "Sci-Fi",
        "Sitar", "Banjo", "Shamisen", "Koto",
        "Kalimba", "Bagpipe", "Fiddle", "Shanai",
        "Tinkle Bell", "Agogo", "Steel Drums", "Woodblock",
        "Taiko Drum", "Melodic Tom", "Synth Drum", "Reverse Cymbal",
        "Fret Noise", "Breath Noise", "Seashore", "Bird Tweet",
        "Telephone", "Helicopter", "Applause", "Gunshot"
    ]

    property bool present: false
    property int peak: 0
    property real synthGain: 0.5
    property string sf2: ""
    property var chans:   [0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]
    property var acts:    [0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]
    // V13.2-FLUID : activité lissée à la frame (1 seule FrameAnimation,
    // décimée ×2 — recette SpectrumView), les barres lisent actsDisp
    property var actsDisp: [0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]
    property int _atick: 0
    FrameAnimation {
        running: page.visible
        onTriggered: {
            if ((page._atick++ & 1) === 1) return;
            const dt = Math.min(frameTime * 2, 0.1);
            const kA = 1 - Math.exp(-dt / 0.030);
            const kR = 1 - Math.exp(-dt / 0.150);
            const out = page.actsDisp.slice();
            for (let i = 0; i < 16; i++) {
                const t = Math.min(1, (page.acts[i] || 0) / 1000.0);
                out[i] += (t - out[i]) * (t > out[i] ? kA : kR);
            }
            page.actsDisp = out;
        }
    }
    property var engines: [0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]
    property var cpatch:  [0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]
    property var patchNames: []
    property var instNames: []
    property bool draggingGain: false

    // ---- éditeur de patch M1 ----
    property int  editPatch: -1        // -1 = fermé
    property var  pd: null             // données du patch édité

    onVisibleChanged: if (visible) { pollStatus(); loadPatchNames(); }
    Timer { interval: 250; running: page.visible; repeat: true; onTriggered: page.pollStatus() }
    Timer {
        interval: 250; running: page.visible; repeat: true
        onTriggered: mixer.call({ op: "get_midix" }, function(r) {
            if (r.ok) { page.present = r.present === 1; page.peak = r.peak; }
        })
    }

    function pollStatus() {
        mixer.call({ op: "midix_ctl", cmd: "status" }, function(r) {
            if (!r.ok) { page.present = false; return; }
            page.sf2 = r.sf2 || "";
            page.chans = r.chans;
            if (r.act) page.acts = r.act;
            if (r.engines) page.engines = r.engines;
            if (r.patch) page.cpatch = r.patch;
            if (!page.draggingGain) page.synthGain = r.gain;
        });
    }
    function loadPatchNames() {
        mixer.call({ op: "midix_ctl", line: "patch_list" }, function(r) {
            if (r.ok) page.patchNames = r.patches;
        });
    }
    function loadInstNames(cb) {
        if (instNames.length > 0) { cb(); return; }
        mixer.call({ op: "midix_ctl", line: "inst_list" }, function(r) {
            if (r.ok) { page.instNames = r.inst; cb(); }
        });
    }
    function setProg(chan, num) {
        if (num < 0) num = 127;
        if (num > 127) num = 0;
        mixer.call({ op: "midix_ctl", cmd: "prog", chan: chan, num: num },
                   function() { page.pollStatus(); });
    }
    function setEngine(chan, eng, patch) {
        mixer.call({ op: "midix_ctl",
                     line: "engine " + chan + " " + eng + " " + patch },
                   function() { page.pollStatus(); });
    }
    function openEditor(pidx) {
        loadInstNames(function() {
            mixer.call({ op: "midix_ctl", line: "patch_get " + pidx },
                       function(r) {
                if (r.ok) { page.pd = r; page.editPatch = pidx; }
            });
        });
    }
    function pset(key, val) {
        mixer.call({ op: "midix_ctl",
                     line: "patch_set " + editPatch + " " + key + " " + val },
                   function() {});
        var d = page.pd; d[key] = val; page.pd = d;
    }
    function psetArr(arr, i, val) {   // vdf0..5 / vda0..5
        mixer.call({ op: "midix_ctl",
                     line: "patch_set " + editPatch + " " + arr + i + " " + val },
                   function() {});
        var d = page.pd; d[arr][i] = val; page.pd = d; page.pdChanged();
    }

    Column {
        anchors.fill: parent
        anchors.margins: 14
        spacing: 10

        // ===================== EN-TÊTE =====================
        Rectangle {
            width: parent.width; height: 86
            color: "#171c21"; radius: 8; border.color: "#060809"
            Item {
                anchors.fill: parent; anchors.margins: 12

                Row {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                spacing: 16

                Column {
                    spacing: 4
                    anchors.verticalCenter: parent.verticalCenter
                    Text { text: "EXPANDEUR"; color: "#e5a13c"; font.pixelSize: 13
                           font.bold: true; font.letterSpacing: 3 }
                    Row {
                        spacing: 8
                        Rectangle {
                            width: 10; height: 10; radius: 5
                            anchors.verticalCenter: parent.verticalCenter
                            color: page.present ? "#4cc470" : "#e05545"
                        }
                        Text {
                            text: page.present
                                  ? (page.sf2 !== "" ? page.sf2 + " + M1" : "en ligne")
                                  : "module absent"
                            color: page.present ? "#8b959d" : "#e05545"
                            font.pixelSize: 11
                        }
                    }
                }

                // VU synthé : gabarit standard (étiquette dessus, 26 px,
                // ballistique à la frame — recette SpectrumView)
                Column {
                    id: xvuCol
                    spacing: 6
                    anchors.verticalCenter: parent.verticalCenter
                    property real vu: 0
                    FrameAnimation {
                        running: page.visible
                        onTriggered: {
                            const dt = Math.min(frameTime, 0.1);
                            const kA = 1 - Math.exp(-dt / 0.030);
                            const kR = 1 - Math.exp(-dt / 0.120);
                            var tgt = 0;
                            var f = page.peak / 2147483647.0;
                            if (f > 0) {
                                var db = 20 * Math.log(f) / Math.LN10;
                                tgt = Math.max(0, Math.min(1, (db + 48) / 48));
                            }
                            xvuCol.vu += (tgt - xvuCol.vu)
                                         * (tgt > xvuCol.vu ? kA : kR);
                        }
                    }
                    Text { text: "NIVEAU"; color: "#5c666e"; font.pixelSize: 9
                           font.letterSpacing: 2 }
                    Rectangle {
                        width: 200; height: 26; radius: 13; color: "#0b0e11"
                        Rectangle {
                            height: parent.height; radius: 13
                            width: parent.width * xvuCol.vu
                            color: "#4cc470"
                        }
                    }
                }

                Column {
                    spacing: 6
                    anchors.verticalCenter: parent.verticalCenter
                    Text { text: "VOLUME  " + page.synthGain.toFixed(2)
                           color: "#5c666e"; font.pixelSize: 9; font.letterSpacing: 2 }
                    Rectangle {
                        width: 260; height: 26; radius: 13; color: "#0b0e11"
                        Rectangle {
                            height: parent.height; radius: 13
                            width: parent.width * Math.min(1, page.synthGain / 2.0)
                            color: "#39434b"
                            Rectangle { width: 4; height: parent.height
                                        anchors.right: parent.right; color: "#e5a13c" }
                        }
                        MouseArea {
                            anchors.fill: parent
                            preventStealing: true
                            onPressed: page.draggingGain = true
                            onReleased: {
                                page.draggingGain = false;
                                mixer.call({ op: "midix_ctl", cmd: "gain",
                                             value: page.synthGain }, function() {});
                            }
                            onCanceled: page.draggingGain = false
                            onPositionChanged: (m) => {
                                if (!pressed) return;
                                var f = Math.max(0, Math.min(1, m.x / width));
                                page.synthGain = Number((f * 2.0).toFixed(2));
                            }
                        }
                    }
                }

                }

                Rectangle {
                    width: 120; height: 44; radius: 6
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    color: "#2a1512"; border.color: "#7a3b32"
                    Text { anchors.centerIn: parent; text: "PANIC"
                           color: "#f2796a"; font.pixelSize: 12; font.bold: true }
                    TapHandler {
                        gesturePolicy: TapHandler.ReleaseWithinBounds
                        onTapped: mixer.call({ op: "midix_ctl", cmd: "panic" },
                                             function() {})
                    }
                }
            }
        }

        // ============ 16 CANAUX MIDI (scroll) ============
        Flickable {
            width: parent.width
            height: parent.height - 86 - 24 - 2*10
            contentHeight: chanCol.height
            clip: true

            Column {
                id: chanCol
                width: parent.width
                spacing: 6

                Repeater {
                    model: 16
                    Rectangle {
                        id: chRow
                        property bool isDrums: index === 9
                        property bool isM1: index < page.engines.length
                                            && page.engines[index] === 1
                        property int prog: index < page.chans.length ? page.chans[index] : 0
                        property int pidx: index < page.cpatch.length ? page.cpatch[index] : 0
                        width: parent.width
                        height: 58
                        radius: 6
                        color: isM1 ? "#181420" : "#14181c"
                        border.color: isM1 ? "#5a4a7a" : (isDrums ? "#5a4a2a" : "#22282e")

                        Row {
                            anchors.fill: parent
                            anchors.margins: 8
                            spacing: 10

                            Rectangle {
                                width: 60; height: 42; radius: 5
                                anchors.verticalCenter: parent.verticalCenter
                                color: "#1b2126"; border.color: "#39434b"
                                Text { anchors.centerIn: parent
                                       text: "CH " + (index + 1)
                                       color: "#e5a13c"; font.pixelSize: 13; font.bold: true }
                            }

                            // bascule GM / M1
                            Rectangle {
                                width: 58; height: 42; radius: 5
                                anchors.verticalCenter: parent.verticalCenter
                                color: chRow.isM1 ? "#2a2440" : "#1b2126"
                                border.color: chRow.isM1 ? "#8a7ad0" : "#39434b"
                                border.width: chRow.isM1 ? 2 : 1
                                Text { anchors.centerIn: parent
                                       text: chRow.isM1 ? "M1" : "GM"
                                       color: chRow.isM1 ? "#b3a5f0" : "#8b959d"
                                       font.pixelSize: 13; font.bold: true }
                                TapHandler {
                                    gesturePolicy: TapHandler.ReleaseWithinBounds
                                    onTapped: page.setEngine(index,
                                                             chRow.isM1 ? 0 : 1,
                                                             chRow.pidx)
                                }
                            }

                            Rectangle {
                                width: 40; height: 42; radius: 5
                                anchors.verticalCenter: parent.verticalCenter
                                color: "#1b2126"; border.color: "#39434b"
                                Text { anchors.centerIn: parent; text: "‹"
                                       color: "#c9c4b8"; font.pixelSize: 20 }
                                TapHandler {
                                    gesturePolicy: TapHandler.ReleaseWithinBounds
                                    onTapped: {
                                        if (chRow.isM1) {
                                            var p = (chRow.pidx + 15) % 16;
                                            page.setEngine(index, 1, p);
                                        } else
                                            page.setProg(index, chRow.prog - 1);
                                    }
                                }
                            }
                            Column {
                                width: 218
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 2
                                Text {
                                    text: {
                                        if (chRow.isM1)
                                            return (chRow.pidx < page.patchNames.length
                                                    ? page.patchNames[chRow.pidx]
                                                    : "Patch " + chRow.pidx);
                                        return chRow.isDrums
                                               ? "Drum Kit " + chRow.prog
                                               : page.gmNames[chRow.prog];
                                    }
                                    color: chRow.isM1 ? "#b3a5f0" : "#e9e5da"
                                    font.pixelSize: 14; font.bold: true
                                    elide: Text.ElideRight; width: parent.width
                                }
                                Text {
                                    text: chRow.isM1 ? "patch M1 " + chRow.pidx
                                                     : "programme GM " + chRow.prog
                                    color: "#5c666e"; font.pixelSize: 10
                                    font.family: "monospace"
                                }
                            }
                            Rectangle {
                                width: 40; height: 42; radius: 5
                                anchors.verticalCenter: parent.verticalCenter
                                color: "#1b2126"; border.color: "#39434b"
                                Text { anchors.centerIn: parent; text: "›"
                                       color: "#c9c4b8"; font.pixelSize: 20 }
                                TapHandler {
                                    gesturePolicy: TapHandler.ReleaseWithinBounds
                                    onTapped: {
                                        if (chRow.isM1) {
                                            var p = (chRow.pidx + 1) % 16;
                                            page.setEngine(index, 1, p);
                                        } else
                                            page.setProg(index, chRow.prog + 1);
                                    }
                                }
                            }

                            // ÉDIT (patch M1)
                            Rectangle {
                                width: 62; height: 42; radius: 5
                                anchors.verticalCenter: parent.verticalCenter
                                visible: chRow.isM1
                                color: "#2a2214"; border.color: "#e5a13c"
                                Text { anchors.centerIn: parent; text: "ÉDIT"
                                       color: "#e5a13c"; font.pixelSize: 11; font.bold: true }
                                TapHandler {
                                    gesturePolicy: TapHandler.ReleaseWithinBounds
                                    onTapped: page.openEditor(chRow.pidx)
                                }
                            }

                            // activité MIDI du canal
                            Rectangle {
                                width: chRow.isM1 ? 108 : 170
                                height: 12; radius: 6
                                anchors.verticalCenter: parent.verticalCenter
                                color: "#0b0e11"
                                Rectangle {
                                    height: parent.height; radius: 6
                                    width: parent.width * (page.actsDisp[index] || 0)
                                    color: chRow.isM1 ? "#b3a5f0"
                                           : (chRow.isDrums ? "#e8b84b" : "#4cc470")
                                }
                            }
                        }
                    }
                }
            }
        }

        Text {
            text: "GM = banque GeneralUser · M1 = synthé A.L.A. (2 OSC PCM + VDF + VDA) · ÉDIT pour sculpter le patch · CH 10 = batterie GM"
            color: "#5c666e"; font.pixelSize: 10; font.letterSpacing: 1
            width: parent.width; wrapMode: Text.WordWrap
        }
    }

    // ================= ÉDITEUR DE PATCH M1 (overlay) =================
    Rectangle {
        anchors.fill: parent
        visible: page.editPatch >= 0 && page.pd !== null
        color: "#0e1114"
        z: 200

        // ligne de slider 0-99 réutilisable
        component ParamSlider: Row {
            id: ps
            property string label: ""
            property int value: 0
            property int minv: 0
            property int maxv: 99
            signal changed(int v)
            spacing: 10
            height: 40
            Text {
                width: 118
                anchors.verticalCenter: parent.verticalCenter
                text: ps.label
                color: "#8b959d"; font.pixelSize: 10; font.bold: true
                font.letterSpacing: 1
            }
            Rectangle {
                width: parent.width - 118 - 56 - 2*10
                height: 24; radius: 12
                anchors.verticalCenter: parent.verticalCenter
                color: "#0b0e11"
                Rectangle {
                    height: parent.height; radius: 12
                    width: parent.width * (ps.value - ps.minv) / (ps.maxv - ps.minv)
                    color: "#39434b"
                    Rectangle { width: 4; height: parent.height
                                anchors.right: parent.right; color: "#b3a5f0" }
                }
                MouseArea {
                    anchors.fill: parent
                    preventStealing: true
                    function apply(mx) {
                        var f = Math.max(0, Math.min(1, mx / width));
                        var v = Math.round(ps.minv + f * (ps.maxv - ps.minv));
                        if (v !== ps.value) ps.changed(v);
                    }
                    onPressed: (m) => apply(m.x)
                    onPositionChanged: (m) => { if (pressed) apply(m.x); }
                }
            }
            Text {
                width: 56
                anchors.verticalCenter: parent.verticalCenter
                text: ps.value
                color: "#e9e5da"; font.pixelSize: 13; font.bold: true
                font.family: "monospace"
            }
        }

        Column {
            anchors.fill: parent
            anchors.margins: 14
            spacing: 8

            // en-tête éditeur
            Item {
                width: parent.width; height: 40
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "PATCH M1 · " + (page.pd ? page.pd.name : "")
                          + "  (n° " + page.editPatch + ")"
                    color: "#b3a5f0"; font.pixelSize: 15; font.bold: true
                    font.letterSpacing: 2
                }
                Rectangle {
                    anchors.right: closeEd.left; anchors.rightMargin: 10
                    width: 100; height: 38; radius: 5
                    color: "#142a19"; border.color: "#4cc470"
                    Text { anchors.centerIn: parent; text: "SAUVER"
                           color: "#4cc470"; font.pixelSize: 12; font.bold: true }
                    TapHandler {
                        gesturePolicy: TapHandler.ReleaseWithinBounds
                        onTapped: {
                            mixer.call({ op: "midix_ctl",
                                         line: "patch_save " + page.editPatch },
                                       function() { page.loadPatchNames(); });
                        }
                    }
                }
                Rectangle {
                    id: closeEd
                    anchors.right: parent.right
                    width: 64; height: 38; radius: 5
                    color: "#2a2214"; border.color: "#e5a13c"
                    Text { anchors.centerIn: parent; text: "✕"
                           color: "#e5a13c"; font.pixelSize: 16; font.bold: true }
                    TapHandler {
                        margin: 10
                        gesturePolicy: TapHandler.ReleaseWithinBounds
                        onTapped: { page.editPatch = -1; page.pd = null; }
                    }
                }
            }

            Flickable {
                width: parent.width
                height: parent.height - 40 - 8
                contentHeight: edCol.height
                clip: true

                Column {
                    id: edCol
                    width: parent.width
                    spacing: 6

                    // ---- OSCILLATEURS ----
                    Text { text: "OSCILLATEURS  (multisamples)"; color: "#e5a13c"
                           font.pixelSize: 11; font.bold: true; font.letterSpacing: 2 }
                    Repeater {
                        model: [["osc1", "OSC 1"], ["osc2", "OSC 2 (−1 = off)"]]
                        Row {
                            spacing: 10; height: 46
                            Text { width: 118; anchors.verticalCenter: parent.verticalCenter
                                   text: modelData[1]; color: "#8b959d"
                                   font.pixelSize: 10; font.bold: true }
                            Rectangle {
                                width: 40; height: 40; radius: 5
                                color: "#1b2126"; border.color: "#39434b"
                                Text { anchors.centerIn: parent; text: "‹"
                                       color: "#c9c4b8"; font.pixelSize: 18 }
                                TapHandler {
                                    gesturePolicy: TapHandler.ReleaseWithinBounds
                                    onTapped: {
                                        var k = modelData[0];
                                        var v = page.pd[k] - 1;
                                        var lo = k === "osc2" ? -1 : 0;
                                        if (v < lo) v = page.instNames.length - 1;
                                        page.pset(k, v);
                                    }
                                }
                            }
                            Rectangle {
                                width: 330; height: 40; radius: 5
                                color: "#0f1216"; border.color: "#5a4a7a"
                                Text {
                                    anchors.centerIn: parent
                                    width: parent.width - 12; elide: Text.ElideRight
                                    horizontalAlignment: Text.AlignHCenter
                                    text: {
                                        var v = page.pd ? page.pd[modelData[0]] : 0;
                                        if (v < 0) return "OFF";
                                        return v < page.instNames.length
                                               ? page.instNames[v] : "#" + v;
                                    }
                                    color: "#e9e5da"; font.pixelSize: 13; font.bold: true
                                }
                            }
                            Rectangle {
                                width: 40; height: 40; radius: 5
                                color: "#1b2126"; border.color: "#39434b"
                                Text { anchors.centerIn: parent; text: "›"
                                       color: "#c9c4b8"; font.pixelSize: 18 }
                                TapHandler {
                                    gesturePolicy: TapHandler.ReleaseWithinBounds
                                    onTapped: {
                                        var k = modelData[0];
                                        var v = page.pd[k] + 1;
                                        if (v >= page.instNames.length)
                                            v = k === "osc2" ? -1 : 0;
                                        page.pset(k, v);
                                    }
                                }
                            }
                        }
                    }
                    ParamSlider { width: edCol.width; label: "DETUNE OSC2 (cts)"
                        minv: -50; maxv: 50
                        value: page.pd ? page.pd.detune : 0
                        onChanged: (v) => page.pset("detune", v) }
                    ParamSlider { width: edCol.width; label: "BALANCE 1↔2"
                        value: page.pd ? page.pd.balance : 50
                        onChanged: (v) => page.pset("balance", v) }

                    // ---- VDF ----
                    Text { text: "VDF  (filtre passe-bas, sans résonance — M1)"
                           color: "#e5a13c"; font.pixelSize: 11; font.bold: true
                           font.letterSpacing: 2 }
                    ParamSlider { width: edCol.width; label: "CUTOFF"
                        value: page.pd ? page.pd.cutoff : 0
                        onChanged: (v) => page.pset("cutoff", v) }
                    ParamSlider { width: edCol.width; label: "EG INT"
                        value: page.pd ? page.pd.eg_int : 0
                        onChanged: (v) => page.pset("eg_int", v) }
                    Repeater {
                        model: ["ATTACK", "DECAY", "BREAK LVL", "SUSTAIN", "SLOPE", "RELEASE"]
                        ParamSlider { width: edCol.width
                            label: "VDF " + modelData
                            value: page.pd ? page.pd.vdf[index] : 0
                            onChanged: (v) => page.psetArr("vdf", index, v) }
                    }

                    // ---- VDA ----
                    Text { text: "VDA  (ampli)"; color: "#e5a13c"
                           font.pixelSize: 11; font.bold: true; font.letterSpacing: 2 }
                    Repeater {
                        model: ["ATTACK", "DECAY", "BREAK LVL", "SUSTAIN", "SLOPE", "RELEASE"]
                        ParamSlider { width: edCol.width
                            label: "VDA " + modelData
                            value: page.pd ? page.pd.vda[index] : 0
                            onChanged: (v) => page.psetArr("vda", index, v) }
                    }

                    // ---- LFO / divers ----
                    Text { text: "LFO / EXPRESSION"; color: "#e5a13c"
                           font.pixelSize: 11; font.bold: true; font.letterSpacing: 2 }
                    ParamSlider { width: edCol.width; label: "LFO RATE"
                        value: page.pd ? page.pd.lfo_rate : 0
                        onChanged: (v) => page.pset("lfo_rate", v) }
                    ParamSlider { width: edCol.width; label: "LFO DEPTH"
                        value: page.pd ? page.pd.lfo_depth : 0
                        onChanged: (v) => page.pset("lfo_depth", v) }
                    ParamSlider { width: edCol.width; label: "LFO DELAY"
                        value: page.pd ? page.pd.lfo_delay : 0
                        onChanged: (v) => page.pset("lfo_delay", v) }
                    ParamSlider { width: edCol.width; label: "VEL SENS"
                        value: page.pd ? page.pd.vel_sens : 0
                        onChanged: (v) => page.pset("vel_sens", v) }
                    ParamSlider { width: edCol.width; label: "LEVEL"
                        value: page.pd ? page.pd.level : 0
                        onChanged: (v) => page.pset("level", v) }

                    Text {
                        text: "Édition LIVE : joue pendant que tu règles — cutoff/level immédiats, enveloppes à la prochaine note. SAUVER persiste la banque."
                        color: "#5c666e"; font.pixelSize: 10
                        width: edCol.width; wrapMode: Text.WordWrap
                    }
                }
            }
        }
    }
}
