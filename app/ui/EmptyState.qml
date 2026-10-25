import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    property string title: ""
    property string description: ""
    /// Task 15 (R93⑧): optional primary action. Empty (default) adds no
    /// button; the host owns the handler (FolderRail forwards to Main's
    /// AccountSetupDialog).
    property string actionText: ""
    signal actionTriggered()

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

        Button {
            objectName: "addAccountButton"
            visible: root.actionText.length > 0
            text: root.actionText
            Layout.alignment: Qt.AlignHCenter
            onClicked: root.actionTriggered()
        }
    }
}
