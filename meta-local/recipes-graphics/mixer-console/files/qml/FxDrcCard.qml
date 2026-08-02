// SPDX-License-Identifier: GPL-2.0-or-later
// Carte DRC / MULTIBAND (blob SOF, sliders par bande)
// Extrait de StripFxDrawer.qml (V14.0 étape 8). Instancié via le wrapper
// Component du drawer -> la chaîne de contextes QML est inchangée : `ctl`
// (propriété du Loader délégué) et `drawer` (id du document parent) se
// résolvent comme avant l'extraction.
import QtQuick
import "stripfx.js" as FX

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
