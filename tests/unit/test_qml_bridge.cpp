#include <QMetaObject>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QString>
#include <QStringList>
#include <QVariant>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <utility>

namespace {

/// Mirror of the AppController::saveDraft/sendMessage signatures (task 11):
/// the probe records exactly what crossed the QML -> C++ boundary.
class BridgeProbe : public QObject {
    Q_OBJECT

  public:
    Q_INVOKABLE qint64 probeSave(qint64 draftId, const QStringList &to, const QStringList &cc,
                                 const QString &subject, const QString &body) {
        lastDraftId = draftId;
        lastTo = to;
        lastCc = cc;
        lastSubject = subject;
        lastBody = body;
        saveCalls++;
        return draftId == -1 ? qint64(42) : draftId;
    }

    Q_INVOKABLE void probeSend(const QStringList &to, const QStringList &cc, const QString &subject,
                               const QString &body) {
        sendTo = to;
        sendCc = cc;
        sendSubject = subject;
        sendBody = body;
        sendCalls++;
    }

    qint64 lastDraftId = 0;
    QStringList lastTo;
    QStringList lastCc;
    QString lastSubject;
    QString lastBody;
    int saveCalls = 0;

    QStringList sendTo;
    QStringList sendCc;
    QString sendSubject;
    QString sendBody;
    int sendCalls = 0;
};

/// The parse mode ComposerWindow.qml uses: split the raw field text on
/// commas, trim every part, drop the empties, and hand the result to the
/// controller call as a plain JS array of strings.
const char *kBridgeQml = R"qml(import QtQuick

Item {
    function parse(text) {
        const parts = text.split(",")
        const addresses = []
        for (let i = 0; i < parts.length; ++i) {
            const address = parts[i].trim()
            if (address.length > 0) {
                addresses.push(address)
            }
        }
        return addresses
    }

    function runSave(draftId, toText, ccText) {
        return probeBridge.probeSave(draftId, parse(toText), parse(ccText),
                                     "Quarterly notes", "First line")
    }

    function runSend(toText, ccText) {
        probeBridge.probeSend(parse(toText), parse(ccText), "Quarterly notes", "First line")
    }
})qml";

} // namespace

// R89: pins the assumption "today's bridge already converts a QML var array
// into const QStringList&" for the exact signatures AppController uses. If
// this ever turns red, the fix is either the C++ parameter type or an
// explicit string-array conversion on the QML side — never a silent loss.
TEST_CASE("QML var arrays bridge into QStringList probe signatures", "[bridge]") {
    BridgeProbe probe;
    QQmlEngine engine;
    // Engine is declared after the probe so teardown order stays safe: the
    // probe is a context property that must outlive the engine.
    engine.rootContext()->setContextProperty(QStringLiteral("probeBridge"), &probe);

    QQmlComponent component(&engine);
    component.setData(kBridgeQml, QUrl(QStringLiteral("qrc:/bridge/test")));
    REQUIRE(component.isReady());

    std::unique_ptr<QObject> root(component.create());
    REQUIRE(root != nullptr);

    // saveDraft path: "a@x, b@y" plus a name-less extra and surrounding
    // whitespace must arrive as two clean addresses; cc likewise; the qint64
    // return value round-trips back into JS.
    QVariant result;
    REQUIRE(QMetaObject::invokeMethod(root.get(), "runSave", Q_RETURN_ARG(QVariant, result),
                                      Q_ARG(QVariant, -1), Q_ARG(QVariant, " a@x , b@y,,"),
                                      Q_ARG(QVariant, "cc@x")));
    CHECK(probe.saveCalls == 1);
    CHECK(probe.lastDraftId == -1);
    CHECK(probe.lastTo == QStringList{QStringLiteral("a@x"), QStringLiteral("b@y")});
    CHECK(probe.lastCc == QStringList{QStringLiteral("cc@x")});
    CHECK(probe.lastSubject == QStringLiteral("Quarterly notes"));
    CHECK(probe.lastBody == QStringLiteral("First line"));
    CHECK(result.toLongLong() == 42); // qint64 across the bridge, value intact

    // sendMessage path: same conversion for the void-returning overload.
    REQUIRE(QMetaObject::invokeMethod(root.get(), "runSend", Q_ARG(QVariant, " send@x "),
                                      Q_ARG(QVariant, ",cc2@x,")));
    CHECK(probe.sendCalls == 1);
    CHECK(probe.sendTo == QStringList{QStringLiteral("send@x")});
    CHECK(probe.sendCc == QStringList{QStringLiteral("cc2@x")});
    CHECK(probe.sendSubject == QStringLiteral("Quarterly notes"));
    CHECK(probe.sendBody == QStringLiteral("First line"));
}

#include "test_qml_bridge.moc"
