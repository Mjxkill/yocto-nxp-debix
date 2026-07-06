// V12-SMP — Page PADS : grille 4×4 de déclencheurs de samples.
// Les WAVs vivent dans /var/lib/ala/samples (slot = ordre alphabétique) ;
// niveau/routage = tranches P1/P2 du mixer (le sampleur est une source).
import QtQuick

Item {
    id: page
    property var slots: []

    onVisibleChanged: if (visible) refresh()
    Timer { interval: 500; running: page.visible; repeat: true; onTriggered: page.refresh() }

    function refresh() {
        mixer.call({ op: "sampler_list" }, function(r) {
            if (r.ok) page.slots = r.slots;
        });
    }

    Column {
        anchors.fill: parent
        anchors.margins: 14
        spacing: 10

        Row {
            width: parent.width
            spacing: 12
            Text {
                text: "PADS · " + "/var/lib/ala/samples"
                color: "#e5a13c"; font.pixelSize: 11; font.bold: true
                font.letterSpacing: 3
                anchors.verticalCenter: parent.verticalCenter
            }
            Item { width: parent.width - 460; height: 1 }
            Rectangle {
                width: 120; height: 32; radius: 5
                color: "#1b2126"; border.color: "#39434b"
                Text { anchors.centerIn: parent; text: "RECHARGER"; color: "#8b959d"; font.pixelSize: 10; font.bold: true }
                TapHandler {
                    gesturePolicy: TapHandler.ReleaseWithinBounds
                    onTapped: mixer.call({ op: "sampler_reload" },
                                         function() { page.refresh(); })
                }
            }
            Rectangle {
                width: 120; height: 32; radius: 5
                color: "#2a1512"; border.color: "#7a3b32"
                Text { anchors.centerIn: parent; text: "STOP ALL"; color: "#f2796a"; font.pixelSize: 10; font.bold: true }
                TapHandler {
                    gesturePolicy: TapHandler.ReleaseWithinBounds
                    onTapped: mixer.call({ op: "sampler_stop", slot: -1 },
                                         function() { page.refresh(); })
                }
            }
        }

        Grid {
            columns: 4
            columnSpacing: 10
            rowSpacing: 10
            width: parent.width
            Repeater {
                model: 16
                Rectangle {
                    property var sl: index < page.slots.length ? page.slots[index] : null
                    property bool hasWav: sl !== null && sl.name !== ""
                    property bool playing: hasWav && sl.playing === 1
                    width: (page.width - 28 - 30) / 4
                    height: 118
                    radius: 8
                    color: playing ? "#3a2c10" : (hasWav ? "#1b2126" : "#14181c")
                    border.color: playing ? "#e5a13c" : (hasWav ? "#39434b" : "#22282e")
                    border.width: playing ? 2 : 1

                    Column {
                        anchors.centerIn: parent
                        spacing: 6
                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: parent.parent.hasWav ? parent.parent.sl.name : "—"
                            color: parent.parent.playing ? "#e5a13c"
                                   : (parent.parent.hasWav ? "#e9e5da" : "#3a434b")
                            font.pixelSize: 15; font.bold: true
                            elide: Text.ElideRight
                            width: Math.min(implicitWidth, 200)
                        }
                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: !parent.parent.hasWav ? ""
                                  : parent.parent.playing
                                    ? parent.parent.sl.pos_s.toFixed(1) + " / "
                                      + parent.parent.sl.len_s.toFixed(1) + " s"
                                    : parent.parent.sl.len_s.toFixed(1) + " s"
                            color: "#8b959d"; font.pixelSize: 11
                            font.family: "monospace"
                        }
                    }

                    TapHandler {
                        enabled: parent.hasWav
                        gesturePolicy: TapHandler.ReleaseWithinBounds
                        onTapped: mixer.call({ op: "sampler_trigger",
                                               slot: index, gain_db: 0 },
                                             function() { page.refresh(); })
                    }
                }
            }
        }

        Text {
            text: "TAP = lecture (re-tap = relance) · niveau et routage sur les tranches P1/P2 du MIXER"
            color: "#5c666e"; font.pixelSize: 10; font.letterSpacing: 1
        }
    }
}
