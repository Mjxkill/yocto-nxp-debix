// Spectre 64 barres + enveloppe ML NPU superposée (ligne ambre).
// Barres = Rectangles scenegraph (GPU), maj impérative 10 Hz + epsilon ;
// enveloppe = Canvas 60x plus petit repeint à 5 Hz max (leçon N1 : jamais
// de repaint plein cadre par frame).
import QtQuick

Rectangle {
    id: box
    color: "#0b0e11"
    border.color: "#05070a"
    radius: 6
    clip: true

    property int nb: 64

    // ---- barres du spectre ----
    Row {
        id: barsRow
        anchors.fill: parent
        anchors.margins: 3
        spacing: 1
        Repeater {
            id: barsRep
            model: box.nb
            Item {
                width: (barsRow.width - (box.nb - 1)) / box.nb
                height: barsRow.height
                property real v: 0
                Rectangle {
                    anchors.bottom: parent.bottom
                    width: parent.width
                    height: parent.height * parent.v * 0.92
                    gradient: Gradient {
                        GradientStop { position: 0.0; color: "#4cc470" }
                        GradientStop { position: 1.0; color: "#2e6e48" }
                    }
                    opacity: 0.85
                }
            }
        }
    }

    // ---- enveloppe ML (courbe ambre) ----
    Canvas {
        id: env
        anchors.fill: parent
        anchors.margins: 3
        property var pts: []
        onPaint: {
            const ctx = getContext("2d");
            ctx.reset();
            if (pts.length < 2)
                return;
            ctx.beginPath();
            for (let i = 0; i < pts.length; i++) {
                const x = i / (pts.length - 1) * width;
                // gains dB ±14 → écran (0 dB au centre)
                const y = height / 2 - (pts[i] / 14.0) * (height / 2 - 4);
                if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
            }
            ctx.lineWidth = 1.8;
            ctx.strokeStyle = "#e5a13c";
            ctx.stroke();
        }
    }

    Text {
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.margins: 6
        text: "SPECTRE · " + (mixer.mlActive ? "ENVELOPPE ML 64PT" : "ML INACTIF")
        color: mixer.mlActive ? "#e5a13c" : "#5c666e"
        font.pixelSize: 9
        font.letterSpacing: 3
    }

    // ---- alimentation (impérative, epsilon) ----
    Connections {
        target: mixer
        function onSpectrumChanged() {
            const sp = mixer.spectrum;
            for (let i = 0; i < box.nb && i < sp.length; i++) {
                const item = barsRep.itemAt(i);
                if (item && Math.abs(item.v - sp[i]) > 0.01)
                    item.v = sp[i];
            }
        }
        function onInsertChanged() {
            env.pts = mixer.mlActive ? mixer.mlEnvL : [];
            env.requestPaint();      // 5 Hz max, petit canvas
        }
    }
}
