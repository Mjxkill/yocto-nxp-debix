// SPDX-License-Identifier: GPL-2.0-or-later
// Contrôle simple : INTEGER→knob dB, BOOLEAN→switch, ENUM→cycle
// Extrait de StripFxDrawer.qml (V14.0 étape 8). Instancié via le wrapper
// Component du drawer -> la chaîne de contextes QML est inchangée : `ctl`
// (propriété du Loader délégué) et `drawer` (id du document parent) se
// résolvent comme avant l'extraction.
import QtQuick
import "stripfx.js" as FX

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
