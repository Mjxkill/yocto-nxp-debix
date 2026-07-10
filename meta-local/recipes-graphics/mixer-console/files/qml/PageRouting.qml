// Page ROUTING — parité web P2m : matrice 1 entrée → 18 sorties (valeurs
// réelles), 18 trims de sortie, remap mics. Knobs console, groupes titrés.
import QtQuick

Item {
    id: page
    property int srcIdx: 0
    property var inNames: []
    property var outNames: []

    Component.onCompleted: {
        const n = [];
        for (let i = 1; i <= 8; i++) n.push("M" + i);
        for (let i = 1; i <= 8; i++) n.push("U" + i);
        n.push("P1"); n.push("P2");
        for (let i = 1; i <= 8; i++) n.push("R" + i);
        inNames = n;
        const o = [];
        for (let i = 1; i <= 8; i++) o.push("S" + i);
        for (let i = 1; i <= 8; i++) o.push("U" + i);
        o.push("P1"); o.push("P2");
        outNames = o;
    }

    onVisibleChanged: if (visible) refresh()

    function refresh() {
        mixer.call({ op: "get_strip_routing", src: srcIdx }, function(r) {
            if (!r.ok || !r.master) return;
            for (let o = 0; o < 18; o++) {
                const k = mxRep.itemAt(o);
                if (k) k.value = r.master[o] > 0.000316
                                 ? Math.max(-60, 20 * Math.log10(r.master[o])) : -60;
            }
        });
        mixer.call({ op: "get_output_gain" }, function(r) {
            if (!r.gains) return;
            for (let o = 0; o < 18; o++) {
                const k = outRep.itemAt(o);
                if (k) k.value = r.gains[o] > 0
                                 ? 20 * Math.log10(r.gains[o] / 1000.0) : -60;
            }
        });
        mixer.call({ op: "get_input_map" }, function(r) {
            if (!r.map) return;
            for (let m = 0; m < 8; m++) {
                const b = remapRep.itemAt(m);
                if (b) b.slot = r.map[m];
            }
        });
    }

    Column {
        anchors.fill: parent
        anchors.margins: 14
        spacing: 12

        // ---- ROUTAGE D'UNE ENTRÉE ----
        Rectangle {
            width: parent.width; height: 205
            color: "#171c21"; radius: 8; border.color: "#060809"
            Column {
                anchors.fill: parent; anchors.margins: 12; spacing: 8
                Row {
                    spacing: 12
                    Text { text: "ROUTAGE D'UNE ENTRÉE"; color: "#e5a13c"; font.pixelSize: 11; font.bold: true; font.letterSpacing: 3; anchors.verticalCenter: parent.verticalCenter }
                    Rectangle {
                        width: 34; height: 26; radius: 4; color: "#1b2126"; border.color: "#39434b"
                        Text { anchors.centerIn: parent; text: "‹"; color: "#e5a13c"; font.pixelSize: 15 }
                        MouseArea { anchors.fill: parent; onClicked: { page.srcIdx = (page.srcIdx + 25) % 26; page.refresh(); } }
                    }
                    Text { text: page.inNames[page.srcIdx] + " — entrée " + page.srcIdx; color: "#e9e5da"; font.pixelSize: 13; font.family: "monospace"; anchors.verticalCenter: parent.verticalCenter }
                    Rectangle {
                        width: 34; height: 26; radius: 4; color: "#1b2126"; border.color: "#39434b"
                        Text { anchors.centerIn: parent; text: "›"; color: "#e5a13c"; font.pixelSize: 15 }
                        MouseArea { anchors.fill: parent; onClicked: { page.srcIdx = (page.srcIdx + 1) % 26; page.refresh(); } }
                    }
                }
                Row {
                    width: parent.width
                    spacing: Math.max(4, (width - 18 * 62) / 17)
                    Repeater {
                        id: mxRep
                        model: 18
                        Column {
                            spacing: 3
                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: page.outNames[index]
                                color: index < 8 ? "#e5a13c" : (index < 16 ? "#8b959d" : "#4cc470")
                                font.pixelSize: 9; font.bold: true
                                font.letterSpacing: 2
                            }
                            Knob {
                                id: mxKnob
                                width: 62; height: 78
                                from: -60; to: 6
                                onMoved: (db) => mixer.call({ op: "set_master", src: page.srcIdx, out: index,
                                    gain: db <= -59 ? 0 : Math.pow(10, db / 20) }, function(){})
                            }
                            property alias value: mxKnob.value
                        }
                    }
                }
            }
        }

        // ---- GAINS DE SORTIE ----
        Rectangle {
            width: parent.width; height: 185
            color: "#171c21"; radius: 8; border.color: "#060809"
            Column {
                anchors.fill: parent; anchors.margins: 12; spacing: 8
                Text { text: "GAINS DE SORTIE · trim final persisté"; color: "#e5a13c"; font.pixelSize: 11; font.bold: true; font.letterSpacing: 3 }
                Row {
                    width: parent.width
                    spacing: Math.max(4, (width - 18 * 62) / 17)
                    Repeater {
                        id: outRep
                        model: 18
                        Column {
                            spacing: 3
                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: page.outNames[index]
                                color: index < 8 ? "#e5a13c" : (index < 16 ? "#8b959d" : "#4cc470")
                                font.pixelSize: 9; font.bold: true
                                font.letterSpacing: 2
                            }
                            Knob {
                                id: outKnob
                                width: 62; height: 78
                                from: -60; to: 12
                                onMoved: (db) => mixer.setOutputGain(index, db)
                            }
                            property alias value: outKnob.value
                        }
                    }
                }
            }
        }

        // ---- REMAP MICS ----
        Rectangle {
            width: parent.width; height: 96
            color: "#171c21"; radius: 8; border.color: "#060809"
            Column {
                anchors.fill: parent; anchors.margins: 12; spacing: 8
                Text { text: "REMAP MICS DSP · tap = source suivante"; color: "#e5a13c"; font.pixelSize: 11; font.bold: true; font.letterSpacing: 3 }
                Row {
                    width: parent.width
                    spacing: Math.max(10, (width - 8 * 92) / 7)
                    Repeater {
                        id: remapRep
                        model: 8
                        Rectangle {
                            property int slot: index
                            width: 92; height: 40; radius: 5
                            color: "#1b2126"; border.color: "#39434b"
                            Column {
                                anchors.centerIn: parent
                                Text { anchors.horizontalCenter: parent.horizontalCenter; text: "M" + (index + 1); color: "#8b959d"; font.pixelSize: 9; font.letterSpacing: 2 }
                                Text { anchors.horizontalCenter: parent.horizontalCenter; text: "src " + (slot + 1); color: "#e9e5da"; font.pixelSize: 13; font.family: "monospace" }
                            }
                            MouseArea {
                                anchors.fill: parent
                                onClicked: {
                                    parent.slot = (parent.slot + 1) % 8;
                                    mixer.call({ op: "set_input_map", mic: index, slot: parent.slot }, function(){});
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
