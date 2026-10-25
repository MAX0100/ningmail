import QtQuick
import QtQuick.Controls
import QtTest
import NingMail

TestCase {
    id: root

    name: "AccountSetupDialog"

    // Direct instantiation like tst_composer.qml; the dialog parented to this
    // TestCase item keeps the overlay inside the test window offscreen.
    Component {
        id: dialogComponent

        AccountSetupDialog {}
    }

    SignalSpy { id: createSpy; signalName: "accountCreateRequested" }

    function findObjectByName(item, objectName) {
        if (item === null || item === undefined) {
            return null
        }
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

    // The widgets split between the dialog's contentItem and its footer.
    function locateFields(dialog) {
        const find = function(name) {
            // The footer carries saveButton; contentItem carries the rest.
            return findObjectByName(dialog.contentItem, name)
                || findObjectByName(dialog.footer, name)
        }
        const fields = {
            displayName: find("nameField"),
            email: find("emailField"),
            imapHost: find("imapHostField"),
            imapPort: find("imapPortField"),
            imapSecurity: find("imapSecurityBox"),
            smtpHost: find("smtpHostField"),
            smtpPort: find("smtpPortField"),
            smtpSecurity: find("smtpSecurityBox"),
            username: find("userField"),
            password: find("passwordField"),
            save: find("saveButton")
        }
        for (const key in fields) {
            verify(fields[key] !== null, key + " must exist")
        }
        return fields
    }

    function fillValid(dialog) {
        const fields = locateFields(dialog)
        fields.displayName.text = "Home"
        fields.email.text = "home@example.com"
        fields.imapHost.text = "imap.example.com"
        fields.smtpHost.text = "smtp.example.com"
        fields.username.text = "home@example.com"
        fields.password.text = "secret"
        return fields
    }

    function freshDialog() {
        const dialog = createTemporaryObject(dialogComponent, root)
        verify(dialog !== null, "dialog must instantiate")
        dialog.open()
        tryVerify(function() { return dialog.opened })
        return dialog
    }

    function test_dialog_all_named_fields_exist() {
        const dialog = freshDialog()
        locateFields(dialog)
        compare(findObjectByName(dialog.contentItem, "imapSecurityBox").currentValue, "tls")
        compare(findObjectByName(dialog.contentItem, "smtpSecurityBox").currentValue, "tls")
    }

    function test_save_disabled_without_required_fields() {
        const dialog = freshDialog()
        const fields = locateFields(dialog)

        // Fresh: defaults (ports filled) exist but every required field is empty.
        verify(!fields.save.enabled, "save must stay disabled with empty required fields")

        fillValid(dialog)
        verify(fields.save.enabled, "valid input must enable save")

        // Clearing any single required field disables saving again.
        fields.imapHost.text = ""
        verify(!fields.save.enabled, "an empty imap host must disable save")
    }

    function test_save_disabled_on_out_of_range_ports() {
        const dialog = freshDialog()
        fillValid(dialog)
        const fields = locateFields(dialog)

        fields.imapPort.text = "0"
        verify(!fields.save.enabled, "port 0 must disable save")
        fields.imapPort.text = "65536"
        verify(!fields.save.enabled, "port 65536 must disable save")
        fields.imapPort.text = "993"
        verify(fields.save.enabled, "port 993 must enable save again")

        fields.smtpPort.text = "65536"
        verify(!fields.save.enabled, "out-of-range smtp port must disable save")
        fields.smtpPort.text = "465"
        verify(fields.save.enabled, "valid smtp port must enable save")
    }

    function test_save_emits_fields_with_host_and_port() {
        const dialog = freshDialog()
        const fields = fillValid(dialog)
        fields.imapSecurity.currentIndex = 0 // "tls"
        fields.smtpSecurity.currentIndex = 1 // "starttls"

        createSpy.target = dialog
        createSpy.clear()
        mouseClick(fields.save)
        tryVerify(function() { return createSpy.count >= 1 })
        compare(createSpy.count, 1)

        const args = createSpy.signalArguments[0]
        compare(args.length, 1)
        const sent = args[0]
        compare(sent.displayName, "Home")
        compare(sent.email, "home@example.com")
        compare(sent.imapHost, "imap.example.com")
        compare(sent.imapPort, 993)
        compare(sent.imapSecurity, "tls")
        compare(sent.smtpHost, "smtp.example.com")
        compare(sent.smtpPort, 465)
        compare(sent.smtpSecurity, "starttls")
        compare(sent.username, "home@example.com")
        // The password travels inside fields only; the dialog never logs it.
        compare(sent.password, "secret")

        // A successful save closes the dialog itself.
        tryVerify(function() { return !dialog.visible })
    }
}
