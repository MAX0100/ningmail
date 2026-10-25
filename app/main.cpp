#include "AppController.h"
#include "FakeMailBackend.h"
#include "MemoryCredentialStore.h"
#include "SendService.h"
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

#include <iostream>
#include <memory>
#include <string_view>

namespace {

int runApplication(int argc, char *argv[], bool smokeTest) {
    QGuiApplication app(argc, argv);

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
