// VU-mètre à aiguille — version scenegraph :
// le cadran (Canvas) est peint UNE fois ; l'aiguille est un Rectangle
// tourné par le GPU (transform), zéro repaint logiciel par frame.
// (v1 Canvas+shadowBlur par frame : 117 % CPU — leçon N1)
import QtQuick

Item {
    id: vu
    property real level: 0        // 0..1
    property string channel: "L"
    property real shown: 0

    onLevelChanged: shown = level   // lissage en amont (C++)

    Canvas {   // cadran statique
        id: dial
        anchors.fill: parent
        onPaint: {
            const ctx = getContext("2d");
            const w = width, h = height;
            ctx.reset();
            const grad = ctx.createRadialGradient(w/2, -h*0.1, 4, w/2, h*0.4, w*0.9);
            grad.addColorStop(0, "#2a2013");
            grad.addColorStop(0.46, "#191411");
            grad.addColorStop(1, "#100d0b");
            ctx.fillStyle = grad;
            ctx.fillRect(0, 0, w, h);
            // PIVOT commun aiguille/graduations : (w/2, h*1.12), arc ±50.4°
            const cx = w / 2, cy = h * 1.12, r = h * 0.92;
            for (let i = 0; i <= 10; i++) {
                const a = -Math.PI * 0.28 + (i / 10) * Math.PI * 0.56;
                ctx.beginPath();
                ctx.moveTo(cx + r * Math.sin(a), cy - r * Math.cos(a));
                ctx.lineTo(cx + (r - 6) * Math.sin(a), cy - (r - 6) * Math.cos(a));
                ctx.lineWidth = i >= 8 ? 2 : 1;
                ctx.strokeStyle = i >= 8 ? "#e05545" : "#8a6f45";
                ctx.stroke();
            }
        }
        Component.onCompleted: requestPaint()
    }

    Rectangle {   // aiguille GPU — même pivot que les graduations
        id: needle
        width: 2.4
        height: vu.height * 0.90            // pointe au milieu des ticks (r-6)
        radius: 1
        color: "#e9e5da"
        antialiasing: true
        x: vu.width / 2 - width / 2
        y: vu.height * 1.12 - height        // bas du rect = pivot (w/2, 1.12h)
        transformOrigin: Item.Bottom
        rotation: -50.4 + vu.shown * 100.8  // ±50.4° = ±0.28π, aligné ticks
    }

    Text {
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 3
        anchors.horizontalCenter: parent.horizontalCenter
        text: vu.channel
        color: "#8a6f45"
        font.pixelSize: 9
        font.letterSpacing: 3
    }
}
