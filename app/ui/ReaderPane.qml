import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Pane {
    id: root

    padding: Theme.spacing

    background: Rectangle {
        color: Theme.surface
    }

    // No selection yet (reader.id stays 0 when empty per the controller contract).
    EmptyState {
        anchors.fill: parent
        visible: appController.reader.id === 0
        title: qsTr("还没有打开邮件")
        description: qsTr("从中间列表选择一封邮件，正文会显示在这里")
    }

    ColumnLayout {
        anchors.fill: parent
        visible: appController.reader.id !== 0
        spacing: Theme.spacing

        Label {
            text: appController.reader.subject ?? ""
            color: Theme.textPrimary
            font.pixelSize: 20
            font.bold: true
            wrapMode: Text.Wrap
            elide: Text.ElideRight
            Layout.fillWidth: true
        }

        RowLayout {
            Layout.fillWidth: true

            Label {
                text: appController.reader.from ?? ""
                color: Theme.textSecondary
                font.pixelSize: 13
                elide: Text.ElideRight
                Layout.fillWidth: true
            }

            Label {
                // Defensive binding: reader.date is a QVariant QDateTime; in
                // some binding contexts it arrives as an invalid/null value,
                // so only format when the key carries a value at all (R60
                // keeps the C++ contract fixed at QDateTime).
                text: appController.reader.date !== undefined && appController.reader.date !== null
                      ? Qt.formatDateTime(appController.reader.date, "yyyy-MM-dd hh:mm") : ""
                color: Theme.textSecondary
                font.pixelSize: 13
            }
        }

        Rectangle {
            color: Theme.divider
            Layout.fillWidth: true
            Layout.preferredHeight: 1
        }

        Label {
            text: qsTr("正在加载…")
            visible: appController.reader.loading === true
            color: Theme.textSecondary
        }

        Label {
            text: appController.reader.error ?? ""
            visible: (appController.reader.error ?? "") !== ""
            color: Theme.danger
            wrapMode: Text.Wrap
            Layout.fillWidth: true
        }

        Flickable {
            clip: true
            visible: (appController.reader.plainText ?? "") !== ""
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: width
            contentHeight: bodyTextArea.height

            TextArea {
                id: bodyTextArea

                // Plain text view; the HTML view lands in task 10b.
                readOnly: true
                text: appController.reader.plainText ?? ""
                wrapMode: TextArea.Wrap
                color: Theme.textPrimary
                background: null
            }
        }
    }
}
