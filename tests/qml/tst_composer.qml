import QtQuick
import QtQuick.Controls
import QtTest
import NingMail

TestCase {
    name: "ComposerWindow"

    // Own window: offscreen supports Window instances (see tst_delegate.qml).
    Component {
        id: composerComponent

        ComposerWindow { visible: true }
    }

    SignalSpy { id: sendSpy; signalName: "sendRequested" }
    SignalSpy { id: saveSpy; signalName: "draftSaveRequested" }

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

    function find(composer, objectName) {
        return findObjectByName(composer.contentItem, objectName)
    }

    function locateFields(composer) {
        const toField = find(composer, "toField")
        const ccField = find(composer, "ccField")
        const subjectField = find(composer, "subjectField")
        const bodyField = find(composer, "bodyField")
        const sendButton = find(composer, "sendButton")
        const errorLabel = find(composer, "errorLabel")
        verify(toField !== null, "toField must exist")
        verify(ccField !== null, "ccField must exist")
        verify(subjectField !== null, "subjectField must exist")
        verify(bodyField !== null, "bodyField must exist")
        verify(sendButton !== null, "sendButton must exist")
        verify(errorLabel !== null, "errorLabel must exist")
        return { to: toField, cc: ccField, subject: subjectField,
                 body: bodyField, send: sendButton, error: errorLabel }
    }

    function test_composer_prefills_initial_values() {
        const composer = createTemporaryObject(composerComponent, null, {
            draftId: 3,
            initialTo: "rong@example.com",
            initialCc: "cc@example.com",
            initialSubject: "Quarterly notes",
            initialBody: "First line"
        })
        verify(composer !== null, "composer window must instantiate")
        const fields = locateFields(composer)

        compare(fields.to.text, "rong@example.com")
        compare(fields.cc.text, "cc@example.com")
        compare(fields.subject.text, "Quarterly notes")
        compare(fields.body.text, "First line")
        // Main writes the store id back into the window between saves.
        compare(composer.draftId, 3)
    }

    function test_composer_sends_parsed_addresses() {
        const composer = createTemporaryObject(composerComponent, null)
        verify(composer !== null, "composer window must instantiate")
        const fields = locateFields(composer)

        fields.to.text = "rong@example.com, plain@example.com"
        fields.cc.text = "cc@example.com, cc2@example.com"
        fields.subject.text = "Quarterly notes"
        fields.body.text = "First line\nSecond line"

        // cc-only recipients are not enough to enable sending (R78).
        compare(composer.recipients.length, 2)

        verify(fields.send.enabled, "send button must enable for non-empty recipients")

        sendSpy.target = composer
        sendSpy.clear()
        mouseClick(fields.send)
        compare(sendSpy.count, 1)
        const args = sendSpy.signalArguments[0]
        compare(args.length, 4)
        // The QML layer splits comma-separated text and drops empty parts.
        compare(args[0].length, 2)
        compare(args[0][0], "rong@example.com")
        compare(args[0][1], "plain@example.com")
        compare(args[1].length, 2)
        compare(args[1][0], "cc@example.com")
        compare(args[1][1], "cc2@example.com")
        compare(args[2], "Quarterly notes")
        compare(args[3], "First line\nSecond line")
    }

    function test_composer_blocks_empty_recipients() {
        const composer = createTemporaryObject(composerComponent, null)
        verify(composer !== null, "composer window must instantiate")
        const fields = locateFields(composer)

        // Fresh window: no recipients, button disabled, error text set.
        compare(composer.recipients.length, 0)
        verify(!fields.send.enabled, "send button must stay disabled without recipients")
        compare(fields.error.text, qsTr("请填写收件人"))

        // cc-only is not a valid recipient list either.
        fields.cc.text = "cc@example.com"
        compare(composer.recipients.length, 0)
        verify(!fields.send.enabled, "cc-only recipients must not enable sending")

        // One valid recipient through the field flips both states.
        fields.to.text = "rong@example.com"
        compare(fields.error.text, "")
        verify(fields.send.enabled, "one valid recipient must enable sending")
    }

    function test_composer_autosave_skips_pristine_window() {
        // Opening and never typing must not pile empty draft rows into the
        // store: the tick only carries a draft once content exists.
        const composer = createTemporaryObject(composerComponent, null, {
            draftId: -1,
            autosaveInterval: 50
        })
        verify(composer !== null, "composer window must instantiate")
        saveSpy.target = composer
        saveSpy.clear()

        wait(250) // several ticks' worth of pristine stillness
        compare(saveSpy.count, 0)

        // Any content switches the autosave back on.
        const fields = locateFields(composer)
        fields.subject.text = "Quarterly notes"
        tryVerify(function() { return saveSpy.count >= 1 })
        compare(saveSpy.signalArguments[0][0], -1)
    }

    function test_composer_autosave_chain() {
        const composer = createTemporaryObject(composerComponent, null, {
            draftId: -1,
            autosaveInterval: 50
        })
        verify(composer !== null, "composer window must instantiate")
        const fields = locateFields(composer)
        compare(composer.autosaveInterval, 50)
        compare(composer.draftId, -1)

        // Editing any field restarts the debounce timer; interval 50 pins the
        // Timer -> draftSaveRequested chain without waiting the real 3 s.
        saveSpy.target = composer
        saveSpy.clear()
        fields.to.text = "rong@example.com"
        fields.subject.text = "Quarterly notes"
        fields.body.text = "First line"
        tryVerify(function() { return saveSpy.count >= 1 })
        compare(saveSpy.count, 1)
        const first = saveSpy.signalArguments[0]
        compare(first.length, 5)
        compare(first[0], -1) // initial draftId flows through unchanged
        compare(first[1].length, 1)
        compare(first[1][0], "rong@example.com")
        compare(first[2].length, 0)
        compare(first[3], "Quarterly notes")
        compare(first[4], "First line")

        // Main writes the saved id back into the window; the next tick must
        // carry that id so saveDraft() switches from INSERT to UPDATE.
        composer.draftId = 7
        fields.body.text = "First line\nSecond line"
        tryVerify(function() { return saveSpy.count >= 2 })
        compare(saveSpy.count, 2)
        const second = saveSpy.signalArguments[1]
        compare(second[0], 7)
        compare(second[4], "First line\nSecond line")
    }
}
