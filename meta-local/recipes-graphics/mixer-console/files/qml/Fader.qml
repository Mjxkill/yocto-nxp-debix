// Fader à course verticale, tactile. value 0..1 (1 = haut).
import QtQuick

Item {
    id: fader
    property real value: 0.72
    property bool interacting: false
    signal moved(real v)
    width: 38

    Rectangle {   // rail
        id: track
        anchors.fill: parent
        radius: 4
        border.color: "#05070a"
        gradient: Gradient {
            GradientStop { position: 0; color: "#0b0e11" }
            GradientStop { position: 1; color: "#11161a" }
        }
        Rectangle {   // fente centrale
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.topMargin: 10
            anchors.bottomMargin: 10
            width: 3
            radius: 1
            color: "#05070a"
        }
    }

    Rectangle {   // curseur
        id: cap
        anchors.horizontalCenter: parent.horizontalCenter
        width: parent.width - 6
        height: 22
        radius: 3
        y: 10 + (1.0 - fader.value) * (fader.height - 20 - height)
        border.color: "#0a0d10"
        gradient: Gradient {
            GradientStop { position: 0; color: "#3a434b" }
            GradientStop { position: 0.5; color: "#252c32" }
            GradientStop { position: 1; color: "#181d22" }
        }
        // trait ambre EXPLICITE : l'arrêt de dégradé (~0.9 px) passait
        // sous le pixel et disparaissait selon l'arrondi (retour user)
        Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width - 6
            height: 2
            color: "#e5a13c"
        }
    }

    MouseArea {
        anchors.fill: parent
        anchors.margins: -8      // zone tactile élargie
        preventStealing: true
        function apply(my) {
            const usable = fader.height - 20 - cap.height;
            const v = 1.0 - (my - 10 - cap.height / 2) / usable;
            fader.value = Math.max(0, Math.min(1, v));
            fader.moved(fader.value);
        }
        onPressed: (e) => { fader.interacting = true; apply(e.y - 8); }
        onPositionChanged: (e) => apply(e.y - 8)
        onReleased: fader.interacting = false
        onCanceled: fader.interacting = false
    }
}
