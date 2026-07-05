// Bargraph vertical vert→ambre→rouge, cible 0..1 à 30 Hz,
// ballistique attack/release par Behavior (mêmes constantes que le web).
// Le dégradé est à ÉCHELLE FIXE (clip par fenêtre), pas étiré par le niveau.
import QtQuick

Rectangle {
    id: bar
    property real level: 0        // cible 0..1 (posée à 30 Hz)
    property real shown: 0
    width: 12
    radius: 3
    color: "#0b0e11"
    border.color: "#05070a"

    // ballistique faite en C++ (MixerClient) — binding direct, pas d'animation
    onLevelChanged: shown = level

    Item {
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: 1
        height: (bar.height - 2) * bar.shown
        clip: true

        Rectangle {
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            height: bar.height - 2
            gradient: Gradient {
                GradientStop { position: 0.0; color: "#e05545" }
                GradientStop { position: 0.12; color: "#e5a13c" }
                GradientStop { position: 0.35; color: "#4cc470" }
                GradientStop { position: 1.0; color: "#2e6e48" }
            }
        }
    }
}
