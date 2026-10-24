#include "SyncService.h"

#include "FakeMailBackend.h"
#include "MailBackend.h"
#include "MemoryCredentialStore.h"
#include "Types.h"

#include <QSemaphore>
#include <QSignalSpy>
#include <QTest>
#include <QtConcurrent/QtConcurrent>

#include <catch2/catch_test_macros.hpp>

#include <memory>

namespace {

ningmail::Account makeSyncAccount(qint64 accountId, const QString &credentialRef) {
    ningmail::Account account;
    account.id = accountId;
    account.email = QString::fromUtf8("user%1@ningmail.test").arg(accountId);
    account.imapHost = QString::fromUtf8("imap.ningmail.test");
    account.username = account.email;
    account.credentialRef = credentialRef;
    return account;
}

// Strings recorded by QueuedReceiver, one entry per queued signal delivery.
const char *const kStartedEvent = "started %1";
const char *const kFoldersEvent = "folders %1:%2";
const char *const kHeadersEvent = "headers %1:%2:%3";
const char *const kProgressEvent = "progress %1/%2";
const char *const kFinishedEvent = "finished %1:%2";

class QueuedReceiver final : public QObject {
    Q_OBJECT
  public:
    QStringList events;

    // Connected with Qt::QueuedConnection, so every slot below runs on the
    // thread that owns this receiver (the test thread), proving the signal
    // arguments survive cross-thread marshalling.
    void onSyncStarted(qint64 accountId) {
        events.append(QString::fromUtf8(kStartedEvent).arg(accountId));
    }

    void onFoldersFetched(qint64 accountId, const QList<ningmail::Folder> &folders) {
        events.append(QString::fromUtf8(kFoldersEvent).arg(accountId).arg(folders.size()));
    }

    void onHeadersFetched(qint64 accountId, const QString &folderFullName,
                          const QList<ningmail::MessageHeader> &headers) {
        events.append(QString::fromUtf8(kHeadersEvent)
                          .arg(accountId)
                          .arg(folderFullName)
                          .arg(headers.size()));
    }

    void onSyncProgress(qint64 accountId, int done, int total) {
        Q_UNUSED(accountId);
        events.append(QString::fromUtf8(kProgressEvent).arg(done).arg(total));
    }

    void onSyncFinished(qint64 accountId, const ningmail::MailError &error) {
        events.append(
            QString::fromUtf8(kFinishedEvent).arg(accountId).arg(static_cast<int>(error.code)));
    }
};

// Backend whose listFolders() blocks until the test releases it; used to
// pin the worker at a known phase for stop()/supersede behaviour tests.
class BlockingBackend final : public ningmail::MailBackend {
  public:
    BlockingBackend(const ningmail::Account &account, qint64 blockingAccountId,
                    std::shared_ptr<QSemaphore> entered, std::shared_ptr<QSemaphore> proceed)
        : m_accountId(account.id), m_blockingAccountId(blockingAccountId),
          m_entered(std::move(entered)), m_proceed(std::move(proceed)) {
    }

    ningmail::MailError connect(const ningmail::Account &, const QString &secret) override {
        if (secret != QString::fromUtf8("secret")) {
            return ningmail::MailError{ningmail::ErrorCode::Auth,
                                       QString::fromUtf8("Invalid credentials"),
                                       QString::fromUtf8("test backend")};
        }
        return {};
    }

    ningmail::Result<QList<ningmail::Folder>> listFolders() override {
        if (m_accountId == m_blockingAccountId) {
            m_entered->release();
            m_proceed->acquire();
        }
        ningmail::Folder inbox;
        inbox.id = 1;
        inbox.accountId = m_accountId;
        inbox.fullName = QString::fromUtf8("INBOX");
        inbox.displayName = QString::fromUtf8("Inbox");
        inbox.role = ningmail::FolderRole::Inbox;
        return ningmail::Result<QList<ningmail::Folder>>::success({inbox});
    }

    ningmail::Result<QList<ningmail::MessageHeader>>
    fetchHeaders(const ningmail::Folder &folder, quint32 fromUid, int limit) override {
        if (limit <= 0) {
            return ningmail::Result<QList<ningmail::MessageHeader>>::success({});
        }
        ningmail::MessageHeader header;
        header.id = 10 * m_accountId + static_cast<qint64>(fromUid);
        header.accountId = m_accountId;
        header.folderId = folder.id;
        header.uid = fromUid;
        header.subject = QString::fromUtf8("Test message %1").arg(fromUid);
        return ningmail::Result<QList<ningmail::MessageHeader>>::success({header});
    }

