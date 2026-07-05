// V10-N6 — Intro de démarrage : A.L.A. métal + ombre, reflet de lumière
// balayant les lettres (100 % GPU : 3 couches de texte + bande clipée).
// S'efface en fondu quand le mixer est connecté (min 3 s d'affichage).
import QtQuick

Rectangle {
    id: intro
    anchors.fill: parent
    z: 900
    visible: opacity > 0
    opacity: 1

    gradient: Gradient {
        GradientStop { position: 0.0; color: "#0a0d10" }
        GradientStop { position: 0.5; color: "#12161a" }
        GradientStop { position: 1.0; color: "#080a0c" }
    }

    property bool done: false
    property double t0: 0
    Component.onCompleted: t0 = Date.now()

    Timer {
        interval: 250; running: !intro.done; repeat: true
        onTriggered: {
            // V10-N6f : rester affiché tant que le tac-reset de boot tourne
            // (transitoires visibles sur les VU-mètres sinon)
            if (mixer.connected && boot.tacResetDone
                    && Date.now() - intro.t0 > 3000) {
                intro.done = true;
                fadeOut.start();
            }
        }
    }
    NumberAnimation {
        id: fadeOut
        target: intro; property: "opacity"
        to: 0; duration: 700; easing.type: Easing.InQuad
    }

    // halo ambré qui respire derrière le logo
    Rectangle {
        anchors.centerIn: parent
        width: 700; height: 340; radius: 170
        color: "#e5a13c"
        opacity: 0.05 + 0.04 * Math.sin(glow.v)
        QtObject {
            id: glow
            property real v: 0
            NumberAnimation on v { from: 0; to: Math.PI * 2; duration: 3400; loops: Animation.Infinite }
        }
    }

    Column {
        id: logoCol
        anchors.centerIn: parent
        spacing: 10

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "by ELECTROSENS R&D"
            color: "#8b959d"
            font.pixelSize: 15
            font.letterSpacing: 6
        }

        Item {
            id: logo
            anchors.horizontalCenter: parent.horizontalCenter
            width: alaBase.width
            height: alaBase.height

            // ombre portée
            Text {
                x: 5; y: 7
                text: "A.L.A."
                color: "#000000"; opacity: 0.65
                font.pixelSize: 160; font.bold: true; font.letterSpacing: 14
            }
            // corps métal
            Text {
                id: alaBase
                text: "A.L.A."
                color: "#b9c2ca"
                font.pixelSize: 160; font.bold: true; font.letterSpacing: 14
            }
            // biseau haut (fausse lumière zénithale)
            Text {
                y: -2
                text: "A.L.A."
                color: "#ffffff"; opacity: 0.22
                font.pixelSize: 160; font.bold: true; font.letterSpacing: 14
            }
            // reflet balayant : copie lumineuse visible dans une bande mobile
            Item {
                id: sheenBand
                width: 130
                height: logo.height
                clip: true
                x: -width
                NumberAnimation on x {
                    from: -130; to: logo.width + 130
                    duration: 2100
                    loops: Animation.Infinite
                    easing.type: Easing.InOutSine
                }
                Text {
                    x: -sheenBand.x
                    text: "A.L.A."
                    color: "#fff6e0"
                    font.pixelSize: 160; font.bold: true; font.letterSpacing: 14
                }
            }
        }

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "AUDIO LIVE ASSISTANT"
            color: "#e5a13c"
            font.pixelSize: 24
            font.letterSpacing: 12
        }

        Item { width: 1; height: 26 }

        // ligne de progression discrète
        Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter
            width: 320; height: 3; radius: 2
            color: "#1b2126"
            Rectangle {
                width: mixer.connected
                       ? (boot.tacResetDone ? parent.width : parent.width * 0.65)
                       : parent.width * 0.25
                height: parent.height; radius: 2
                color: "#e5a13c"
                Behavior on width { NumberAnimation { duration: 500 } }
            }
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: mixer.connected
                  ? (boot.tacResetDone ? "MOTEUR AUDIO CONNECTÉ"
                                       : "INITIALISATION DES CONVERTISSEURS…")
                  : "DÉMARRAGE DU MOTEUR AUDIO…"
            color: "#5c666e"
            font.pixelSize: 11
            font.letterSpacing: 3
        }
    }
}
