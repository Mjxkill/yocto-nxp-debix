// SPDX-License-Identifier: GPL-2.0-or-later
// Ligne biquad RBJ (type + freq + Q + gain -> blob TAC)
// Extrait de StripFxDrawer.qml (V14.0 étape 8). Instancié via le wrapper
// Component du drawer -> la chaîne de contextes QML est inchangée : `ctl`
// (propriété du Loader délégué) et `drawer` (id du document parent) se
// résolvent comme avant l'extraction.
import QtQuick
import "stripfx.js" as FX

Row {
    spacing: 16
    height: 92
    property var bp: drawer.bqParams[ctl.fullName]
                     || { type: 0, fHz: 1000, q: 0.707, gainDb: 0 }
    function push(partial) {
        const cur = Object.assign({}, bp, partial);
        const all = drawer.bqParams; all[ctl.fullName] = cur; drawer.bqParams = all;
        drawer.setAlsa(ctl, FX.rbjBlob(cur.type, cur.fHz, cur.q, cur.gainDb, 48000).join(","));
    }
    Text {
        width: 90
        anchors.verticalCenter: parent.verticalCenter
        text: ctl.short.replace(" Coefs", "")
        color: "#e5a13c"; font.pixelSize: 12; font.bold: true
    }
    Rectangle {
        anchors.verticalCenter: parent.verticalCenter
        width: 130; height: 30; radius: 4
        color: "#1b2126"; border.color: "#39434b"
        Text { anchors.centerIn: parent; text: FX.BIQUAD_TYPES[bp.type]; color: "#e9e5da"; font.pixelSize: 11 }
        TapHandler { onTapped: push({ type: (bp.type + 1) % FX.BIQUAD_TYPES.length }) }
    }
    Knob {
        visible: bp.type !== 0
        anchors.verticalCenter: parent.verticalCenter
        from: 20; to: 22000; unit: "Hz"
        Component.onCompleted: value = bp.fHz
        onMoved: (v) => push({ fHz: Math.round(v) })
    }
    Knob {
        visible: bp.type !== 0
        anchors.verticalCenter: parent.verticalCenter
        from: 0.1; to: 10; unit: "Q"
        Component.onCompleted: value = bp.q
        onMoved: (v) => push({ q: v })
    }
    Knob {
        visible: [5, 6, 7].indexOf(bp.type) >= 0
        anchors.verticalCenter: parent.verticalCenter
        from: -24; to: 24; unit: "dB"
        Component.onCompleted: value = bp.gainDb
        onMoved: (v) => push({ gainDb: v })
    }
}