    ningmail::Result<ningmail::MessageBody>
    fetchBody(const ningmail::MessageHeader &header) override {
        ningmail::MessageBody body;
        body.messageId = header.id;
        body.plainText = QString::fromUtf8("Test body for %1").arg(header.id);
        return ningmail::Result<ningmail::MessageBody>::success(body);
    }

    ningmail::MailError send(const ningmail::OutgoingMessage &) override {
        return ningmail::MailError{ningmail::ErrorCode::Unknown, QString::fromUtf8("not supported"),
                                   QString::fromUtf8("test backend")};
    }

    ningmail::MailError appendSent(const ningmail::OutgoingMessage &, const QByteArray &) override {
        return ningmail::MailError{ningmail::ErrorCode::Unknown, QString::fromUtf8("not supported"),
                                   QString::fromUtf8("test backend")};
    }

    void disconnect() override {
    }

  private:
    qint64 m_accountId;
    qint64 m_blockingAccountId;
    std::shared_ptr<QSemaphore> m_entered;
    std::shared_ptr<QSemaphore> m_proceed;
};

class BlockingBackendFactory final : public ningmail::MailBackendFactory {
  public:
    BlockingBackendFactory(qint64 blockingAccountId, std::shared_ptr<QSemaphore> entered,
                           std::shared_ptr<QSemaphore> proceed)
        : m_blockingAccountId(blockingAccountId), m_entered(std::move(entered)),
          m_proceed(std::move(proceed)) {
    }

    std::unique_ptr<ningmail::MailBackend> create(const ningmail::Account &account) override {
        return std::make_unique<BlockingBackend>(account, m_blockingAccountId, m_entered,
                                                 m_proceed);
    }

  private:
    qint64 m_blockingAccountId;
    std::shared_ptr<QSemaphore> m_entered;
    std::shared_ptr<QSemaphore> m_proceed;
};

} // namespace

