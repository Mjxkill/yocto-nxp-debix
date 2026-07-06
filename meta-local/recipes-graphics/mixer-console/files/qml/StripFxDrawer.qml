// V10-N5 — Panneau effets par piste (TAC + DSP) : port natif du drawer web.
// Données : /api/alsa/* et /api/dsp/blob/* de mixer-gui-http (localhost).
import QtQuick
import "stripfx.js" as FX

Rectangle {
    id: drawer
    visible: false
    color: "#12161a"
    border.color: "#39434b"
    radius: 8
    z: 500

    property int chanIdx: 0
    property bool isOut: false
    property var controls: ({})
    property var groups: []
    property string tab: ""
    property var blobs: ({})
    property var bqParams: ({})
    property string status: ""

    function chanName() {
        const i = chanIdx;
        if (!isOut) return i < 8 ? "M" + (i + 1) : (i < 16 ? "U" + (i - 7) : "P" + (i - 15));
        return i < 8 ? "S" + (i + 1) : (i < 16 ? "U" + (i - 7) : "P" + (i - 15));
    }

    function open(idx, out) {
        chanIdx = idx; isOut = out; tab = ""; blobs = {}; status = "Lecture des contrôles ALSA…";
        visible = true;
        groups = [];
        xhr("GET", "/api/alsa/contents", null, function(txt) {
            controls = FX.parseAmixer(txt);
            const n = Object.keys(controls).length;
            status = n < 300 ? ("⚠ " + n + " contrôles (attendu 339)") : "";
            rebuild();
        }, function(err) { status = "⚠ ALSA inaccessible : " + err; });
    }

    function rebuild() {
        var g = FX.groupsFor(chanIdx, isOut, controls);
        /* V12-EXP : onglet GATE natif (mixer-pro) sur les voies IN 0..15 —
         * groupe synthétique sans contrôles ALSA, corps dédié (gatePanel) */
        if (!isOut && chanIdx < 16)
            g.unshift({ title: "GATE", controls: [] });
        groups = g;
        if (groups.length === 0) status = "Aucun effet TAC/DSP sur cette voie";
        tabChanged();  // force le rafraîchissement du corps
    }

    function step(d) {
        const idx = (chanIdx + d + 18) % 18;
        open(idx, isOut);
    }

    function xhr(method, url, body, ok, fail) {
        const q = new XMLHttpRequest();
        let done = false;
        const to = timeoutTimer.createObject(drawer, { interval: 8000 });
        to.triggered.connect(function() { if (!done) { done = true; if (fail) fail("timeout 8 s"); } });
        to.start();
        q.onreadystatechange = function() {
            if (q.readyState !== XMLHttpRequest.DONE || done) return;
            done = true; to.stop(); to.destroy();
            if (q.status === 200) ok(q.responseText);
            else if (fail) fail("HTTP " + q.status);
        };
        q.open(method, "http://127.0.0.1:8080" + url);
        if (body) q.setRequestHeader("Content-Type", "application/json");
        q.send(body || undefined);
    }
    Component { id: timeoutTimer; Timer { repeat: false } }

    function setAlsa(c, value) {
        xhr("POST", "/api/alsa/set",
            JSON.stringify({ numid: c.numid, value: String(value) }),
            function() {}, function() {});
    }

    function activeGroup() {
        for (const g of groups) if (g.title === tab) return g;
        return groups.length ? groups[0] : null;
    }

    function loadBlob(c, cb) {
        if (blobs[c.numid]) { cb(blobs[c.numid]); return; }
        xhr("GET", "/api/dsp/blob/" + c.numid + "/raw", null, function(txt) {
            try {
                const j = JSON.parse(txt);
                if (!j.ok) { cb(null); return; }
                const b = FX.parseBlob(j.hex, c.fullName);
                if (b) {
                    /* autotest round-trip AVANT d'autoriser APPLIQUER */
                    b.selftest = FX.selfTest(j.hex, c.fullName);
                    b.bandTab = 0;
                    b.dirty = false;
                    const bb = blobs; bb[c.numid] = b; blobs = bb;
                }
                cb(b);
            } catch (e) { cb(null); }
        }, function() { cb(null); });
    }

    /* V10-N7 : callback delegate — le texte « non appliqué » est lié au
     * signal blobChanged() DU DELEGATE ; émettre blobsChanged() (map
     * globale) ne le rafraîchissait jamais. On vérifie aussi le ok:true
     * du serveur au lieu de croire le HTTP 200. */
    function applyBlob(numid, done) {
        const b = blobs[numid];
        if (!b || b.selftest !== "OK") return;
        const hex = FX.packBlob(b);
        xhr("POST", "/api/dsp/blob/set",
            JSON.stringify({ numid: numid, hex: hex }),
            function(txt) {
                let ok = false;
                try { ok = JSON.parse(txt).ok === true; } catch (e) {}
                if (ok) { b.dirty = false; status = "✓ appliqué au DSP"; }
                else { status = "⚠ écriture blob refusée"; }
                if (done) done(ok);
            },
            function(e) { status = "⚠ écriture blob : " + e; if (done) done(false); });
    }

    // ================== UI ==================
    Column {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 8

        // entête — ancrages FIXES (V10-N7c : le Row en flux poussait le ✕
        // hors position dès que le statut s'allongeait ; et TapHandler sans
        // gesturePolicy annule le tap au moindre glissement du doigt)
        Item {
            width: parent.width; height: 36
            Rectangle {
                id: prevBtn
                width: 48; height: 36; radius: 4; color: "#1b2126"; border.color: "#39434b"
                Text { anchors.centerIn: parent; text: "‹"; color: "#e5a13c"; font.pixelSize: 17 }
                TapHandler { margin: 6; gesturePolicy: TapHandler.ReleaseWithinBounds; onTapped: drawer.step(-1) }
            }
            Rectangle {
                id: nextBtn
                anchors.left: prevBtn.right; anchors.leftMargin: 8
                width: 48; height: 36; radius: 4; color: "#1b2126"; border.color: "#39434b"
                Text { anchors.centerIn: parent; text: "›"; color: "#e5a13c"; font.pixelSize: 17 }
                TapHandler { margin: 6; gesturePolicy: TapHandler.ReleaseWithinBounds; onTapped: drawer.step(1) }
            }
            Text {
                anchors.left: nextBtn.right; anchors.leftMargin: 12
                anchors.right: closeBtn.left; anchors.rightMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                elide: Text.ElideRight
                text: drawer.chanName() + " · EFFETS PISTE" + (drawer.status ? "   " + drawer.status : "")
                color: "#e5a13c"; font.pixelSize: 14; font.bold: true; font.letterSpacing: 2
            }
            Rectangle {
                id: closeBtn
                anchors.right: parent.right
                width: 64; height: 36; radius: 4
                color: "#2a2214"; border.color: "#e5a13c"
                Text { anchors.centerIn: parent; text: "✕"; color: "#e5a13c"; font.pixelSize: 16; font.bold: true }
                TapHandler { margin: 10; gesturePolicy: TapHandler.ReleaseWithinBounds; onTapped: drawer.visible = false }
            }
        }

        // onglets
        Row {
            spacing: 6
            Repeater {
                model: drawer.groups
                Rectangle {
                    property bool on: (drawer.tab === "" && index === 0) || drawer.tab === modelData.title
                    width: gTxt.width + 24; height: 28; radius: 4
                    color: on ? "#2a2214" : "#1b2126"
                    border.color: on ? "#e5a13c" : "#39434b"
                    Text { id: gTxt; anchors.centerIn: parent; text: modelData.title; color: on ? "#e5a13c" : "#8b959d"; font.pixelSize: 10; font.bold: true }
                    TapHandler { onTapped: drawer.tab = modelData.title }
                }
            }
        }

        // corps
        Flickable {
            width: parent.width
            height: parent.height - 90
            contentHeight: bodyCol.height
            clip: true

            Column {
                id: bodyCol
                width: parent.width
                spacing: 10

                // ============ V12-EXP : panneau GATE (onglet synthétique) ============
                Column {
                    id: gatePanel
                    width: bodyCol.width
                    spacing: 10
                    visible: {
                        const g = drawer.activeGroup();
                        drawer.tab;   // dépendance
                        return g !== null && g.title === "GATE";
                    }
                    // config courante de la voie (poll get_expander)
                    property var exp: null
                    property bool dragging: false

                    onVisibleChanged: if (visible) poll()
                    Timer {
                        interval: 250; repeat: true
                        running: gatePanel.visible && drawer.visible
                        onTriggered: gatePanel.poll()
                    }
                    function poll() {
                        mixer.call({ op: "get_expander" }, function(r) {
                            if (r.ok && drawer.chanIdx < 16 && !gatePanel.dragging)
                                gatePanel.exp = r.channels[drawer.chanIdx];
                        });
                    }
                    function send(field, val) {
                        var m = { op: "set_expander", src: drawer.chanIdx };
                        m[field] = val;
                        mixer.call(m, function() {});
                    }

                    // --- interrupteur ON + GR meter ---
                    Row {
                        spacing: 16
                        Rectangle {
                            width: 120; height: 44; radius: 6
                            property bool on: gatePanel.exp !== null && gatePanel.exp.on === 1
                            color: on ? "#2a2214" : "#1b2126"
                            border.color: on ? "#e5a13c" : "#39434b"
                            border.width: on ? 2 : 1
                            Text {
                                anchors.centerIn: parent
                                text: parent.on ? "GATE ON" : "GATE OFF"
                                color: parent.on ? "#e5a13c" : "#8b959d"
                                font.pixelSize: 12; font.bold: true
                            }
                            TapHandler {
                                gesturePolicy: TapHandler.ReleaseWithinBounds
                                onTapped: gatePanel.send("on", parent.on ? 0 : 1)
                            }
                        }
                        Column {
                            spacing: 4
                            anchors.verticalCenter: parent.verticalCenter
                            Text {
                                text: "RÉDUCTION " + (gatePanel.exp !== null
                                      ? gatePanel.exp.gr_db.toFixed(1) + " dB" : "—")
                                color: "#8b959d"; font.pixelSize: 10; font.letterSpacing: 1
                            }
                            Rectangle {
                                width: 320; height: 12; radius: 6; color: "#0b0e11"
                                Rectangle {
                                    height: parent.height; radius: 6
                                    anchors.right: parent.right
                                    width: {
                                        if (gatePanel.exp === null) return 0;
                                        var rng = Math.max(1, gatePanel.exp.range_db);
                                        var f = Math.min(1, -gatePanel.exp.gr_db / rng);
                                        return parent.width * Math.max(0, f);
                                    }
                                    color: "#e05545"
                                }
                            }
                        }
                    }

                    // --- sliders paramètres ---
                    Repeater {
                        model: [
                            { key: "threshold_db", label: "SEUIL",   min: -80,  max: 0,    unit: "dB", dec: 1 },
                            { key: "ratio",        label: "RATIO",   min: 1,    max: 20,   unit: ":1", dec: 1 },
                            { key: "attack_ms",    label: "ATTACK",  min: 0.5,  max: 100,  unit: "ms", dec: 1 },
                            { key: "release_ms",   label: "RELEASE", min: 5,    max: 1000, unit: "ms", dec: 0 },
                            { key: "range_db",     label: "RANGE",   min: 0,    max: 80,   unit: "dB", dec: 0 },
                            { key: "hold_ms",      label: "HOLD",    min: 0,    max: 500,  unit: "ms", dec: 0 }
                        ]
                        Row {
                            spacing: 14
                            height: 52
                            property real cur: gatePanel.exp !== null
                                               ? gatePanel.exp[modelData.key] : modelData.min
                            Text {
                                width: 90
                                anchors.verticalCenter: parent.verticalCenter
                                text: modelData.label
                                color: "#8b959d"; font.pixelSize: 11; font.bold: true
                                font.letterSpacing: 1
                            }
                            Rectangle {
                                id: track
                                width: bodyCol.width - 90 - 110 - 2*14
                                height: 26; radius: 13
                                anchors.verticalCenter: parent.verticalCenter
                                color: "#0b0e11"
                                Rectangle {
                                    height: parent.height; radius: 13
                                    width: parent.width *
                                           Math.max(0, Math.min(1,
                                               (parent.parent.cur - modelData.min)
                                               / (modelData.max - modelData.min)))
                                    color: "#39434b"
                                    Rectangle { width: 4; height: parent.height
                                                anchors.right: parent.right
                                                color: "#e5a13c" }
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    preventStealing: true
                                    onPressed: gatePanel.dragging = true
                                    onReleased: gatePanel.dragging = false
                                    onCanceled: gatePanel.dragging = false
                                    onPositionChanged: (m) => {
                                        if (!pressed || gatePanel.exp === null) return;
                                        var f = Math.max(0, Math.min(1, m.x / width));
                                        var v = modelData.min + f * (modelData.max - modelData.min);
                                        v = Number(v.toFixed(modelData.dec));
                                        var e = gatePanel.exp; e[modelData.key] = v;
                                        gatePanel.exp = e; gatePanel.expChanged();
                                        gatePanel.send(modelData.key, v);
                                    }
                                    onClicked: (m) => {
                                        if (gatePanel.exp === null) return;
                                        var f = Math.max(0, Math.min(1, m.x / width));
                                        var v = modelData.min + f * (modelData.max - modelData.min);
                                        v = Number(v.toFixed(modelData.dec));
                                        gatePanel.send(modelData.key, v);
                                    }
                                }
                            }
                            Text {
                                width: 110
                                anchors.verticalCenter: parent.verticalCenter
                                text: parent.cur.toFixed(modelData.dec) + " " + modelData.unit
                                color: "#e9e5da"; font.pixelSize: 13
                                font.family: "monospace"; font.bold: true
                            }
                        }
                    }
                    Text {
                        text: "Gate pré-fader : agit sur les départs FX, le master, le looper et l'automix"
                        color: "#5c666e"; font.pixelSize: 10
                    }
                }

                Repeater {
                    model: {
                        const g = drawer.activeGroup();
                        drawer.tab;   // dépendance
                        return g ? g.controls : [];
                    }
                    delegate: Loader {
                        width: bodyCol.width
                        property var ctl: modelData
                        sourceComponent: {
                            if (ctl.type === "BYTES" && FX.isDrcBlob(ctl.fullName)) return drcCard;
                            if (ctl.type === "BYTES" && /BQ\d+ Coefs$/.test(ctl.fullName)) return bqRow;
                            return plainRow;
                        }
                    }
                }
            }
        }
    }

    // ---- contrôle simple : INTEGER→knob dB, BOOLEAN→switch, ENUM→cycle ----
    Component {
        id: plainRow
        Row {
            spacing: 16
            height: 84
            Text {
                width: 260
                anchors.verticalCenter: parent.verticalCenter
                text: ctl.short
                color: "#8b959d"; font.pixelSize: 12
                elide: Text.ElideRight
            }
            Loader {
                anchors.verticalCenter: parent.verticalCenter
                sourceComponent: ctl.type === "INTEGER" ? knobComp
                               : ctl.type === "BOOLEAN" ? switchComp : enumComp
            }
            property Component knobComp: Component {
                Knob {
                    property bool hasDb: ctl.dbStep !== undefined && ctl.dbMin !== undefined
                    from: hasDb ? ctl.dbMin : (ctl.min || 0)
                    to: hasDb ? ctl.dbMin + ((ctl.max || 0) - (ctl.min || 0)) * ctl.dbStep : (ctl.max || 100)
                    unit: hasDb ? "dB" : ""
                    Component.onCompleted: value = hasDb
                        ? ctl.dbMin + (FX.intVal(ctl) - (ctl.min || 0)) * ctl.dbStep
                        : FX.intVal(ctl)
                    onMoved: (v) => drawer.setAlsa(ctl, hasDb
                        ? Math.round((v - ctl.dbMin) / ctl.dbStep) + (ctl.min || 0)
                        : Math.round(v))
                }
            }
            property Component switchComp: Component {
                Rectangle {
                    property bool on: FX.isOn(ctl)
                    width: 74; height: 30; radius: 15
                    color: on ? "#2a2214" : "#1b2126"
                    border.color: on ? "#e5a13c" : "#39434b"
                    Text { anchors.centerIn: parent; text: parent.on ? "ON" : "OFF"; color: parent.on ? "#e5a13c" : "#5c666e"; font.pixelSize: 11; font.bold: true }
                    TapHandler { onTapped: { parent.on = !parent.on; drawer.setAlsa(ctl, parent.on ? "on" : "off"); } }
                }
            }
            property Component enumComp: Component {
                Rectangle {
                    property var items: ctl.items || []
                    property int cur: {
                        const v = String(ctl.value || "").replace(/'/g, "");
                        const byName = items.indexOf(v);
                        if (byName >= 0) return byName;
                        const n = parseInt(v, 10);
                        return isNaN(n) ? 0 : Math.min(n, items.length - 1);
                    }
                    width: 210; height: 30; radius: 4
                    color: "#1b2126"; border.color: "#39434b"
                    Text { anchors.centerIn: parent; text: parent.items[parent.cur] || "—"; color: "#e9e5da"; font.pixelSize: 11 }
                    TapHandler {
                        onTapped: {
                            parent.cur = (parent.cur + 1) % Math.max(1, parent.items.length);
                            drawer.setAlsa(ctl, parent.items[parent.cur]);
                        }
                    }
                }
            }
        }
    }

    // ---- biquad RBJ ----
    Component {
        id: bqRow
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
    }

    // ---- carte DRC / MULTIBAND ----
    Component {
        id: drcCard
        Rectangle {
            width: bodyCol.width
            height: drcCol.height + 24
            color: "#171c21"; radius: 8; border.color: "#060809"
            property var blob: null
            Component.onCompleted: drawer.loadBlob(ctl, function(b) { blob = b; })

            Column {
                id: drcCol
                x: 12; y: 12
                width: parent.width - 24
                spacing: 8

                Row {
                    spacing: 12
                    Text {
                        text: ctl.short + (blob && blob.selftest !== "OK"
                              ? "   ⚠ codec: " + blob.selftest : "")
                        color: blob && blob.selftest !== "OK" ? "#e05545" : "#e5a13c"
                        font.pixelSize: 12; font.bold: true; font.letterSpacing: 2
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    Text {
                        visible: blob === null
                        text: "chargement du blob…"
                        color: "#5c666e"; font.pixelSize: 11
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }

                // bandes (multiband)
                Row {
                    visible: blob && blob.kind === "multiband"
                    spacing: 6
                    Repeater {
                        model: blob && blob.kind === "multiband" ? blob.num_bands : 0
                        Rectangle {
                            width: 84; height: 26; radius: 4
                            color: blob.bandTab === index ? "#2a2214" : "#1b2126"
                            border.color: blob.bandTab === index ? "#e5a13c" : "#39434b"
                            Text { anchors.centerIn: parent; text: "BANDE " + (index + 1); color: blob.bandTab === index ? "#e5a13c" : "#8b959d"; font.pixelSize: 10; font.bold: true }
                            TapHandler { onTapped: { blob.bandTab = index; blobChanged(); } }
                        }
                    }
                }

                // crossovers — V10-FX : PAR CANAL (blob V3). Le knob édite
                // le canal courant du drawer ; le 1er réglage fait passer
                // le blob en layout V3 (xover_per_ch).
                Row {
                    visible: blob && blob.kind === "multiband"
                    spacing: 14
                    Repeater {
                        model: !blob || blob.kind !== "multiband" ? []
                               : (blob.num_bands === 2 ? ["low"]
                                  : blob.num_bands === 3 ? ["low", "high"] : ["low", "mid", "high"])
                        Knob {
                            from: 20; to: 20000; unit: "Hz"
                            Component.onCompleted: value =
                                blob.crossover_fcs_ch[drawer.chanIdx % 8][modelData]
                            onMoved: (v) => {
                                const ch = drawer.chanIdx % 8;
                                blob.crossover_fcs_ch[ch][modelData] =
                                    Math.max(20, Math.min(Math.round(v), 20000));
                                blob.xover_per_ch = true;
                                blob.dirty = true; blobChanged();
                            }
                            Text {
                                anchors.top: parent.top; anchors.topMargin: -12
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: "XOVER " + modelData.toUpperCase() + " · CANAL"
                                color: "#8b959d"; font.pixelSize: 9
                            }
                        }
                    }
                }

                // ACTIF + knobs du canal courant
                Row {
                    visible: blob !== null
                    spacing: 14
                    Rectangle {
                        property var pp: {
                            if (!blob) return null;
                            const ch = drawer.chanIdx % 8;
                            if (blob.kind === "multiband") {
                                const row = blob.drc[Math.min(blob.bandTab, blob.drc.length - 1)];
                                return row[Math.min(ch, row.length - 1)];
                            }
                            return blob.params[Math.min(ch, blob.params.length - 1)];
                        }
                        anchors.verticalCenter: parent.verticalCenter
                        width: 74; height: 30; radius: 15
                        color: pp && pp.enabled === 1 ? "#2a2214" : "#1b2126"
                        border.color: pp && pp.enabled === 1 ? "#e5a13c" : "#39434b"
                        Text { anchors.centerIn: parent; text: parent.pp && parent.pp.enabled === 1 ? "ACTIF" : "OFF"; color: parent.pp && parent.pp.enabled === 1 ? "#e5a13c" : "#5c666e"; font.pixelSize: 10; font.bold: true }
                        TapHandler {
                            onTapped: {
                                const p = parent.pp;
                                if (!p) return;
                                const v = p.enabled === 1 ? 0 : 1;
                                // V10-FX : sur multiband, OFF/ACTIF = le
                                // canal courant sur TOUTES les bandes
                                if (blob.kind === "multiband") {
                                    const ch = drawer.chanIdx % 8;
                                    for (const row of blob.drc)
                                        row[Math.min(ch, row.length - 1)].enabled = v;
                                } else {
                                    p.enabled = v;
                                }
                                blob.dirty = true; blobChanged();
                            }
                        }
                    }
                    Repeater {
                        model: blob ? FX.DRC_SLIDERS : []
                        Knob {
                            property var pp: {
                                const ch = drawer.chanIdx % 8;
                                if (blob.kind === "multiband") {
                                    const row = blob.drc[Math.min(blob.bandTab, blob.drc.length - 1)];
                                    return row[Math.min(ch, row.length - 1)];
                                }
                                return blob.params[Math.min(ch, blob.params.length - 1)];
                            }
                            from: modelData.min; to: modelData.max; unit: modelData.unit
                            Component.onCompleted: value = pp ? (pp[modelData.key] || 0) : 0
                            onMoved: (v) => { if (pp) { pp[modelData.key] = v; blob.dirty = true; } }
                            Text {
                                anchors.top: parent.top; anchors.topMargin: -12
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: modelData.label.toUpperCase()
                                color: "#8b959d"; font.pixelSize: 9
                            }
                        }
                    }
                }

                Row {
                    visible: blob !== null
                    spacing: 10
                    Rectangle {
                        width: 170; height: 34; radius: 5
                        property bool enabled2: blob && blob.selftest === "OK"
                        color: enabled2 ? "#e5a13c" : "#1b2126"
                        border.color: enabled2 ? "#ffcf7e" : "#39434b"
                        opacity: enabled2 ? 1 : 0.5
                        Text { anchors.centerIn: parent; text: "APPLIQUER AU DSP"; color: parent.enabled2 ? "#1d1204" : "#5c666e"; font.pixelSize: 11; font.bold: true }
                        TapHandler { onTapped: if (parent.enabled2) drawer.applyBlob(ctl.numid, function(ok) { if (ok) blobChanged(); }) }
                    }
                    Text {
                        visible: blob && blob.dirty === true
                        anchors.verticalCenter: parent.verticalCenter
                        text: "● modifié — non appliqué"
                        color: "#e5a13c"; font.pixelSize: 10
                    }
                }
            }
        }
    }
}
