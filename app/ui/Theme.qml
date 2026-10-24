pragma Singleton

import QtQuick

QtObject {
    // M1 light palette; the dark theme lands in M3.
    readonly property color background: "#F5F6FA"
    readonly property color surface: "#FFFFFF"
    readonly property color surfaceAlt: "#E9ECF2"
    readonly property color textPrimary: "#1C1E26"
    readonly property color textSecondary: "#6B7280"
    readonly property color accent: "#4A6CF7"
    readonly property color danger: "#D9363E"
    readonly property color divider: "#DFE2E9"
    readonly property int radius: 8
    readonly property int spacing: 12
}