TEST_CASE("SyncService runs the full account pipeline", "[sync-service]") {
    ningmail::registerMetaTypes();
    ningmail::FakeMailBackendFactory backendFactory;
    ningmail::MemoryCredentialStore credentials;
    REQUIRE(credentials.write(QString::fromUtf8("ref-7"), QString::fromUtf8("secret")));
    const ningmail::Account account = makeSyncAccount(7, QString::fromUtf8("ref-7"));

    ningmail::SyncService service(&backendFactory, &credentials);
    QueuedReceiver receiver;
    QObject::connect(&service, &ningmail::SyncService::syncStarted, &receiver,
                     &QueuedReceiver::onSyncStarted, Qt::QueuedConnection);
    QObject::connect(&service, &ningmail::SyncService::foldersFetched, &receiver,
                     &QueuedReceiver::onFoldersFetched, Qt::QueuedConnection);
    QObject::connect(&service, &ningmail::SyncService::headersFetched, &receiver,
                     &QueuedReceiver::onHeadersFetched, Qt::QueuedConnection);
    QObject::connect(&service, &ningmail::SyncService::syncProgress, &receiver,
                     &QueuedReceiver::onSyncProgress, Qt::QueuedConnection);
    QObject::connect(&service, &ningmail::SyncService::syncFinished, &receiver,
                     &QueuedReceiver::onSyncFinished, Qt::QueuedConnection);

    QSignalSpy started(&service, &ningmail::SyncService::syncStarted);
    QSignalSpy folders(&service, &ningmail::SyncService::foldersFetched);
    QSignalSpy headers(&service, &ningmail::SyncService::headersFetched);
    QSignalSpy progress(&service, &ningmail::SyncService::syncProgress);
    QSignalSpy finished(&service, &ningmail::SyncService::syncFinished);
    QSignalSpy bodies(&service, &ningmail::SyncService::bodyFetched);

    service.syncAccount(account);

    QTRY_COMPARE(finished.count(), 1);
    REQUIRE(started.count() == 1);
    REQUIRE(started.at(0).at(0).value<qint64>() == 7);
    REQUIRE(folders.count() == 1);
    REQUIRE(folders.at(0).at(0).value<qint64>() == 7);
    REQUIRE(folders.at(0).at(1).value<QList<ningmail::Folder>>().size() == 2);
    REQUIRE(headers.count() == 1);
    REQUIRE(headers.at(0).at(0).value<qint64>() == 7);
    REQUIRE(headers.at(0).at(1).value<QString>() == QString::fromUtf8("INBOX"));
    REQUIRE(headers.at(0).at(2).value<QList<ningmail::MessageHeader>>().size() == 250);
    REQUIRE(headers.at(0).at(2).value<QList<ningmail::MessageHeader>>().first().uid == 1);
    REQUIRE(progress.count() == 2);
    REQUIRE(progress.at(0).at(1).value<int>() == 0);
    REQUIRE(progress.at(0).at(2).value<int>() == 1);
    REQUIRE(progress.at(1).at(1).value<int>() == 1);
    REQUIRE(progress.at(1).at(2).value<int>() == 1);
    REQUIRE(finished.at(0).at(0).value<qint64>() == 7);
    REQUIRE(finished.at(0).at(1).value<ningmail::MailError>().code == ningmail::ErrorCode::None);
    REQUIRE(bodies.count() == 0);

    // Cross-thread queued delivery: every slot below runs on the test thread.
    QTRY_COMPARE(receiver.events.size(), 6);
    QStringList expected;
    expected.append(QString::fromUtf8(kStartedEvent).arg(7));
    expected.append(QString::fromUtf8(kProgressEvent).arg(0).arg(1));
    expected.append(QString::fromUtf8(kFoldersEvent).arg(7).arg(2));
    expected.append(
        QString::fromUtf8(kHeadersEvent).arg(7).arg(QString::fromUtf8("INBOX")).arg(250));
    expected.append(QString::fromUtf8(kProgressEvent).arg(1).arg(1));
    expected.append(
        QString::fromUtf8(kFinishedEvent).arg(7).arg(static_cast<int>(ningmail::ErrorCode::None)));
    REQUIRE(receiver.events == expected);

    QTest::qWait(50); // observe queued leftovers and Qt thread warnings before destruction
}

TEST_CASE("SyncService fetches a message body in the background", "[sync-service]") {
    ningmail::registerMetaTypes();
    ningmail::FakeMailBackendFactory backendFactory;
    ningmail::MemoryCredentialStore credentials;
    REQUIRE(credentials.write(QString::fromUtf8("ref-8"), QString::fromUtf8("secret")));
    const ningmail::Account account = makeSyncAccount(8, QString::fromUtf8("ref-8"));

    ningmail::SyncService service(&backendFactory, &credentials);
    QSignalSpy bodies(&service, &ningmail::SyncService::bodyFetched);

    ningmail::MessageHeader header;
    header.id = 1;
    header.accountId = 8;
    header.uid = 1;
    service.fetchBody(account, header);

    QTRY_COMPARE(bodies.count(), 1);
    REQUIRE(bodies.at(0).at(0).value<qint64>() == header.id);
    const ningmail::MessageBody body = bodies.at(0).at(1).value<ningmail::MessageBody>();
    REQUIRE(body.messageId == header.id);
    REQUIRE(body.plainText == QString::fromUtf8("Seed body for uid 1"));
    REQUIRE(bodies.at(0).at(2).value<ningmail::MailError>().code == ningmail::ErrorCode::None);
}

TEST_CASE("SyncService fetchBody without credentials reports auth error", "[sync-service]") {
    ningmail::registerMetaTypes();
    ningmail::FakeMailBackendFactory backendFactory;
    ningmail::MemoryCredentialStore credentials;
    const ningmail::Account account = makeSyncAccount(8, QString::fromUtf8("ref-missing"));

    ningmail::SyncService service(&backendFactory, &credentials);
    QSignalSpy bodies(&service, &ningmail::SyncService::bodyFetched);

    ningmail::MessageHeader header;
    header.id = 1;
    service.fetchBody(account, header);

    QTRY_COMPARE(bodies.count(), 1);
    REQUIRE(bodies.at(0).at(0).value<qint64>() == header.id);
    REQUIRE(bodies.at(0).at(1).value<ningmail::MessageBody>().messageId == 0);
    REQUIRE(bodies.at(0).at(2).value<ningmail::MailError>().code == ningmail::ErrorCode::Auth);
}

