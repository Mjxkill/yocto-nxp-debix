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
        // parité web inferGroup : groupe de ports LV2 (meta.grp) d'abord,
        // sinon numéro dans le LIBELLÉ ou la clé, sinon GÉNÉRAL
        const m = (page.fx.meta && page.fx.meta[key]) ? page.fx.meta[key] : {};
        if (m.grp) return m.grp;
        const s = m.label || String(key);
        const mb = String(s).match(/(?:^|[ _])(\d+)(?:[ _]|$)/);
        return mb ? "BANDE " + mb[1] : "GÉNÉRAL";
    }

    // ---- composants de paramètre du rack (parité types web) ----
    Component {
        id: fxToggle
        Rectangle {
            property bool on: (page.fx.params[pkey] || 0) > 0.5
            width: 74; height: 30; radius: 15
            color: on ? "#2a2214" : "#1b2126"
            border.color: on ? "#e5a13c" : "#39434b"
            Text { anchors.centerIn: parent; text: parent.on ? "ON" : "OFF"
                   color: parent.on ? "#e5a13c" : "#5c666e"
                   font.pixelSize: 11; font.bold: true }
            TapHandler {
                margin: 6
                gesturePolicy: TapHandler.ReleaseWithinBounds
                onTapped: { parent.on = !parent.on;
                            page.sendParam(pkey, parent.on ? 1 : 0); }
            }
        }
    }
    Component {
        id: fxEnum
        Rectangle {
            property var opts: {
                const o = [];
                String(pmeta.sp || "").split(";").forEach(t => {
                    const i = t.indexOf("=");
                    if (i > 0) o.push({ v: parseFloat(t.slice(0, i)),
                                        l: t.slice(i + 1) });
                });
                o.sort((a, b) => a.v - b.v);
                return o;
            }
            property int cur: {
                const v = Math.round(page.fx.params[pkey] || 0);
                for (let i = 0; i < opts.length; i++)
                    if (Math.round(opts[i].v) === v) return i;
                return 0;
            }
            width: 116; height: 30; radius: 4
            color: "#12171b"; border.color: "#39434b"
            Text { anchors.centerIn: parent
                   width: parent.width - 10; elide: Text.ElideRight
                   horizontalAlignment: Text.AlignHCenter
                   text: parent.opts.length ? parent.opts[parent.cur].l : "—"
                   color: "#e9e5da"; font.pixelSize: 10 }
            TapHandler {
                margin: 6
                gesturePolicy: TapHandler.ReleaseWithinBounds
                onTapped: {
                    if (!parent.opts.length) return;
                    parent.cur = (parent.cur + 1) % parent.opts.length;
                    page.sendParam(pkey, parent.opts[parent.cur].v);
                }
            }
        }
    }
    Component {
        id: fxKnob
        Knob {
            property real mn: (prng.min !== undefined && isFinite(prng.min))
                              ? prng.min : (pmeta.min !== undefined ? pmeta.min : 0)
            property real mx: {
                const m = (prng.max !== undefined && isFinite(prng.max))
                          ? prng.max : (pmeta.max !== undefined ? pmeta.max : 1);
                return m > mn ? m : mn + 1;
            }
            from: plog ? Math.max(mn, 1e-6) : mn
            to: mx
            unit: pmeta.unit || ""
            logScale: plog
            Component.onCompleted:
                value = Math.max(from, Math.min(to, page.fx.params[pkey] || 0))
            onMoved: (v) => page.sendParam(pkey, pkind === 3 ? Math.round(v) : v)
        }
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
                // effets STÉRÉO : L et R séparés, en entrée et en sortie
                property real vinL: 0
                property real vinR: 0
                property real voutL: 0
                property real voutR: 0
                FrameAnimation {
                    running: page.visible
                    onTriggered: {
                        const dt = Math.min(frameTime, 0.1);
                        const kA = 1 - Math.exp(-dt / 0.030);
                        const kR = 1 - Math.exp(-dt / 0.120);
                        const fxl = mixer.fxLevels, inl = mixer.inLevels;
                        const b = page.bus * 2;
                        const st = (v, t) => v + (t - v) * (t > v ? kA : kR);
                        fxVu.vinL  = st(fxVu.vinL,  fxl.length > b     ? fxl[b]     : 0);
                        fxVu.vinR  = st(fxVu.vinR,  fxl.length > b + 1 ? fxl[b + 1] : 0);
                        fxVu.voutL = st(fxVu.voutL, inl.length > 18 + b ? inl[18 + b] : 0);
                        fxVu.voutR = st(fxVu.voutR, inl.length > 19 + b ? inl[19 + b] : 0);
                    }
                }
                Text { text: "ENTRÉE (SENDS)  L / R"; color: "#5c666e"
                       font.pixelSize: 9; font.letterSpacing: 2 }
                Column {
                    width: parent.width
                    spacing: 2
                    Rectangle {
                        width: parent.width; height: 12; radius: 6; color: "#0b0e11"
                        Rectangle { height: parent.height; radius: 6
                                    width: parent.width * fxVu.vinL; color: "#4cc470" }
                    }
                    Rectangle {
                        width: parent.width; height: 12; radius: 6; color: "#0b0e11"
                        Rectangle { height: parent.height; radius: 6
                                    width: parent.width * fxVu.vinR; color: "#4cc470" }
                    }
                }
                Text { text: "SORTIE (RETOUR)  L / R"; color: "#5c666e"
                       font.pixelSize: 9; font.letterSpacing: 2 }
                Column {
                    width: parent.width
                    spacing: 2
                    Rectangle {
                        width: parent.width; height: 12; radius: 6; color: "#0b0e11"
                        Rectangle { height: parent.height; radius: 6
                                    width: parent.width * fxVu.voutL; color: "#e5a13c" }
                    }
                    Rectangle {
                        width: parent.width; height: 12; radius: 6; color: "#0b0e11"
                        Rectangle { height: parent.height; radius: 6
                                    width: parent.width * fxVu.voutR; color: "#e5a13c" }
                    }
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

            // rack — parité avec le look web : en-tête de rack + cartes
            // de paramètres par GROUPE (V13.4)
            Flickable {
                anchors.fill: parent; anchors.margins: 12
                visible: !page.listOpen
                contentHeight: rackCol.height
                clip: true

                Column {
                    id: rackCol
                    width: parent.width
                    spacing: 12

                    // en-tête de rack : nom du plugin + méta
                    Row {
                        width: parent.width
                        spacing: 12
                        Text {
                            text: page.fx.type === "lv2"
                                  ? String(page.fx.uri || "?").split("/").pop()
                                  : String(page.fx.type || "…").toUpperCase()
                            color: "#e9e5da"; font.pixelSize: 16; font.bold: true
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        Text {
                            text: (page.fx.type === "lv2" ? "LV2" : "NATIF")
                                  + "  ·  BUS FX" + (page.bus + 1)
                            color: "#5c666e"; font.pixelSize: 10
                            font.letterSpacing: 2
                            anchors.verticalCenter: parent.verticalCenter
                        }
                    }

                    // cartes par groupe en GRILLE 3 colonnes (parité web
                    // .fxgrid) : les cartes se posent côte à côte et
                    // s'élargissent par pas de colonne selon leur contenu
                    Flow {
                        width: parent.width
                        spacing: 9
                    Repeater {
                        model: {
                            const keys = page.fx.params
                                         ? Object.keys(page.fx.params) : [];
                            const gs = {}, order = [];
                            keys.forEach(k => {
                                const g = page.grpOf(k);
                                if (!gs[g]) { gs[g] = []; order.push(g); }
                                gs[g].push(k);
                            });
                            order.sort((a, b) =>
                                a === "GÉNÉRAL" ? -1 : b === "GÉNÉRAL" ? 1 : 0);
                            return order.map(g => ({ name: g, keys: gs[g] }));
                        }
                        Rectangle {
                            // largeur en pas de colonne (1/3, 2/3, plein)
                            // selon le nombre de knobs (98 px chacun)
                            property real col: (rackCol.width - 18) / 3
                            width: {
                                const need = modelData.keys.length * 98 + 24;
                                if (need <= col) return col;
                                if (need <= col * 2 + 9) return col * 2 + 9;
                                return rackCol.width;
                            }
                            height: grpFlow.height + 46
                            radius: 8
                            color: "#1b2126"; border.color: "#060809"
                            Text {
                                x: 12; y: 10
                                text: modelData.name
                                color: "#e5a13c"; font.pixelSize: 10
                                font.bold: true; font.letterSpacing: 3
                            }
                            Flow {
                                id: grpFlow
                                x: 12; y: 32
                                width: parent.width - 24
                                spacing: 14
                                Repeater {
                                    model: modelData.keys
                                    Column {
                                        width: kind === 2 ? 120 : 84
                                        spacing: 4
                                        property var meta: (page.fx.meta && page.fx.meta[modelData]) ? page.fx.meta[modelData] : {}
                                        property var rng: (page.fx.ranges && page.fx.ranges[modelData]) ? page.fx.ranges[modelData] : {}
                                        // parité web : 1=toggle, 2=enum (sp),
                                        // 3=entier, bit 0x10 = échelle log
                                        property int kind: (meta.kind || 0) & 0x0f
                                        property bool isLog: ((meta.kind || 0) & 0x10) !== 0
                                        Text {
                                            anchors.horizontalCenter: parent.horizontalCenter
                                            width: parent.width
                                            horizontalAlignment: Text.AlignHCenter
                                            elide: Text.ElideRight
                                            text: (meta.label || modelData)
                                            color: "#5c666e"; font.pixelSize: 9
                                            font.letterSpacing: 2
                                        }
                                        Loader {
                                            anchors.horizontalCenter: parent.horizontalCenter
                                            property var pkey: modelData
                                            property var pmeta: meta
                                            property var prng: rng
                                            property bool plog: isLog
                                            property int pkind: kind
                                            sourceComponent: kind === 1 ? fxToggle
                                                : (kind === 2 && meta.sp) ? fxEnum
                                                : fxKnob
                                        }
                                    }
                                }
                            }
                        }
                    }
                    }

                    Text {
                        visible: !page.fx.params || Object.keys(page.fx.params).length === 0
                        width: parent.width
                        horizontalAlignment: Text.AlignHCenter
                        text: page.fx.type ? "Moteur « " + page.fx.type + " » sans paramètres exposés — choisis un effet dans la liste" : "Chargement…"
                        color: "#5c666e"; font.pixelSize: 12
                    }
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
