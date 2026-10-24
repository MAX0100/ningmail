import QtQuick
import QtQuick.Controls

Pane {
    id: root

    padding: 0
    background: Rectangle {
        color: Theme.surfaceAlt
    }

    ListView {
        id: folderList

        anchors.fill: parent
        clip: true
        model: appController.folders

        // R57/R61: highlight comes from the QML-internal currentIndex only.
        highlightMoveDuration: 0

        EmptyState {
            anchors.fill: parent
            visible: folderList.count === 0
            title: qsTr("还没有账号")
            description: qsTr("请先添加账号，文件夹会同步后出现在这里")
        }

        delegate: ItemDelegate {
            id: folderDelegate

            width: ListView.view.width
            height: 44
            highlighted: ListView.isCurrentItem
            onClicked: {
                folderList.currentIndex = index
                appController.selectFolder(model.id)
            }

            background: Rectangle {
                color: folderDelegate.highlighted ? Theme.accent : "transparent"
            }

            contentItem: Text {
                text: model.name
                color: folderDelegate.highlighted ? "#FFFFFF" : Theme.textPrimary
                font.pixelSize: 14
                elide: Text.ElideRight
                verticalAlignment: Text.AlignVCenter
            }
        }
    }
}
