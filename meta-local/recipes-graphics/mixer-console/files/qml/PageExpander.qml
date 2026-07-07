// V12-MIDIX-GUI — Page EXPANDEUR : module de sons MIDI multi-timbral.
// Affectation d'un son (programme GM) PAR CANAL MIDI 1-16 ; le daemon
// midi-expander (fluidsynth) est piloté via l'op proxy midix_ctl de
// mixer-pro. VU/présence via get_midix. Canal 10 = batterie (GM).
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
    property var chans: [0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]
    property var acts:  [0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]
    property bool draggingGain: false

    onVisibleChanged: if (visible) { pollStatus(); }
    // statut (programmes + activité par canal) 4 Hz — via proxy daemon
    Timer { interval: 250; running: page.visible; repeat: true; onTriggered: page.pollStatus() }
    // VU 4 Hz — lecture directe mixer-pro (pas de proxy)
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
            if (!page.draggingGain) page.synthGain = r.gain;
        });
    }
    function setProg(chan, num) {
        if (num < 0) num = 127;
        if (num > 127) num = 0;
        mixer.call({ op: "midix_ctl", cmd: "prog", chan: chan, num: num },
                   function() { page.pollStatus(); });
    }

    Column {
        anchors.fill: parent
        anchors.margins: 14
        spacing: 10

        // ===================== EN-TÊTE =====================
        Rectangle {
            width: parent.width; height: 86
            color: "#171c21"; radius: 8; border.color: "#060809"
            Row {
                anchors.fill: parent; anchors.margins: 12; spacing: 16

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
                                  ? (page.sf2 !== "" ? page.sf2 : "en ligne")
                                  : "module absent"
                            color: page.present ? "#8b959d" : "#e05545"
                            font.pixelSize: 11
                        }
                    }
                    // VU
                    Rectangle {
                        width: 200; height: 8; radius: 4; color: "#0b0e11"
                        Rectangle {
                            height: parent.height; radius: 4
                            width: {
                                var f = page.peak / 2147483647.0;
                                if (f <= 0) return 0;
                                var db = 20 * Math.log(f) / Math.LN10;
                                return parent.width * Math.max(0, Math.min(1, (db + 48) / 48));
                            }
                            color: "#4cc470"
                        }
                    }
                }

                // volume synthé
                Column {
                    spacing: 4
                    anchors.verticalCenter: parent.verticalCenter
                    Text { text: "VOLUME  " + page.synthGain.toFixed(2)
                           color: "#8b959d"; font.pixelSize: 10; font.letterSpacing: 1 }
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

                Item { width: parent.width - 620; height: 1 }

                Rectangle {
                    width: 100; height: 48; radius: 6
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
                        property bool isDrums: index === 9
                        property int prog: index < page.chans.length ? page.chans[index] : 0
                        width: parent.width
                        height: 58
                        radius: 6
                        color: "#14181c"
                        border.color: isDrums ? "#5a4a2a" : "#22282e"

                        Row {
                            anchors.fill: parent
                            anchors.margins: 8
                            spacing: 14

                            Rectangle {
                                width: 64; height: 42; radius: 5
                                anchors.verticalCenter: parent.verticalCenter
                                color: "#1b2126"; border.color: "#39434b"
                                Text { anchors.centerIn: parent
                                       text: "CH " + (index + 1)
                                       color: "#e5a13c"; font.pixelSize: 13; font.bold: true }
                            }

                            Rectangle {
                                width: 42; height: 42; radius: 5
                                anchors.verticalCenter: parent.verticalCenter
                                color: "#1b2126"; border.color: "#39434b"
                                Text { anchors.centerIn: parent; text: "‹"
                                       color: "#c9c4b8"; font.pixelSize: 20 }
                                TapHandler {
                                    gesturePolicy: TapHandler.ReleaseWithinBounds
                                    onTapped: page.setProg(index, prog - 1)
                                }
                            }
                            Column {
                                width: 300
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 2
                                Text {
                                    text: isDrums ? "Drum Kit " + prog + " (batterie GM)"
                                                  : page.gmNames[prog]
                                    color: "#e9e5da"; font.pixelSize: 14; font.bold: true
                                    elide: Text.ElideRight; width: parent.width
                                }
                                Text {
                                    text: "programme " + prog
                                    color: "#5c666e"; font.pixelSize: 10
                                    font.family: "monospace"
                                }
                            }
                            Rectangle {
                                width: 42; height: 42; radius: 5
                                anchors.verticalCenter: parent.verticalCenter
                                color: "#1b2126"; border.color: "#39434b"
                                Text { anchors.centerIn: parent; text: "›"
                                       color: "#c9c4b8"; font.pixelSize: 20 }
                                TapHandler {
                                    gesturePolicy: TapHandler.ReleaseWithinBounds
                                    onTapped: page.setProg(index, prog + 1)
                                }
                            }

                            // V12-VU : vumètre d'activité MIDI du canal
                            // (note-on/vélocité, retombée ~500 ms côté daemon)
                            Rectangle {
                                width: 170; height: 12; radius: 6
                                anchors.verticalCenter: parent.verticalCenter
                                color: "#0b0e11"
                                Rectangle {
                                    height: parent.height; radius: 6
                                    width: parent.width * Math.min(1,
                                           (index < page.acts.length
                                            ? page.acts[index] : 0) / 1000.0)
                                    color: isDrums ? "#e8b84b" : "#4cc470"
                                }
                            }
                        }
                    }
                }
            }
        }

        Text {
            text: "DAW/clavier → port MIDI USB « Debix UAC2 8x8 MIDI 1 » · le son sort sur les tranches P1/P2 du MIXER · CH 10 = batterie"
            color: "#5c666e"; font.pixelSize: 10; font.letterSpacing: 1
            width: parent.width; wrapMode: Text.WordWrap
        }
    }
}
