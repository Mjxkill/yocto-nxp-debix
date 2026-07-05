// Overlay de calibration : 5 mires en coordonnées FENÊTRE (hors scène
// tournée — la position des croix est donc exacte par construction).
// 3 taps par mire (moyenne), puis LSQ + composition côté C++.
import QtQuick

Rectangle {
    id: overlay
    anchors.fill: parent
    color: "#0c0f12"
    visible: calib.active
    z: 1000

    property int step: 0
    property int tapCount: 0
    property var acc: ({ x: 0, y: 0 })
    property var rawPts: []
    property var targets: []

    onVisibleChanged: {
        if (visible) {
            step = 0; tapCount = 0; acc = { x: 0, y: 0 }; rawPts = [];
            const mx = 0.08 * width, my = 0.08 * height;
            targets = [
                { x: mx,          y: my },
                { x: width - mx,  y: my },
                { x: width / 2,   y: height / 2 },
                { x: mx,          y: height - my },
                { x: width - mx,  y: height - my }
            ];
        }
    }

    Connections {
        target: calib
        function onRawTouch(x, y) {
            if (!overlay.visible || overlay.step >= 5) return;
            overlay.acc = { x: overlay.acc.x + x, y: overlay.acc.y + y };
            overlay.tapCount++;
            if (overlay.tapCount >= 3) {
                const pts = overlay.rawPts.slice();
                pts.push({ x: overlay.acc.x / 3, y: overlay.acc.y / 3 });
                overlay.rawPts = pts;
                overlay.acc = { x: 0, y: 0 };
                overlay.tapCount = 0;
                overlay.step++;
                if (overlay.step >= 5)
                    calib.finish(overlay.rawPts, overlay.targets,
                                 overlay.width, overlay.height);
            }
        }
    }

    // croix courante
    Item {
        visible: overlay.step < 5 && overlay.targets.length === 5
        x: overlay.targets.length === 5 ? overlay.targets[Math.min(overlay.step, 4)].x : 0
        y: overlay.targets.length === 5 ? overlay.targets[Math.min(overlay.step, 4)].y : 0
        Rectangle { x: -22; y: -1.5; width: 44; height: 3; color: "#e5a13c" }
        Rectangle { x: -1.5; y: -22; width: 3; height: 44; color: "#e5a13c" }
        Rectangle {
            x: -14; y: -14; width: 28; height: 28; radius: 14
            color: "transparent"; border.color: "#e5a13c"; border.width: 2
        }
    }

    // texte d'instructions — dans un item tourné pour rester lisible
    Item {
        anchors.centerIn: parent
        rotation: 270
        Column {
            anchors.centerIn: parent
            spacing: 8
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "CALIBRATION TACTILE"
                color: "#e5a13c"; font.pixelSize: 22; font.bold: true; font.letterSpacing: 6
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "Presse le centre de la croix — 3 appuis par cible"
                color: "#8b959d"; font.pixelSize: 14
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "Cible " + Math.min(overlay.step + 1, 5) + " / 5   ·   appui " + (overlay.tapCount + 1) + " / 3"
                color: "#e9e5da"; font.pixelSize: 16; font.family: "monospace"
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "(l'application redémarre à la fin)"
                color: "#5c666e"; font.pixelSize: 11
            }
        }
    }
}
