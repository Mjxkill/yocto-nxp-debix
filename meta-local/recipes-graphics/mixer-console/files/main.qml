// V10-NATIVE N0 — châssis console + animation témoin + FPS.
// Tokens de la maquette validée : anthracite #14181c, ambre #e5a13c.
import QtQuick

Window {
    id: root
    visible: true
    visibility: Window.FullScreen
    color: "#0c0f12"

    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0.0; color: "#181d22" }
            GradientStop { position: 0.22; color: "#14181c" }
            GradientStop { position: 1.0; color: "#12161a" }
        }

        Column {
            anchors.centerIn: parent
            spacing: 18

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "MODEL AB"
                color: "#e9e5da"
                font.pixelSize: 42
                font.bold: true
                font.letterSpacing: 8
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "CONSOLE NATIVE · N0 · EGLFS SANS WAYLAND"
                color: "#e5a13c"
                font.pixelSize: 15
                font.letterSpacing: 4
            }

            // animation témoin : un "VU" qui respire à 60 fps (coût GPU réel)
            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 6
                Repeater {
                    model: 16
                    Rectangle {
                        width: 14
                        radius: 3
                        height: 40 + 60 * Math.abs(Math.sin(pulse.v + index * 0.42))
                        anchors.bottom: parent.bottom
                        color: index < 10 ? "#4cc470" : (index < 13 ? "#e5a13c" : "#e05545")
                        opacity: 0.9
                    }
                }
            }

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: root.width + "×" + root.height + "  ·  " + fpsMeter.fps + " FPS"
                color: "#8b959d"
                font.pixelSize: 13
                font.family: "monospace"
            }
        }

        QtObject {
            id: pulse
            property real v: 0
            NumberAnimation on v {
                from: 0; to: Math.PI * 2
                duration: 1600
                loops: Animation.Infinite
            }
        }
    }
}
