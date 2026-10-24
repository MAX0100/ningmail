import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ItemDelegate {
    id: root

    objectName: "messageDelegate"
    width: ListView.view ? ListView.view.width : 320
    implicitHeight: 78
    highlighted: ListView.isCurrentItem

    contentItem: RowLayout {
        spacing: 6

        // Unread marker.
        Rectangle {
            objectName: "unreadDot"
            Layout.preferredWidth: 10
            Layout.preferredHeight: 10
            Layout.alignment: Qt.AlignTop
            radius: 5
            color: Theme.accent
            visible: model.unread ?? false
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2

            RowLayout {
                Layout.fillWidth: true

                Label {
                    text: model.from ?? ""
                    color: Theme.textPrimary
                    font.pixelSize: 13
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }

                Label {
                    text: model.date instanceof Date
                          ? Qt.formatDateTime(model.date, "MM-dd hh:mm") : ""
                    color: Theme.textSecondary
                    font.pixelSize: 11
                }
            }

            RowLayout {
                Layout.fillWidth: true

                Label {
                    text: model.subject ?? ""
                    color: Theme.textPrimary
                    font.pixelSize: 14
                    font.bold: model.unread ?? false
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }

                Label {
                    objectName: "attachmentIcon"
                    text: "📎"
                    visible: model.hasAttachments ?? false
                }
            }

            Label {
                text: model.snippet ?? ""
                color: Theme.textSecondary
                font.pixelSize: 12
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
        }
    }

    onClicked: appController.openMessage(model.id)
}
