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
                // couleur UNIE sans opacity : les 64 barres restent dans
                // UN batch scenegraph (le gradient+opacity par barre =
                // 64 draw calls → 36 fps, saccades)
                Rectangle {
                    anchors.bottom: parent.bottom
                    width: parent.width
                    height: parent.height * parent.v * 0.92
                    color: "#3da55f"
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

    // ---- alimentation : lissage par frame vsync (cibles 10 Hz) ----
    FrameAnimation {
        running: true
        onTriggered: {
            const dt = Math.min(frameTime, 0.1);
            const kA = 1 - Math.exp(-dt / 0.025);
            const kR = 1 - Math.exp(-dt / 0.090);
            const sp = mixer.spectrum;
            for (let i = 0; i < box.nb && i < sp.length; i++) {
                const item = barsRep.itemAt(i);
                if (!item) continue;
                const t = sp[i];
                item.v += (t - item.v) * (t > item.v ? kA : kR);
            }
        }
    }
    Connections {
        target: mixer
        function onInsertChanged() {
            env.pts = mixer.mlActive ? mixer.mlEnvL : [];
            env.requestPaint();      // 5 Hz max, petit canvas
        }
    }
}
