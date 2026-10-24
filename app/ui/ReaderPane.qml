import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import NingMail.Components

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
            visible: (appController.reader.html ?? "") !== ""
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: width
            contentHeight: Math.max(messageBody.contentHeight, height)

            // Task 10b: the HTML body view. The controller already converted
            // plain-text bodies into an escaped <pre> block, so the reader
            // html is the single source of truth; MessageViewItem sanitizes
            // again inside LiteHtmlRenderer before litehtml ever parses.
            MessageViewItem {
                id: messageBody

                width: parent.width
                // QQuickPaintedItem paints into its bounding rect; the layout
                // height must equal the rendered image's logical height or the
                // blit gets clipped to zero.
                height: messageBody.contentHeight
                html: appController.reader.html ?? ""
            }
        }
    }
}
