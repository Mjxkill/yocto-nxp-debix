// V13.2 — Bouton à APPUI LONG : anneau de progression autour du libellé,
// relâcher avant la fin = annuler. holdMs = 0 → simple tap (même API).
// Canvas repeint uniquement pendant l'appui (transitoire — leçons GPU N1/N8).
import QtQuick

Rectangle {
    id: btn
    property string label: ""
    property color accent: "#f2796a"
    property color bg: "#2a1512"
    property color disabledBorder: "#22282e"
    property color disabledText: "#3a434b"
    property int holdMs: 900
    property int fontSize: 12
    signal triggered()

    property real hold: 0
    width: 120; height: 44; radius: 6
    color: bg
    border.color: enabled ? accent : disabledBorder
    border.width: 1
    scale: area.pressed && enabled ? 0.96 : 1.0
    Behavior on scale { NumberAnimation { duration: 80 } }

    Text {
        anchors.centerIn: parent
        text: btn.label
        color: btn.enabled ? btn.accent : btn.disabledText
        font.pixelSize: btn.fontSize; font.bold: true
        opacity: (area.pressed && btn.enabled && btn.holdMs > 0) ? 0.35 : 1.0
    }
    Canvas {
        id: ring
        anchors.centerIn: parent
        width: 40; height: 40
        visible: btn.hold > 0
        onPaint: {
            const ctx = getContext("2d");
            ctx.reset();
            ctx.beginPath();
            ctx.arc(20, 20, 16, -Math.PI / 2,
                    -Math.PI / 2 + btn.hold * 2 * Math.PI);
            ctx.lineWidth = 4;
            ctx.lineCap = "round";
            ctx.strokeStyle = btn.accent;
            ctx.stroke();
        }
    }
    onHoldChanged: {
        ring.requestPaint();
        if (hold >= 1) {
            holdAnim.stop();
            hold = 0;
            btn.triggered();
        }
    }
    NumberAnimation {
        id: holdAnim
        target: btn; property: "hold"
        from: 0; to: 1; duration: Math.max(1, btn.holdMs)
    }
    MouseArea {
        id: area
        anchors.fill: parent
        anchors.margins: -8
        onPressed: if (btn.holdMs > 0) holdAnim.start()
        onReleased: { holdAnim.stop(); btn.hold = 0; ring.requestPaint(); }
        onCanceled: { holdAnim.stop(); btn.hold = 0; ring.requestPaint(); }
        onClicked: if (btn.holdMs <= 0) btn.triggered()
    }
}
