// Knob rotatif SVG-like (Canvas) : arc ambre -135°..+135°, drag vertical.
import QtQuick

Item {
    id: knob
    property real value: 0
    property real from: -12
    property real to: 12
    property string unit: "dB"
    property string label: ""
    property bool interacting: false
    signal moved(real v)

    width: 50
    height: 74

    Canvas {
        id: cv
        width: 50
        height: 50
        anchors.horizontalCenter: parent.horizontalCenter
        onPaint: {
            const ctx = getContext("2d");
            const c = width / 2, r = c - 3;
            const n = (knob.value - knob.from) / (knob.to - knob.from);
            const a0 = -Math.PI * 0.75 - Math.PI / 2;
            const a1 = a0 + n * Math.PI * 1.5;
            ctx.reset();
            // corps
            ctx.beginPath();
            ctx.arc(c, c, r - 3, 0, Math.PI * 2);
            const g = ctx.createRadialGradient(c * 0.7, c * 0.55, 2, c, c, r);
            g.addColorStop(0, "#3a434b");
            g.addColorStop(0.6, "#242b31");
            g.addColorStop(1, "#181d22");
            ctx.fillStyle = g;
            ctx.fill();
            ctx.lineWidth = 1;
            ctx.strokeStyle = "#31383f";
            ctx.stroke();
            // arc ambre
            ctx.beginPath();
            ctx.arc(c, c, r, a0, a1);
            ctx.lineWidth = 2.6;
            ctx.lineCap = "round";
            ctx.strokeStyle = "#e5a13c";
            ctx.stroke();
            // index
            ctx.beginPath();
            ctx.moveTo(c, c);
            ctx.lineTo(c + (r - 6) * Math.cos(a1), c + (r - 6) * Math.sin(a1));
            ctx.lineWidth = 2.4;
            ctx.strokeStyle = "#e9e5da";
            ctx.stroke();
        }
    }
    onValueChanged: cv.requestPaint()

    Text {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: cv.bottom
        anchors.topMargin: 1
        text: knob.value.toFixed(1) + (knob.unit.length ? " " + knob.unit : "")
        color: "#8b959d"
        font.pixelSize: 10
        font.family: "monospace"
    }

    MouseArea {
        anchors.fill: cv
        anchors.margins: -6
        preventStealing: true
        property real y0: 0
        property real v0: 0
        onPressed: (e) => { y0 = e.y; v0 = knob.value; knob.interacting = true; }
        onReleased: knob.interacting = false
        onCanceled: knob.interacting = false
        onPositionChanged: (e) => {
            const span = knob.to - knob.from;
            let v = v0 + (y0 - e.y) * span / 200.0;
            v = Math.max(knob.from, Math.min(knob.to, v));
            knob.value = v;
            knob.moved(v);
        }
    }
}
