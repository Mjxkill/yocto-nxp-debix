// V13-BANDMIX — Page AUTO MIX : assistant de sonorisation de groupe.
// 1) rôle par tranche  2) MESURER chaque source (12 s)  3) CALCULER LE MIX
// (gains/gates/comps/faders posés par rôle)  4) VERROUILLER L'ÉQUILIBRE
// (référence 30 s, retouches incluses)  5) LIVE : keeper lent ±3 dB.
import QtQuick

Item {
    id: page
    signal duganSettings()   // V13 : ouvre le panneau réglages Dugan (main.qml)

    readonly property var roleKeys: ["off","lead","choir","kick","snare",
                                     "drums","bass","guitar","keys","line"]
    readonly property var roleNames: ["—","VOIX LEAD","CHŒURS","GR. CAISSE",
                                      "C. CLAIRE","BATTERIE","BASSE",
                                      "GUITARE","CLAVIER","LIGNE"]
    function stripName(i) { return i < 8 ? "M" + (i + 1) : "U" + (i - 7); }

    property var st: null      // bandmix_status
    property var vf: null      // get_vfocus (place à la voix)
    property var meq: null     // master_eq (EQ mastering + makeup/LUFS)
    property var at: null      // automix_tune (gel/mémoire/marge — réglable live)
    property var bal: null     // set_balance (balance auto musique/voix)
    property var vsp: null     // set_vspatial (widener voix)
    property bool vfDrag: false
    property bool atDrag: false
    property bool meqDrag: false
    property bool balDrag: false
    property bool vspDrag: false
    onVisibleChanged: if (visible) refresh()
    Timer { interval: 500; running: page.visible; repeat: true
            onTriggered: page.refresh() }
    Timer { interval: 250; running: page.visible; repeat: true
            onTriggered: mixer.call({ op: "get_vfocus" }, function(r) {
                if (r.ok && !page.vfDrag) page.vf = r; }) }
    function refresh() {
        mixer.call({ op: "bandmix_status" }, function(r) {
            if (r.ok) page.st = r;
        });
        if (!page.meqDrag)
            mixer.call({ op: "master_eq" }, function(r) {
                if (r.ok && !page.meqDrag) page.meq = r;
            });
        if (!page.atDrag)
            mixer.call({ op: "automix_tune" }, function(r) {
                if (r.ok && !page.atDrag) page.at = r;
            });
        if (!page.balDrag)
            mixer.call({ op: "set_balance" }, function(r) {
                if (r.ok && !page.balDrag) page.bal = r;
            });
        if (!page.vspDrag)
            mixer.call({ op: "set_vspatial" }, function(r) {
                if (r.ok && !page.vspDrag) page.vsp = r;
            });
    }
    function setRole(src, dir) {
        if (!st) return;
        var cur = roleKeys.indexOf(st.chans[src].role);
        var nx = (cur + dir + roleKeys.length) % roleKeys.length;
        mixer.call({ op: "bandmix_role", src: src, role: roleKeys[nx] },
                   function() { page.refresh(); });
    }

    Column {
        anchors.fill: parent
        anchors.margins: 14
        spacing: 8

        // ================= BANDEAU PILOTAGE =================
        Rectangle {
            width: parent.width; height: 74
            color: "#171c21"; radius: 8; border.color: "#060809"
            Item {
                anchors.fill: parent; anchors.margins: 12
                Column {
                    spacing: 3
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    Text { text: "AUTO MIX"; color: "#e5a13c"; font.pixelSize: 13
                           font.bold: true; font.letterSpacing: 3 }
                    Text {
                        text: {
                            if (!page.st) return "…";
                            if (page.st.measuring >= 0)
                                return "mesure " + page.stripName(page.st.measuring)
                                       + "  " + page.st.meas_elapsed + "/12 s";
                            if (page.st.locking) return "capture de l'équilibre… (30 s)";
                            if (page.st.live) return "LIVE — équilibre tenu";
                            return page.st.ref_valid ? "équilibre verrouillé, LIVE prêt"
                                                     : "assigne les rôles puis mesure";
                        }
                        color: "#8b959d"; font.pixelSize: 10
                    }
                }
                Row {
                id: bmxBtns
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                spacing: 12
                Rectangle {
                    width: 130; height: 46; radius: 6
                    anchors.verticalCenter: parent.verticalCenter
                    color: "#2a2214"; border.color: "#e5a13c"
                    Text { anchors.centerIn: parent; text: "CALCULER\nLE MIX"
                           horizontalAlignment: Text.AlignHCenter
                           color: "#e5a13c"; font.pixelSize: 11; font.bold: true }
                    TapHandler {
                        gesturePolicy: TapHandler.ReleaseWithinBounds
                        onTapped: mixer.call({ op: "bandmix_calc" },
                                             function() { page.refresh(); })
                    }
                }
                Rectangle {
                    width: 130; height: 46; radius: 6
                    anchors.verticalCenter: parent.verticalCenter
                    color: page.st && page.st.locking ? "#2a2c10" : "#1b2126"
                    border.color: "#8b959d"
                    Text { anchors.centerIn: parent; text: "VERROUILLER\nL'ÉQUILIBRE"
                           horizontalAlignment: Text.AlignHCenter
                           color: "#c9c4b8"; font.pixelSize: 11; font.bold: true }
                    TapHandler {
                        gesturePolicy: TapHandler.ReleaseWithinBounds
                        onTapped: mixer.call({ op: "bandmix_lock" },
                                             function() { page.refresh(); })
                    }
                }
                // V13.5 : AUTOMIX LIVE — un seul bouton, aucun réglage,
                // continu. Équilibre auto référencé sur la voix.
                Rectangle {
                    width: 150; height: 46; radius: 6
                    anchors.verticalCenter: parent.verticalCenter
                    property bool on: page.st !== null && page.st.autolive === 1
                    color: on ? "#142a19" : "#1b2126"
                    border.color: on ? "#4cc470" : "#4cc470"
                    border.width: on ? 2 : 1
                    Text { anchors.centerIn: parent
                           text: parent.on ? "● AUTOMIX LIVE" : "AUTOMIX LIVE"
                           color: parent.on ? "#4cc470" : "#8b959d"
                           font.pixelSize: 12; font.bold: true }
                    TapHandler {
                        gesturePolicy: TapHandler.ReleaseWithinBounds
                        onTapped: mixer.call({ op: "bandmix_autolive",
                                               on: parent.on ? 0 : 1 },
                                             function() { page.refresh(); })
                    }
                }
                Rectangle {
                    width: 96; height: 46; radius: 6
                    anchors.verticalCenter: parent.verticalCenter
                    property bool on: page.st !== null && page.st.live === 1
                    color: on ? "#142a19" : "#1b2126"
                    border.color: on ? "#4cc470" : "#39434b"
                    border.width: on ? 2 : 1
                    Text { anchors.centerIn: parent
                           text: parent.on ? "LIVE ON" : "LIVE OFF"
                           color: parent.on ? "#4cc470" : "#8b959d"
                           font.pixelSize: 11; font.bold: true }
                    TapHandler {
                        gesturePolicy: TapHandler.ReleaseWithinBounds
                        onTapped: mixer.call({ op: "bandmix_live",
                                               on: parent.on ? 0 : 1 },
                                             function() { page.refresh(); })
                    }
                }
                // V13 : le Dugan (parole) vit ici désormais — retiré du master
                Rectangle {
                    width: 96; height: 46; radius: 6
                    anchors.verticalCenter: parent.verticalCenter
                    color: mixer.amxOn ? "#2a2214" : "#1b2126"
                    border.color: mixer.amxOn ? "#e5a13c" : "#39434b"
                    border.width: mixer.amxOn ? 2 : 1
                    Text { anchors.centerIn: parent
                           text: mixer.amxOn ? "DUGAN ON" : "DUGAN OFF"
                           color: mixer.amxOn ? "#e5a13c" : "#8b959d"
                           font.pixelSize: 11; font.bold: true }
                    TapHandler {
                        gesturePolicy: TapHandler.ReleaseWithinBounds
                        onTapped: mixer.call({ op: "set_automix_cfg",
                                               on: mixer.amxOn ? 0 : 1 },
                                             function() {})
                    }
                }
                Rectangle {
                    width: 40; height: 46; radius: 6
                    anchors.verticalCenter: parent.verticalCenter
                    color: "#1b2126"; border.color: "#39434b"
                    Text { anchors.centerIn: parent; text: "⚙"
                           color: "#8b959d"; font.pixelSize: 15 }
                    TapHandler {
                        gesturePolicy: TapHandler.ReleaseWithinBounds
                        onTapped: page.duganSettings()
                    }
                }
                }
            }
        }

        // === CORPS : 16 voies (gauche) · analyse + réglages (colonne droite) ===
        Item {
            width: parent.width
            height: parent.height - 74 - 26 - 2*8

            // ===================== COLONNE DROITE (scrollable) =====================
            Flickable {
                width: 300
                anchors.right: parent.right
                anchors.top: parent.top
                height: parent.height
                contentHeight: bmxRight.height
                clip: true
            Column {
                id: bmxRight
                width: 300
                spacing: 8

            // ========= V13-VFOCUS : « PLACE À LA VOIX » (unmasking) =========
            Rectangle {
                width: parent.width; height: 76
                color: "#171c21"; radius: 8; border.color: "#060809"
                Column {
                    anchors.fill: parent; anchors.margins: 10; spacing: 6
                    Row {
                        width: parent.width; spacing: 8
                        Rectangle {
                            width: 168; height: 30; radius: 5
                            property bool on: page.vf !== null && page.vf.on === 1
                            color: on ? "#2a2214" : "#1b2126"
                            border.color: on ? "#e5a13c" : "#39434b"
                            border.width: on ? 2 : 1
                            Text { anchors.centerIn: parent; text: "PLACE À LA VOIX"
                                   color: parent.on ? "#e5a13c" : "#8b959d"
                                   font.pixelSize: 10; font.bold: true; font.letterSpacing: 2 }
                            TapHandler {
                                gesturePolicy: TapHandler.ReleaseWithinBounds
                                onTapped: mixer.call({ op: "set_vfocus",
                                                       on: parent.on ? 0 : 1 }, function() {})
                            }
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: page.vf && page.vf.active === 1 ? "♪ voix" : ""
                            color: "#4cc470"; font.pixelSize: 9
                        }
                    }
                    Row {
                        width: parent.width; spacing: 8
                        Text { text: "DOSE " + (page.vf ? page.vf.amount : 50)
                               color: "#5c666e"; font.pixelSize: 8
                               anchors.verticalCenter: parent.verticalCenter }
                        Rectangle {
                            width: parent.width - 130; height: 18; radius: 9; color: "#0b0e11"
                            anchors.verticalCenter: parent.verticalCenter
                            Rectangle {
                                height: parent.height; radius: 9
                                width: parent.width * (page.vf ? page.vf.amount : 50) / 100
                                color: "#39434b"
                                Rectangle { width: 4; height: parent.height
                                            anchors.right: parent.right; color: "#e5a13c" }
                            }
                            MouseArea {
                                anchors.fill: parent
                                preventStealing: true
                                onPressed: page.vfDrag = true
                                onReleased: page.vfDrag = false
                                onCanceled: page.vfDrag = false
                                onPositionChanged: (m) => {
                                    if (!pressed) return;
                                    var v = Math.round(Math.max(0, Math.min(1, m.x / width)) * 100);
                                    var d = page.vf; if (d) { d.amount = v; page.vf = d; }
                                    mixer.call({ op: "set_vfocus", amount: v }, function() {});
                                }
                            }
                        }
                        Row {
                            spacing: 3
                            anchors.verticalCenter: parent.verticalCenter
                            Repeater {
                                model: 5
                                Rectangle {
                                    width: 9; height: 18; radius: 2; color: "#0b0e11"
                                    Rectangle {
                                        anchors.bottom: parent.bottom
                                        width: parent.width; radius: 2
                                        height: parent.height * Math.min(1,
                                            (page.vf && page.vf.cuts_db ? page.vf.cuts_db[index] : 0) /
                                            (page.vf ? Math.max(1, page.vf.max_cut_db) : 4.5))
                                        color: "#e8b84b"
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // ============ EQ MASTER (mastering) + makeup/LUFS live ============
            Rectangle {
                width: parent.width; height: 158; radius: 8; color: "#171c21"
                Column {
                    anchors.fill: parent; anchors.margins: 10; spacing: 5
                    Row {
                        width: parent.width
                        Text { text: "EQ MASTER"; color: "#e0a030"
                               font.pixelSize: 9; font.letterSpacing: 2
                               anchors.verticalCenter: parent.verticalCenter }
                        Item { width: parent.width - 200; height: 1 }
                        Column {
                            Text { anchors.right: parent.right
                                   text: page.meq ? ((page.meq.makeup_db>0?"+":"")
                                         + page.meq.makeup_db.toFixed(1) + " dB") : "—"
                                   color: "#4cc470"; font.pixelSize: 15; font.bold: true }
                            Text { anchors.right: parent.right
                                   text: page.meq ? ("LUFS " + page.meq.lufs.toFixed(1)) : ""
                                   color: "#7a848c"; font.pixelSize: 8 }
                        }
                    }
                    Canvas {
                        width: parent.width; height: 46
                        property var p: page.meq
                        onPChanged: requestPaint()
                        onPaint: {
                            var ctx = getContext("2d"), W = width, H = height,
                                MID = H/2, SC = H/2/12;
                            ctx.clearRect(0, 0, W, H);
                            ctx.strokeStyle = "#242c34"; ctx.beginPath();
                            ctx.moveTo(0, MID); ctx.lineTo(W, MID); ctx.stroke();
                            if (!p) return;
                            function sh(f, fc, g, hi) { var r = f/fc;
                                return g * (hi ? r/(1+r) : 1/(1+r)); }
                            function bell(f, fc, g, q) { var bw = Math.max(1, fc/q),
                                d = f-fc; return g / (1 + Math.pow(d/(bw/2), 2)); }
                            ctx.strokeStyle = "#4cc470"; ctx.lineWidth = 1.6;
                            ctx.beginPath();
                            for (var x = 0; x < W; x++) {
                                var f = 30 * Math.pow(20000/30, x/W);
                                var db = sh(f, p.low_hz, p.low_db, false)
                                       + bell(f, p.mid_hz, p.mid_db, p.mid_q)
                                       + sh(f, p.air_hz, p.air_db, true);
                                var y = Math.max(1, Math.min(H-1, MID - db*SC));
                                if (x) ctx.lineTo(x, y); else ctx.moveTo(x, y);
                            }
                            ctx.stroke();
                        }
                    }
                    // V13.9 : SUB / MÉD / AIR réglables au doigt (parité web)
                    Row {
                        width: parent.width; spacing: 8
                        Repeater {
                            // [libellé, clé, min, max]
                            model: [["SUB", "low_db", -6, 6],
                                    ["MÉD", "mid_db", -9, 4],
                                    ["AIR", "air_db", -4, 9]]
                            Column {
                                width: (parent.width - 16) / 3
                                spacing: 3
                                property real vmin: modelData[2]
                                property real vmax: modelData[3]
                                property real val: page.meq && page.meq[modelData[1]] !== undefined
                                                   ? page.meq[modelData[1]] : 0
                                Text {
                                    text: modelData[0] + " "
                                          + (parent.val >= 0 ? "+" : "")
                                          + Number(parent.val).toFixed(1)
                                    color: "#c9c4b8"; font.pixelSize: 8
                                }
                                Rectangle {
                                    width: parent.width; height: 18; radius: 9; color: "#0b0e11"
                                    Rectangle {
                                        height: parent.height; radius: 9
                                        width: parent.width * Math.max(0, Math.min(1,
                                            (parent.parent.val - parent.parent.vmin)
                                            / (parent.parent.vmax - parent.parent.vmin)))
                                        color: "#39434b"
                                        Rectangle { width: 4; height: parent.height
                                                    anchors.right: parent.right
                                                    color: "#e5a13c" }
                                    }
                                    MouseArea {
                                        anchors.fill: parent
                                        preventStealing: true
                                        property string key: modelData[1]
                                        property real vmin: modelData[2]
                                        property real vmax: modelData[3]
                                        onPressed: page.meqDrag = true
                                        onReleased: page.meqDrag = false
                                        onCanceled: page.meqDrag = false
                                        onPositionChanged: (m) => {
                                            if (!pressed) return;
                                            var frac = Math.max(0, Math.min(1, m.x / width));
                                            var v = Math.round((vmin + frac * (vmax - vmin)) * 2) / 2;
                                            var d = page.meq || {}; d[key] = v; page.meq = d;
                                            mixer.call({ op: "master_eq", [key]: v },
                                                       function() {});
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // =========== FFT MASTER (spectre sortie + enveloppe NPU) ===========
            SpectrumView {
                width: parent.width; height: 150
            }

            // ======= RÉGLAGES AUTOMIX (tout réglable live — mandat R&D) =======
            Rectangle {
                width: parent.width; height: 134; radius: 8; color: "#171c21"
                Column {
                    anchors.fill: parent; anchors.margins: 10; spacing: 6
                    Text { text: "RÉGLAGES AUTOMIX";
                           color: "#e0a030"; font.pixelSize: 9; font.letterSpacing: 2 }
                    Repeater {
                        // [libellé, clé, min, max, préfixe signe, décimales, suffixe]
                        model: [["GEL SILENCE",  "freeze_db",   3,  40, "−", 0, " dB"],
                                ["MÉMOIRE CRÊTE", "risk_decay",  0,   2, "",  2, " dB/s"],
                                ["MARGE CRÊTE",   "risk_margin", 0,  12, "+", 1, " dB"],
                                ["GATE VOIX",     "gate_db",     3,  30, "−", 0, " dB"]]
                        Row {
                            width: parent.width; spacing: 8
                            property real vmin: modelData[2]
                            property real vmax: modelData[3]
                            property real val: page.at && page.at[modelData[1]] !== undefined
                                               ? page.at[modelData[1]] : vmin
                            Text {
                                width: 128
                                anchors.verticalCenter: parent.verticalCenter
                                text: modelData[0] + "  " + modelData[4]
                                      + Number(parent.val).toFixed(modelData[5]) + modelData[6]
                                color: "#c9c4b8"; font.pixelSize: 8
                            }
                            Rectangle {
                                width: parent.width - 136; height: 16; radius: 8; color: "#0b0e11"
                                anchors.verticalCenter: parent.verticalCenter
                                Rectangle {
                                    height: parent.height; radius: 8
                                    width: parent.width * Math.max(0, Math.min(1,
                                        (parent.parent.val - parent.parent.vmin)
                                        / (parent.parent.vmax - parent.parent.vmin)))
                                    color: "#39434b"
                                    Rectangle { width: 4; height: parent.height
                                                anchors.right: parent.right; color: "#e5a13c" }
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    preventStealing: true
                                    property string key: modelData[1]
                                    property real vmin: modelData[2]
                                    property real vmax: modelData[3]
                                    onPressed: page.atDrag = true
                                    onReleased: page.atDrag = false
                                    onCanceled: page.atDrag = false
                                    onPositionChanged: (m) => {
                                        if (!pressed) return;
                                        var frac = Math.max(0, Math.min(1, m.x / width));
                                        var v = vmin + frac * (vmax - vmin);
                                        var d = page.at || {}; d[key] = v; page.at = d;
                                        mixer.call({ op: "automix_tune", [key]: v }, function() {});
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // ======= V13.9 : BALANCE AUTO musique/voix (quadrants + gel) =======
            Rectangle {
                width: parent.width; height: 144; radius: 8; color: "#171c21"
                Column {
                    anchors.fill: parent; anchors.margins: 10; spacing: 6
                    Row {
                        width: parent.width
                        Text { text: "BALANCE AUTO"; color: "#e0a030"
                               font.pixelSize: 9; font.letterSpacing: 2
                               anchors.verticalCenter: parent.verticalCenter }
                        Item { width: parent.width - 150; height: 1 }
                        Rectangle {
                            width: 56; height: 20; radius: 5
                            property bool on: page.bal !== null && page.bal.on === 1
                            color: on ? "#142a19" : "#1b2126"
                            border.color: on ? "#4cc470" : "#39434b"
                            Text { anchors.centerIn: parent
                                   text: parent.on ? "● ON" : "OFF"
                                   color: parent.on ? "#4cc470" : "#8b959d"
                                   font.pixelSize: 9; font.bold: true }
                            TapHandler {
                                gesturePolicy: TapHandler.ReleaseWithinBounds
                                onTapped: mixer.call({ op: "set_balance",
                                                       on: parent.on ? 0 : 1 },
                                                     function(r) { if (r.ok) page.bal = r; })
                            }
                        }
                    }
                    Repeater {
                        // [libellé, clé, min, max, décimales, suffixe]
                        model: [["LUFS CIBLE",      "lufs_tgt", -24, -8, 1, ""],
                                ["VOIX / MUSIQUE",   "e_tgt",    -6, 12, 1, " dB"],
                                ["CHŒURS / MUSIQUE", "c_tgt",    -6, 12, 1, " dB"]]
                        Row {
                            width: parent.width; spacing: 8
                            property real vmin: modelData[2]
                            property real vmax: modelData[3]
                            property real val: page.bal && page.bal[modelData[1]] !== undefined
                                               ? page.bal[modelData[1]] : vmin
                            Text {
                                width: 128
                                anchors.verticalCenter: parent.verticalCenter
                                text: modelData[0] + "  "
                                      + (modelData[1] !== "lufs_tgt" && parent.val >= 0 ? "+" : "")
                                      + Number(parent.val).toFixed(modelData[4]) + modelData[5]
                                color: "#c9c4b8"; font.pixelSize: 8
                            }
                            Rectangle {
                                width: parent.width - 136; height: 16; radius: 8; color: "#0b0e11"
                                anchors.verticalCenter: parent.verticalCenter
                                Rectangle {
                                    height: parent.height; radius: 8
                                    width: parent.width * Math.max(0, Math.min(1,
                                        (parent.parent.val - parent.parent.vmin)
                                        / (parent.parent.vmax - parent.parent.vmin)))
                                    color: "#39434b"
                                    Rectangle { width: 4; height: parent.height
                                                anchors.right: parent.right; color: "#e5a13c" }
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    preventStealing: true
                                    property string key: modelData[1]
                                    property real vmin: modelData[2]
                                    property real vmax: modelData[3]
                                    onPressed: page.balDrag = true
                                    onReleased: page.balDrag = false
                                    onCanceled: page.balDrag = false
                                    onPositionChanged: (m) => {
                                        if (!pressed) return;
                                        var frac = Math.max(0, Math.min(1, m.x / width));
                                        var v = Math.round((vmin + frac * (vmax - vmin)) * 2) / 2;
                                        var d = page.bal || {}; d[key] = v; page.bal = d;
                                        mixer.call({ op: "set_balance", [key]: v }, function() {});
                                    }
                                }
                            }
                        }
                    }
                    Text {
                        width: parent.width; color: "#7a848c"; font.pixelSize: 8
                        text: page.bal ? ("voix " + (page.bal.voice_db >= 0 ? "+" : "")
                              + page.bal.voice_db.toFixed(1) + " · chœurs "
                              + (page.bal.choir_db !== undefined
                                 ? (page.bal.choir_db >= 0 ? "+" : "")
                                   + page.bal.choir_db.toFixed(1) : "—")
                              + " · musique "
                              + (page.bal.music_db >= 0 ? "+" : "")
                              + page.bal.music_db.toFixed(1) + " · "
                              + page.bal.lufs.toFixed(1) + " LUFS") : "…"
                    }
                }
            }

            // ==== V13.9 : SPATIAL VOIX (widener Lauridsen, mono-compatible) ====
            Rectangle {
                width: parent.width; height: 96; radius: 8; color: "#171c21"
                Column {
                    anchors.fill: parent; anchors.margins: 10; spacing: 6
                    Row {
                        width: parent.width
                        Text { text: "SPATIAL VOIX"; color: "#e0a030"
                               font.pixelSize: 9; font.letterSpacing: 2
                               anchors.verticalCenter: parent.verticalCenter }
                        Item { width: parent.width - 142; height: 1 }
                        Rectangle {
                            width: 56; height: 20; radius: 5
                            property bool on: page.vsp !== null && page.vsp.on === 1
                            color: on ? "#142a19" : "#1b2126"
                            border.color: on ? "#4cc470" : "#39434b"
                            Text { anchors.centerIn: parent
                                   text: parent.on ? "● ON" : "OFF"
                                   color: parent.on ? "#4cc470" : "#8b959d"
                                   font.pixelSize: 9; font.bold: true }
                            TapHandler {
                                gesturePolicy: TapHandler.ReleaseWithinBounds
                                onTapped: mixer.call({ op: "set_vspatial",
                                                       on: parent.on ? 0 : 1 },
                                                     function(r) { if (r.ok) page.vsp = r; })
                            }
                        }
                    }
                    Repeater {
                        // [libellé, clé, min, max, décimales, suffixe]
                        model: [["LARGEUR", "amount",   0, 100, 0, ""],
                                ["RETARD",  "delay_ms", 3,  40, 0, " ms"]]
                        Row {
                            width: parent.width; spacing: 8
                            property real vmin: modelData[2]
                            property real vmax: modelData[3]
                            property real val: page.vsp && page.vsp[modelData[1]] !== undefined
                                               ? page.vsp[modelData[1]] : vmin
                            Text {
                                width: 128
                                anchors.verticalCenter: parent.verticalCenter
                                text: modelData[0] + "  "
                                      + Number(parent.val).toFixed(modelData[4]) + modelData[5]
                                color: "#c9c4b8"; font.pixelSize: 8
                            }
                            Rectangle {
                                width: parent.width - 136; height: 16; radius: 8; color: "#0b0e11"
                                anchors.verticalCenter: parent.verticalCenter
                                Rectangle {
                                    height: parent.height; radius: 8
                                    width: parent.width * Math.max(0, Math.min(1,
                                        (parent.parent.val - parent.parent.vmin)
                                        / (parent.parent.vmax - parent.parent.vmin)))
                                    color: "#39434b"
                                    Rectangle { width: 4; height: parent.height
                                                anchors.right: parent.right; color: "#e5a13c" }
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    preventStealing: true
                                    property string key: modelData[1]
                                    property real vmin: modelData[2]
                                    property real vmax: modelData[3]
                                    onPressed: page.vspDrag = true
                                    onReleased: page.vspDrag = false
                                    onCanceled: page.vspDrag = false
                                    onPositionChanged: (m) => {
                                        if (!pressed) return;
                                        var frac = Math.max(0, Math.min(1, m.x / width));
                                        var v = Math.round(vmin + frac * (vmax - vmin));
                                        var d = page.vsp || {}; d[key] = v; page.vsp = d;
                                        mixer.call({ op: "set_vspatial", [key]: v }, function() {});
                                    }
                                }
                            }
                        }
                    }
                }
            }

            }
            }
            // fin COLONNE DROITE (Flickable + bmxRight)

            // ================= COLONNE GAUCHE : 16 voies =================
            Flickable {
                width: parent.width - 310
                anchors.left: parent.left
                anchors.top: parent.top
                height: parent.height
                contentHeight: bmxCol.height
                clip: true
            Column {
                id: bmxCol
                width: parent.width
                spacing: 5
                Repeater {
                    model: 16
                    // V13.9 : ligne COMPACTE — VU et VOL (keeper) CÔTE À CÔTE,
                    // à gauche des réglages ; plus de bloc superposé à droite.
                    Rectangle {
                        property var ch: page.st && index < page.st.chans.length
                                         ? page.st.chans[index] : null
                        property bool active: ch !== null && ch.role !== "off"
                        property bool measuring: page.st !== null
                                                 && page.st.measuring === index
                        property real kdb: (active && ch) ? ch.keeper_db : 0
                        width: parent.width; height: 46; radius: 6
                        color: measuring ? "#2a1512" : (active ? "#171c21" : "#14181c")
                        border.color: measuring ? "#e05545"
                                      : (active ? "#39434b" : "#22282e")

                        // ---- contrôles (rôle, mesure) ancrés À GAUCHE ----
                        Row {
                            anchors.left: parent.left; anchors.leftMargin: 7
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 6
                            Rectangle {
                                width: 46; height: 32; radius: 5
                                anchors.verticalCenter: parent.verticalCenter
                                color: "#1b2126"; border.color: "#39434b"
                                Text { anchors.centerIn: parent
                                       text: page.stripName(index)
                                       color: "#e5a13c"; font.pixelSize: 12
                                       font.bold: true }
                            }
                            Rectangle {
                                width: 30; height: 32; radius: 5
                                anchors.verticalCenter: parent.verticalCenter
                                color: "#1b2126"; border.color: "#39434b"
                                Text { anchors.centerIn: parent; text: "‹"
                                       color: "#c9c4b8"; font.pixelSize: 17 }
                                TapHandler {
                                    gesturePolicy: TapHandler.ReleaseWithinBounds
                                    onTapped: page.setRole(index, -1)
                                }
                            }
                            Rectangle {
                                width: 104; height: 32; radius: 5
                                anchors.verticalCenter: parent.verticalCenter
                                color: "#0f1216"
                                border.color: active ? "#e5a13c" : "#22282e"
                                Text { anchors.centerIn: parent
                                       text: {
                                           if (!ch || !ch.role) return "—";
                                           var k = page.roleKeys.indexOf(ch.role);
                                           return k >= 0 ? page.roleNames[k]
                                                         : String(ch.role);
                                       }
                                       color: active ? "#e9e5da" : "#3a434b"
                                       font.pixelSize: 10; font.bold: true }
                            }
                            Rectangle {
                                width: 30; height: 32; radius: 5
                                anchors.verticalCenter: parent.verticalCenter
                                color: "#1b2126"; border.color: "#39434b"
                                Text { anchors.centerIn: parent; text: "›"
                                       color: "#c9c4b8"; font.pixelSize: 17 }
                                TapHandler {
                                    gesturePolicy: TapHandler.ReleaseWithinBounds
                                    onTapped: page.setRole(index, 1)
                                }
                            }
                            // MESURER / résultat
                            Rectangle {
                                width: 96; height: 32; radius: 5
                                anchors.verticalCenter: parent.verticalCenter
                                visible: active
                                color: measuring ? "#3a1512"
                                       : (ch && ch.done ? "#142a19" : "#2a2214")
                                border.color: measuring ? "#e05545"
                                       : (ch && ch.done ? "#4cc470" : "#e5a13c")
                                Text {
                                    anchors.centerIn: parent
                                    text: measuring
                                          ? "● " + page.st.meas_elapsed + "/12"
                                          : (ch && ch.done
                                             ? "✓ " + ch.rms_db.toFixed(1)
                                             : "MESURER")
                                    color: measuring ? "#ff6a5a"
                                           : (ch && ch.done ? "#4cc470" : "#e5a13c")
                                    font.pixelSize: 10; font.bold: true
                                    font.family: measuring || (ch && ch.done)
                                                 ? "monospace" : ""
                                }
                                TapHandler {
                                    enabled: !measuring
                                    gesturePolicy: TapHandler.ReleaseWithinBounds
                                    onTapped: mixer.call({ op: "bandmix_measure",
                                                           src: index },
                                                         function() { page.refresh(); })
                                }
                            }
                        }
                        // ---- VU + VOL CÔTE À CÔTE, ancrés À DROITE ----
                        Row {
                            visible: active
                            spacing: 10
                            anchors.right: parent.right
                            anchors.rightMargin: 10
                            anchors.verticalCenter: parent.verticalCenter

                            // --- VU d'entrée ---
                            Row {
                                spacing: 5
                                anchors.verticalCenter: parent.verticalCenter
                                Text { text: "VU"; color: "#5c666e"
                                       anchors.verticalCenter: parent.verticalCenter
                                       font.pixelSize: 8; font.letterSpacing: 1 }
                                Rectangle {
                                    width: 150; height: 12; radius: 6
                                    anchors.verticalCenter: parent.verticalCenter
                                    color: "#0b0e11"
                                    Rectangle {
                                        height: parent.height; radius: 6
                                        width: parent.width * Math.max(0, Math.min(1,
                                            index < mixer.inLevels.length
                                            ? mixer.inLevels[index] : 0))
                                        color: "#3d8a54"
                                        Behavior on width { NumberAnimation { duration: 90 } }
                                    }
                                }
                            }

                            // --- VOL (keeper) bipolaire ±24 dB ---
                            Row {
                                spacing: 5
                                anchors.verticalCenter: parent.verticalCenter
                                Text { text: "VOL"; color: "#5c666e"
                                       anchors.verticalCenter: parent.verticalCenter
                                       font.pixelSize: 8 }
                                Rectangle {
                                    width: 130; height: 12; radius: 6
                                    anchors.verticalCenter: parent.verticalCenter
                                    color: "#0b0e11"; clip: true
                                    Rectangle {                // repère central
                                        x: parent.width / 2 - 1; width: 2
                                        height: parent.height; color: "#39434b"
                                    }
                                    Rectangle {
                                        property real k: Math.max(-24, Math.min(24, kdb))
                                        property real half: parent.width / 2
                                        x: k >= 0 ? half : half + k / 24 * half
                                        width: Math.abs(k) / 24 * half
                                        height: parent.height; radius: 6
                                        color: k >= 0 ? "#4cc470" : "#e8b84b"
                                        Behavior on x { NumberAnimation { duration: 300; easing.type: Easing.OutCubic } }
                                        Behavior on width { NumberAnimation { duration: 300; easing.type: Easing.OutCubic } }
                                    }
                                }
                                Text {
                                    width: 44
                                    anchors.verticalCenter: parent.verticalCenter
                                    horizontalAlignment: Text.AlignRight
                                    text: (kdb >= 0 ? "+" : "") + kdb.toFixed(1)
                                    color: kdb >= 0 ? "#4cc470" : "#e8b84b"
                                    font.pixelSize: 10; font.bold: true
                                }
                            }
                        }
                    }
                }
            }
        }
        }

        Text {
            text: "AUTOMIX LIVE : un tap, la console équilibre en continu (référence voix). Ou manuel : rôle · MESURER · CALCULER · VERROUILLER · LIVE."
            color: "#5c666e"; font.pixelSize: 10; font.letterSpacing: 1
            width: parent.width; wrapMode: Text.WordWrap
        }
    }
}