TEST_CASE("SyncService without a stored credential finishes with auth error", "[sync-service]") {
    ningmail::registerMetaTypes();
    ningmail::FakeMailBackendFactory backendFactory;
    ningmail::MemoryCredentialStore credentials;
    const ningmail::Account account = makeSyncAccount(9, QString::fromUtf8("ref-missing"));

    ningmail::SyncService service(&backendFactory, &credentials);
    QSignalSpy started(&service, &ningmail::SyncService::syncStarted);
    QSignalSpy progress(&service, &ningmail::SyncService::syncProgress);
    QSignalSpy folders(&service, &ningmail::SyncService::foldersFetched);
    QSignalSpy headers(&service, &ningmail::SyncService::headersFetched);
    QSignalSpy finished(&service, &ningmail::SyncService::syncFinished);

    service.syncAccount(account);

    // Brief step 68: no syncStarted/syncProgress without credentials.
    QTRY_COMPARE(finished.count(), 1);
    REQUIRE(started.count() == 0);
    REQUIRE(progress.count() == 0);
    REQUIRE(folders.count() == 0);
    REQUIRE(headers.count() == 0);
    const ningmail::MailError error = finished.at(0).at(1).value<ningmail::MailError>();
    REQUIRE(error.code == ningmail::ErrorCode::Auth);
}

TEST_CASE("SyncService reports backend connect failure", "[sync-service]") {
    ningmail::registerMetaTypes();
    ningmail::FakeMailBackendFactory backendFactory;
    ningmail::MemoryCredentialStore credentials;
    REQUIRE(credentials.write(QString::fromUtf8("ref-10"), QString::fromUtf8("wrong")));
    const ningmail::Account account = makeSyncAccount(10, QString::fromUtf8("ref-10"));

    ningmail::SyncService service(&backendFactory, &credentials);
    QSignalSpy started(&service, &ningmail::SyncService::syncStarted);
    QSignalSpy folders(&service, &ningmail::SyncService::foldersFetched);
    QSignalSpy finished(&service, &ningmail::SyncService::syncFinished);

    service.syncAccount(account);

    QTRY_COMPARE(finished.count(), 1);
    REQUIRE(started.count() == 1);
    REQUIRE(folders.count() == 0);
    const ningmail::MailError error = finished.at(0).at(1).value<ningmail::MailError>();
    REQUIRE(error.code == ningmail::ErrorCode::Auth);
}

TEST_CASE("SyncService reports protocol error when no inbox exists", "[sync-service]") {
    ningmail::registerMetaTypes();
    ningmail::FakeMailBackendFactory backendFactory;
    ningmail::MemoryCredentialStore credentials;
    REQUIRE(credentials.write(QString::fromUtf8("ref-11"), QString::fromUtf8("secret")));
    const ningmail::Account account = makeSyncAccount(11, QString::fromUtf8("ref-11"));

    // Pre-create the seed through the factory API and strip every INBOX-like
    // folder, so the backend reports folders but never an inbox.
    auto &seedFolders = backendFactory.seed(11).folders;
    auto isInboxLike = [](const ningmail::Folder &folder) {
        return folder.role == ningmail::FolderRole::Inbox ||
               folder.fullName.compare(QString::fromUtf8("INBOX"), Qt::CaseInsensitive) == 0;
    };
    seedFolders.erase(std::remove_if(seedFolders.begin(), seedFolders.end(), isInboxLike),
                      seedFolders.end());
    ningmail::Folder custom;
    custom.id = 3;
    custom.accountId = 11;
    custom.fullName = QString::fromUtf8("Projects");
    custom.role = ningmail::FolderRole::Custom;
    seedFolders.append(custom);
    REQUIRE(!std::any_of(seedFolders.begin(), seedFolders.end(), isInboxLike));

    ningmail::SyncService service(&backendFactory, &credentials);
    QSignalSpy folders(&service, &ningmail::SyncService::foldersFetched);
    QSignalSpy headers(&service, &ningmail::SyncService::headersFetched);
    QSignalSpy finished(&service, &ningmail::SyncService::syncFinished);

    service.syncAccount(account);

    QTRY_COMPARE(finished.count(), 1);
    REQUIRE(folders.count() == 1);
    REQUIRE(headers.count() == 0);
    const ningmail::MailError error = finished.at(0).at(1).value<ningmail::MailError>();
    REQUIRE(error.code == ningmail::ErrorCode::Protocol);
    REQUIRE(error.detail == QString::fromUtf8("no inbox"));
}

