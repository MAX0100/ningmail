import QtQuick
import QtTest
import NingMail

TestCase {
    name: "Theme"

    function test_tokens() {
        compare(Theme.background.a, 1.0)
        verify(Theme.accent !== undefined)
    }

    function test_all_tokens_present() {
        verify(Theme.surface !== undefined)
        verify(Theme.surfaceAlt !== undefined)
        verify(Theme.textPrimary !== undefined)
        verify(Theme.textSecondary !== undefined)
        verify(Theme.danger !== undefined)
        verify(Theme.divider !== undefined)
        verify(Theme.radius !== undefined)
        verify(Theme.spacing !== undefined)
    }
}
