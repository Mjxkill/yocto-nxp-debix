// Page EFFETS — 4 bus FX : moteur courant, rack de knobs, sélecteur de
// plugin LV2 (liste chargée une fois, filtres par catégorie).
import QtQuick

Item {
    id: page
    property int bus: 0
    property var fx: ({})
    property var plugins: []
    property string cat: "TOUS"
    property bool listOpen: false

    onVisibleChanged: if (visible) { loadFx(); if (plugins.length === 0) loadPlugins(); }

    function loadFx() {
        mixer.call({ op: "get_fx", bus: bus }, function(r) { if (r.ok) page.fx = r; });
    }
    function loadPlugins() {
        mixer.call({ op: "list_lv2_plugins" }, function(r) {
            if (r.ok && r.plugins) page.plugins = r.plugins;
        });
    }
    function filtered() {
        const re = {
            "DYNAMIQUE": /comp|gate|limit|expander|deess/i,
            "EQ": /\beq\b|equal|filter|shelf/i,
            "REVERB": /reverb|room|hall|plate/i,
            "DELAY": /delay|echo/i
        };
        return plugins.filter(p => (p.cat !== "ins") &&
            (cat === "TOUS" || (re[cat] && re[cat].test(p.name || ""))));
    }
    function grpOf(key) {
        const m = String(key).match(/(?:^|[ _])(\d+)(?:[ _]|$)/);
        return m ? "BANDE " + m[1] : "GÉNÉRAL";
    }

    Row {
        anchors.fill: parent
        anchors.margins: 14
        spacing: 12

        // ---- colonne bus + moteur ----
        Column {
            width: 200; height: parent.height
            spacing: 8
            Repeater {
                model: 4
                Rectangle {
                    width: parent.width; height: 52; radius: 6
                    color: page.bus === index ? "#2a2214" : "#1b2126"
                    border.color: page.bus === index ? "#e5a13c" : "#39434b"
                    Column {
                        anchors.centerIn: parent
                        Text { anchors.horizontalCenter: parent.horizontalCenter; text: "FX " + (index + 1); color: page.bus === index ? "#e5a13c" : "#8b959d"; font.pixelSize: 13; font.bold: true; font.letterSpacing: 2 }
                    }
                    MouseArea { anchors.fill: parent; onClicked: { page.bus = index; page.listOpen = false; page.loadFx(); } }
                }
            }
            Item { width: 1; height: 10 }
            Rectangle {
                width: parent.width; height: 60; radius: 6
                color: "#171c21"; border.color: "#060809"
                Column {
                    anchors.centerIn: parent; spacing: 2
                    Text { anchors.horizontalCenter: parent.horizontalCenter; text: "MOTEUR"; color: "#5c666e"; font.pixelSize: 8; font.letterSpacing: 2 }
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: page.fx.type === "lv2" ? String(page.fx.uri || "?").split("/").pop().slice(0, 20) : (page.fx.type || "—")
                        color: "#e9e5da"; font.pixelSize: 11; font.family: "monospace"
                    }
                }
            }
            Rectangle {
                width: parent.width; height: 40; radius: 6
                color: page.listOpen ? "#2a2214" : "#1b2126"
                border.color: page.listOpen ? "#e5a13c" : "#39434b"
                Text { anchors.centerIn: parent; text: page.listOpen ? "FERMER LA LISTE" : "CHANGER D'EFFET"; color: page.listOpen ? "#e5a13c" : "#e9e5da"; font.pixelSize: 11; font.bold: true }
                MouseArea { anchors.fill: parent; onClicked: page.listOpen = !page.listOpen }
            }
        }

        // ---- rack de knobs OU liste de plugins ----
        Rectangle {
            width: parent.width - 212; height: parent.height
            color: "#171c21"; radius: 8; border.color: "#060809"

            // rack
            Flickable {
                anchors.fill: parent; anchors.margins: 12
                visible: !page.listOpen
                contentHeight: rackFlow.height
                clip: true
                Flow {
                    id: rackFlow
                    width: parent.width
                    spacing: 14
                    Repeater {
                        model: page.fx.params ? Object.keys(page.fx.params) : []
                        Knob {
                            width: 76; height: 100
                            property var meta: (page.fx.meta && page.fx.meta[modelData]) ? page.fx.meta[modelData] : {}
                            from: meta.min !== undefined ? meta.min : 0
                            to: meta.max !== undefined ? meta.max : 1
                            unit: meta.unit || ""
                            Component.onCompleted: value = page.fx.params[modelData]
                            onMoved: (v) => mixer.call({ op: "set_fx_param", bus: page.bus,
                                param: modelData, value: v }, function(){})
                            Text {
                                anchors.top: parent.top; anchors.topMargin: -2
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: (meta.label || modelData).slice(0, 12)
                                color: "#8b959d"; font.pixelSize: 9; font.letterSpacing: 1
                            }
                        }
                    }
                }
                Text {
                    visible: !page.fx.params || Object.keys(page.fx.params).length === 0
                    anchors.centerIn: parent
                    text: page.fx.type ? "Moteur « " + page.fx.type + " » sans paramètres exposés" : "Chargement…"
                    color: "#5c666e"; font.pixelSize: 12
                }
            }

            // liste plugins
            Column {
                anchors.fill: parent; anchors.margins: 12
                visible: page.listOpen
                spacing: 8
                Row {
                    spacing: 6
                    Repeater {
                        model: ["TOUS", "DYNAMIQUE", "EQ", "REVERB", "DELAY"]
                        Rectangle {
                            width: catTxt.width + 22; height: 28; radius: 4
                            color: page.cat === modelData ? "#2a2214" : "#1b2126"
                            border.color: page.cat === modelData ? "#e5a13c" : "#39434b"
                            Text { id: catTxt; anchors.centerIn: parent; text: modelData; color: page.cat === modelData ? "#e5a13c" : "#8b959d"; font.pixelSize: 10; font.bold: true }
                            MouseArea { anchors.fill: parent; onClicked: page.cat = modelData }
                        }
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: page.filtered().length + " effets"
                        color: "#5c666e"; font.pixelSize: 10
                    }
                }
                ListView {
                    width: parent.width; height: parent.height - 44
                    clip: true
                    model: page.filtered()
                    delegate: Rectangle {
                        width: ListView.view.width; height: 40
                        color: index % 2 ? "#191e23" : "transparent"
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left; anchors.leftMargin: 8
                            text: modelData.name || modelData.uri
                            color: "#e9e5da"; font.pixelSize: 12
                        }
                        MouseArea {
                            anchors.fill: parent
                            onClicked: {
                                mixer.call({ op: "set_fx_engine", bus: page.bus,
                                             engine: "lv2", uri: modelData.uri },
                                           function(r) { page.listOpen = false; page.loadFx(); });
                            }
                        }
                    }
                }
            }
        }
    }
}
