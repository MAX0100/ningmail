import QtQuick
import QtQuick.Controls
import QtTest
import NingMail

TestCase {
    name: "MessageListDelegate"

    // Own window: visual states (visible) only settle for a rendered window.
    ApplicationWindow {
        id: testWindow

        width: 400
        height: 300
        visible: true

        ListModel {
            id: dataModel

            ListElement {
                unread: true
                from: "Seed Sender"
                fromAddress: "sender@ningmail.test"
                subject: "Seed message"
                snippet: "Seed snippet"
                date: "2026-01-01T08:00:00Z"
                hasAttachments: true
                flagged: false
            }
        }

        ListView {
            id: listView

            objectName: "delegateListView"
            width: 320
            height: 200
            model: dataModel
            delegate: MessageListDelegate {}
        }
    }

    function findObjectByName(item, objectName) {
        if (item.objectName === objectName) {
            return item
        }
        for (let i = 0; i < item.children.length; ++i) {
            const found = findObjectByName(item.children[i], objectName)
            if (found) {
                return found
            }
        }
        return null
    }

    function test_delegate_identity_and_unread_point() {
        const view = testWindow.contentItem.children[testWindow.contentItem.children.length - 1]
        const delegate = view.itemAtIndex(0)
        verify(delegate !== null, "delegate must be instantiated")
        compare(delegate.objectName, "messageDelegate")

        const unreadDot = findObjectByName(delegate, "unreadDot")
        verify(unreadDot !== null, "unread dot must exist")
        verify(unreadDot.visible, "unread point must be visible")

        const attachmentIcon = findObjectByName(delegate, "attachmentIcon")
        verify(attachmentIcon !== null, "attachment icon must exist")
        verify(attachmentIcon.visible, "attachment icon must be visible")
    }
}
