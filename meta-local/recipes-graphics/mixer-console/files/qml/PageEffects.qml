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
    // envoi throttlé des paramètres (45 ms + trailing) — le knob est déjà
    // en écho local, le moteur suit sans inonder le socket
    property var _pv: ({})
    property double _pt: 0
    Timer {
        id: trail
        interval: 50
        onTriggered: page.flushParams()
    }
    function sendParam(param, v) {
        _pv[param] = v;
        const now = Date.now();
        if (now - _pt > 45) { flushParams(); }
        else trail.restart();
    }
    function flushParams() {
        _pt = Date.now();
        for (const k in _pv)
            mixer.call({ op: "set_fx_param", bus: bus, param: k,
                         value: _pv[k] }, function() {});
        _pv = {};
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
            Item { width: 1; height: 2 }

            // VU du bus courant : ENTRÉE (send, pré-FX) / SORTIE (return)
            Column {
                id: fxVu
                width: parent.width
                spacing: 6
                property real vin: 0
                property real vout: 0
                FrameAnimation {
                    running: page.visible
                    onTriggered: {
                        const dt = Math.min(frameTime, 0.1);
                        const kA = 1 - Math.exp(-dt / 0.030);
                        const kR = 1 - Math.exp(-dt / 0.120);
                        const fxl = mixer.fxLevels, inl = mixer.inLevels;
                        const b = page.bus * 2;
                        var ti = 0, to = 0;
                        if (fxl.length > b + 1)
                            ti = Math.max(fxl[b], fxl[b + 1]);
                        if (inl.length > 19 + b)
                            to = Math.max(inl[18 + b], inl[19 + b]);
                        fxVu.vin  += (ti - fxVu.vin)  * (ti > fxVu.vin  ? kA : kR);
                        fxVu.vout += (to - fxVu.vout) * (to > fxVu.vout ? kA : kR);
                    }
                }
                Text { text: "ENTRÉE (SENDS)"; color: "#5c666e"
                       font.pixelSize: 9; font.letterSpacing: 2 }
                Rectangle {
                    width: parent.width; height: 26; radius: 13; color: "#0b0e11"
                    Rectangle { height: parent.height; radius: 13
                                width: parent.width * fxVu.vin; color: "#4cc470" }
                }
                Text { text: "SORTIE (RETOUR)"; color: "#5c666e"
                       font.pixelSize: 9; font.letterSpacing: 2 }
                Rectangle {
                    width: parent.width; height: 26; radius: 13; color: "#0b0e11"
                    Rectangle { height: parent.height; radius: 13
                                width: parent.width * fxVu.vout; color: "#e5a13c" }
                }
            }

            Item { width: 1; height: 2 }
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
            // retrait clair de l'effet : retour au bus NEUTRE (passthrough)
            Rectangle {
                width: parent.width; height: 40; radius: 6
                visible: !!page.fx.type && page.fx.type !== "passthrough"
                color: "#2a1512"; border.color: "#7a3b32"
                Text { anchors.centerIn: parent; text: "✕ RETIRER L'EFFET"
                       color: "#f2796a"; font.pixelSize: 11; font.bold: true }
                TapHandler {
                    gesturePolicy: TapHandler.ReleaseWithinBounds
                    onTapped: mixer.call({ op: "set_fx_engine", bus: page.bus,
                                           engine: "passthrough" },
                                         function() { page.loadFx(); })
                }
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
                        Column {
                            width: 84
                            spacing: 4
                            property var meta: (page.fx.meta && page.fx.meta[modelData]) ? page.fx.meta[modelData] : {}
                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                width: parent.width
                                horizontalAlignment: Text.AlignHCenter
                                elide: Text.ElideRight
                                text: (meta.label || modelData)
                                color: "#5c666e"; font.pixelSize: 9
                                font.letterSpacing: 2
                            }
                            Knob {
                                anchors.horizontalCenter: parent.horizontalCenter
                                from: meta.min !== undefined ? meta.min : 0
                                to: meta.max !== undefined ? meta.max : 1
                                unit: meta.unit || ""
                                Component.onCompleted: value = page.fx.params[modelData]
                                onMoved: (v) => page.sendParam(modelData, v)
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
                    header: Rectangle {
                        width: ListView.view ? ListView.view.width : 0; height: 40
                        color: "#1c1410"
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left; anchors.leftMargin: 8
                            text: "— AUCUN EFFET (bus neutre) —"
                            color: "#e5a13c"; font.pixelSize: 12; font.bold: true
                        }
                        MouseArea {
                            anchors.fill: parent
                            onClicked: mixer.call({ op: "set_fx_engine",
                                bus: page.bus, engine: "passthrough" },
                                function() { page.listOpen = false; page.loadFx(); })
                        }
                    }
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
