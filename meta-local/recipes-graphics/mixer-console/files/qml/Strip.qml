// Tranche console : id, knob gain, fader + meter, mute, sends.
import QtQuick

Item {
    id: strip
    // mapping : type "in"/"out" + index dans les meters
    property string chanType: "in"
    property int chanIndex: 0
    property string name: "M1"
    property string sub: "MIC"
    property real level: 0          // meter 0..1 (posé par la page)
    property bool isOut: chanType === "out"
    // callbacks posés par la page (via signaux)
    signal faderMoved(real db)
    signal gainMoved(real db)
    signal muteToggled(bool m)
    signal sendToggled(int bus, bool on)

    property alias faderValue: fader.value
    // appelée par le tick d'affichage global (1 tick sur 4) — AUCUN timer
    // local : les timers désalignés créaient des frames hors tick (30 fps
    // au lieu de 22)
    function updateDbro() {
        const on = level > 0.003;
        dbro.text = on ? (level * 60 - 60).toFixed(1) : "-∞";
        dbro.color = on ? "#e9e5da" : "#5c666e";
    }
    property bool muted: false
    property var sendsOn: [false, false, false, false]

    Column {
        anchors.fill: parent
        anchors.margins: 6
        spacing: 4

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: strip.name
            color: "#e9e5da"
            font.pixelSize: 14
            font.bold: true
            font.letterSpacing: 2
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: strip.sub
            color: "#8b959d"
            font.pixelSize: 9
            font.letterSpacing: 2
        }

        Knob {
            id: gainKnob
            anchors.horizontalCenter: parent.horizontalCenter
            visible: !strip.isOut
            from: -12; to: 12
            onMoved: (v) => strip.gainMoved(v)
        }
        Item { width: 1; height: strip.isOut ? 74 : 0; visible: strip.isOut }

        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 7
            height: strip.height - 210
            Fader {
                id: fader
                height: parent.height
                onMoved: (v) => strip.faderMoved(v * 84 - 72)   // 0..1 → dB
            }
            MeterBar {
                height: parent.height
                level: strip.level
            }
        }

        Text {
            id: dbro
            anchors.horizontalCenter: parent.horizontalCenter
            text: "-∞"
            color: "#5c666e"
            font.pixelSize: 11
            font.family: "monospace"
        }

        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 5
            visible: !strip.isOut
            Rectangle {
                width: 34; height: 26; radius: 3
                color: strip.muted ? "#e05545" : "#1b2126"
                border.color: strip.muted ? "#f2796a" : "#39434b"
                Text {
                    anchors.centerIn: parent
                    text: "M"
                    color: strip.muted ? "#160505" : "#8b959d"
                    font.pixelSize: 11; font.bold: true
                }
                MouseArea {
                    anchors.fill: parent
                    onClicked: { strip.muted = !strip.muted; strip.muteToggled(strip.muted); }
                }
            }
        }

        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 4
            visible: !strip.isOut
            Repeater {
                model: 4
                Rectangle {
                    width: 29; height: 19; radius: 2
                    color: strip.sendsOn[index] ? "#e5a13c" : "#1b2126"
                    border.color: strip.sendsOn[index] ? "#ffcf7e" : "#39434b"
                    Text {
                        anchors.centerIn: parent
                        text: "F" + (index + 1)
                        color: strip.sendsOn[index] ? "#1d1204" : "#8b959d"
                        font.pixelSize: 9; font.bold: true
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: {
                            const s = strip.sendsOn.slice();
                            s[index] = !s[index];
                            strip.sendsOn = s;
                            strip.sendToggled(index, s[index]);
                        }
                    }
                }
            }
        }
    }
}
