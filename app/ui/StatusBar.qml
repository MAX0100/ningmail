import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ToolBar {
    id: root

    background: Rectangle {
        color: Theme.surfaceAlt
        border.color: Theme.divider
    }

    RowLayout {
        anchors.fill: parent
        spacing: Theme.spacing

        Label {
            text: qsTr("⟳")
            visible: appController.syncing
            color: Theme.accent
            font.pixelSize: 14

            RotationAnimation on rotation {
                from: 0
                to: 360
                duration: 1000
                loops: Animation.Infinite
                running: appController.syncing
            }
        }

        Label {
            text: appController.statusText
            color: Theme.textSecondary
            font.pixelSize: 12
            elide: Text.ElideRight
            Layout.fillWidth: true
        }
    }
}
