#include "AppController.h"
#include "FakeMailBackend.h"
#include "MailtoParser.h"
#include "MemoryCredentialStore.h"
#include "SendService.h"
#include "SingleInstance.h"
#include "Store/AccountStore.h"
#include "Store/Database.h"
#include "Store/DraftStore.h"
#include "Store/FolderStore.h"
#include "Store/MessageStore.h"
#include "Store/OutboxStore.h"
#include "SyncService.h"
#include "Types.h"
#include "Version.h"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QTemporaryDir>
#include <QUrl>

#include <iostream>
#include <memory>
#include <string_view>

namespace {

int runApplication(int argc, char *argv[], bool smokeTest) {
    QGuiApplication app(argc, argv);

    // Task 15 (R93⑥): the single-instance gate runs after QGuiApplication and
    // is skipped entirely for --smoke-test so concurrent ctest smoke runs
    // never kick each other off the shared key. Later processes forward their
    // program-name-stripped argv to the running primary and exit.
    std::unique_ptr<ningmail::SingleInstance> singleInstance;
    if (!smokeTest) {
        singleInstance =
            std::make_unique<ningmail::SingleInstance>(QStringLiteral("ningmail-single-instance"));
        if (!singleInstance->isPrimary()) {
            // R94①: a successful forward means the running primary took over;
            // this instance exits without touching the database or the UI.
            if (singleInstance->sendArguments(app.arguments().mid(1)) &&
                !singleInstance->isPrimary()) {
                return 0;
            }
            // Forwarding failed: either the dead primary's stale lock was
            // reclaimed and this instance is now primary (keep booting), or
            // no single-instance IPC is possible and the GUI still runs (the
            // degraded state was logged by SingleInstance).
        }
    }

    // --smoke-test runs against a one-shot temp directory DB that is discarded
    // when the process exits, so smoke runs never touch the real mailbox (R59).
    std::unique_ptr<QTemporaryDir> smokeDir;
    QString dbPath = ningmail::Database::defaultPath();
    if (smokeTest) {
        smokeDir = std::make_unique<QTemporaryDir>();
        if (!smokeDir->isValid()) {
            qWarning("Failed to create the smoke-test temp directory");
            return 1;
        }
        dbPath = smokeDir->filePath("smoke.db");
    }

    ningmail::Database database(dbPath);
    const ningmail::MailError openError = database.open();
    if (openError) {
        qWarning("Failed to open the mailbox database: %s", openError.detail.toStdString().c_str());
        return 1;
    }
    if (database.migrate()) {
        qWarning("Failed to migrate the mailbox database schema");
        return 1;
    }

    ningmail::AccountStore accountStore(database);
    ningmail::FolderStore folderStore(database);
    ningmail::MessageStore messageStore(database);
    // Drafts live in the same database file. The controller only keeps a
    // nullable pointer to this store, so the object must outlive it: declare
    // it here, before both the controller and the QML engine (see the
    // teardown-order note further down).
    ningmail::DraftStore draftStore(database);

    // R59: M1 always runs against the fake backend (the real backend is task
    // 13); demo secrets stay in memory and never reach the platform keystore.
    const auto accounts = accountStore.list();
    if (!accounts.ok()) {
        qWarning("Failed to read accounts: %s", accounts.error.message.toStdString().c_str());
        return 1;
    }
    if (accounts.value->isEmpty()) {
        ningmail::Account demo;
        demo.displayName = QStringLiteral("演示账号");
        demo.email = QStringLiteral("demo@ningmail.local");
        demo.imapHost = QStringLiteral("demo.ningmail.local");
        demo.username = QStringLiteral("demo@ningmail.local");
        demo.credentialRef = QStringLiteral("demo-account");
        const auto created = accountStore.create(demo);
        if (!created.ok()) {
            qWarning("Failed to seed the demo account: %s",
                     created.error.message.toStdString().c_str());
            return 1;
        }
    }

    // The demo credential always exists by now (seeded above on first run).
    ningmail::MemoryCredentialStore credentials;
    credentials.write(QStringLiteral("demo-account"), QStringLiteral("secret"));

    // The outbox lives in the same database file; like the draft store it must
    // outlive both the controller and the QML engine, so it is declared here.
    ningmail::OutboxStore outboxStore(database);

    ningmail::FakeMailBackendFactory backendFactory;
    // The send service shares the fake backend (R59: M1 always runs against
    // the fake backend) and the outbox store.
    ningmail::SendService sendService(outboxStore, backendFactory, credentials);
    ningmail::SyncService syncService(&backendFactory, &credentials);
    ningmail::AppController appController(accountStore, folderStore, messageStore, syncService,
                                          credentials);
    appController.setDraftStore(&draftStore);
    appController.setSendService(&sendService);

    appController.initialize();

    // The engine is created last and therefore destroyed first.  QML bindings
    // may re-evaluate while the window is being torn down; every C++ object a
    // binding can reach (most importantly the appController context object)
    // must still be alive at that point, so none of these declarations may be
    // moved below the engine.
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("appController"), &appController);
    engine.loadFromModule(u"NingMail", u"Main");
    if (engine.rootObjects().isEmpty()) {
        qWarning("Failed to load the NingMail UI");
        return 1;
    }
    if (smokeTest) {
        return 0;
    }

    // Task 15 (R93⑥): primary side. QLocalServer newConnection events arrive
    // inside app.exec(); each forwarded argument resolves independently (a
    // bad one only logs and never kills the primary or its window).
    if (singleInstance) {
        QObject::connect(singleInstance.get(), &ningmail::SingleInstance::argumentsReceived,
                         &appController, [&appController](const QStringList &arguments) {
                             for (const QString &raw : arguments) {
                                 // `mailto:...` must work straight from the command line,
                                 // where nothing pre-decoded exists yet.
                                 const QUrl url = QUrl::fromUserInput(raw);
                                 if (!url.isValid() || url.scheme() != QLatin1String("mailto")) {
                                     qWarning() << "Ignoring non-mailto argument:" << raw;
                                     continue;
                                 }
                                 const auto parsed = ningmail::MailtoParser::parse(url);
                                 if (!parsed.ok()) {
                                     qWarning() << "Ignoring unparseable mailto argument:" << raw
                                                << parsed.error.message;
                                     continue;
                                 }
                                 appController.openComposer(*parsed.value);
                             }
                         });
    }
    return app.exec();
}

} // namespace

int main(int argc, char *argv[]) {
    ningmail::registerMetaTypes();

    // Parsed without any QGuiApplication so --version also works on headless systems.
    bool smokeTest = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];
        if (argument == "--version") {
            std::cout << "NingMail " << ningmail::version().toStdString() << std::endl;
            return 0;
        }
        if (argument == "--smoke-test") {
            smokeTest = true;
        }
    }
    return runApplication(argc, argv, smokeTest);
}
