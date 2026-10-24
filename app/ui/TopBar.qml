import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ToolBar {
    id: root

    background: Rectangle {
        color: Theme.surface
        border.color: Theme.divider
    }

    RowLayout {
        anchors.fill: parent
        spacing: Theme.spacing

        Label {
            text: qsTr("NingMail")
            font.pixelSize: 18
            font.bold: true
            color: Theme.textPrimary
        }

        TextField {
            id: searchField

            objectName: "searchField"
            // M1: display only; search logic lands later.
            placeholderText: qsTr("搜索邮件…")
            Layout.fillWidth: true
            Layout.maximumWidth: 420
        }

        Button {
            objectName: "composeButton"
            text: qsTr("✉ 写信")
            // Enabled in task 11 once the composer window exists.
            enabled: false
        }

        Button {
            objectName: "refreshButton"
            text: qsTr("⟳ 刷新")
            onClicked: appController.refresh()
        }
    }
}
