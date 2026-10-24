import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    property string title: ""
    property string description: ""

    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(parent.width * 0.8, 360)
        spacing: Theme.spacing

        Label {
            text: root.title
            color: Theme.textPrimary
            font.pixelSize: 16
            font.bold: true
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        Label {
            text: root.description
            color: Theme.textSecondary
            font.pixelSize: 13
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
    }
}