TEST_CASE("SyncService stop drops further signals within timeout", "[sync-service]") {
    ningmail::registerMetaTypes();
    auto entered = std::make_shared<QSemaphore>();
    auto proceed = std::make_shared<QSemaphore>();
    BlockingBackendFactory backendFactory(1, entered, proceed);
    ningmail::MemoryCredentialStore credentials;
    REQUIRE(credentials.write(QString::fromUtf8("ref-1"), QString::fromUtf8("secret")));
    const ningmail::Account account = makeSyncAccount(1, QString::fromUtf8("ref-1"));

    ningmail::SyncService service(&backendFactory, &credentials);
    QSignalSpy started(&service, &ningmail::SyncService::syncStarted);
    QSignalSpy folders(&service, &ningmail::SyncService::foldersFetched);
    QSignalSpy headers(&service, &ningmail::SyncService::headersFetched);
    QSignalSpy progress(&service, &ningmail::SyncService::syncProgress);
    QSignalSpy finished(&service, &ningmail::SyncService::syncFinished);

    service.syncAccount(account);

    // Pin the worker inside listFolders() so stop() races with real work.
    QTRY_COMPARE(entered->available(), 1);
    // Flush the queued deliveries of the pending signals (started, progress)
    // on the service thread, so the post-stop assertions are deterministic.
    QTest::qWait(50);
    service.stop();
    proceed->release();

    QTest::qWait(300);
    REQUIRE(started.count() == 1);
    REQUIRE(progress.count() == 1);
    REQUIRE(folders.count() == 0);
    REQUIRE(headers.count() == 0);
    REQUIRE(finished.count() == 0);

    QTest::qWait(50); // destroying the service must not hang or crash
}

TEST_CASE("SyncService supersedes a running sync with a new account", "[sync-service]") {
    ningmail::registerMetaTypes();
    auto entered = std::make_shared<QSemaphore>();
    auto proceed = std::make_shared<QSemaphore>();
    BlockingBackendFactory backendFactory(1, entered, proceed);
    ningmail::MemoryCredentialStore credentials;
    REQUIRE(credentials.write(QString::fromUtf8("ref-1"), QString::fromUtf8("secret")));
    REQUIRE(credentials.write(QString::fromUtf8("ref-2"), QString::fromUtf8("secret")));
    const ningmail::Account first = makeSyncAccount(1, QString::fromUtf8("ref-1"));
    const ningmail::Account second = makeSyncAccount(2, QString::fromUtf8("ref-2"));

    ningmail::SyncService service(&backendFactory, &credentials);
    QSignalSpy started(&service, &ningmail::SyncService::syncStarted);
    QSignalSpy folders(&service, &ningmail::SyncService::foldersFetched);
    QSignalSpy headers(&service, &ningmail::SyncService::headersFetched);
    QSignalSpy finished(&service, &ningmail::SyncService::syncFinished);

    service.syncAccount(first);
    QTRY_COMPARE(entered->available(), 1);
    // Flush the queued deliveries of the first sync's pending signals on the
    // service thread, so the supersede assertions are deterministic.
    QTest::qWait(50);

    // A new sync stops the old one (R48) and invalidates its results.
    service.syncAccount(second);
    proceed->release();

    QTRY_COMPARE(finished.count(), 1);
    REQUIRE(started.count() == 2);
    REQUIRE(started.at(0).at(0).value<qint64>() == 1);
    REQUIRE(started.at(1).at(0).value<qint64>() == 2);
    REQUIRE(folders.count() == 1);
    REQUIRE(folders.at(0).at(0).value<qint64>() == 2);
    REQUIRE(headers.count() == 1);
    REQUIRE(headers.at(0).at(0).value<qint64>() == 2);
    REQUIRE(finished.at(0).at(0).value<qint64>() == 2);
    REQUIRE(finished.at(0).at(1).value<ningmail::MailError>().code == ningmail::ErrorCode::None);

    QTest::qWait(50);
}

#include "test_sync_service.moc"
