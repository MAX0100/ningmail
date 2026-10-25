#include "AppController.h"

#include "FakeMailBackend.h"
#include "MailError.h"
#include "MemoryCredentialStore.h"
#include "Store/AccountStore.h"
#include "Store/Database.h"
#include "Store/DraftStore.h"
#include "Store/FolderStore.h"
#include "Store/MessageStore.h"
#include "SyncService.h"
#include "Types.h"
#include "models/FolderListModel.h"
#include "models/MessageListModel.h"

#include <QDateTime>
#include <QSignalSpy>
#include <QString>
#include <QTemporaryDir>
#include <QTest>
#include <QVariantMap>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <utility>

namespace {

/// Stores + service + controller over a schema-v1 database in a temp dir.
struct ControllerFixture {
    QTemporaryDir dir;
    std::unique_ptr<ningmail::Database> database;
    std::unique_ptr<ningmail::AccountStore> accountStore;
    std::unique_ptr<ningmail::FolderStore> folderStore;
    std::unique_ptr<ningmail::MessageStore> messageStore;
    std::unique_ptr<ningmail::MemoryCredentialStore> credentials;
    std::unique_ptr<ningmail::FakeMailBackendFactory> backendFactory;
    std::unique_ptr<ningmail::SyncService> syncService;
    std::unique_ptr<ningmail::DraftStore> draftStore;
    std::unique_ptr<ningmail::AppController> controller;
    ningmail::AccountId accountId = 0;
    QString credentialRef = QStringLiteral("ref-controller");

    ControllerFixture() {
        database = std::make_unique<ningmail::Database>(dir.filePath("test.db"));
        REQUIRE_FALSE(database->open());
        REQUIRE_FALSE(database->migrate());
        accountStore = std::make_unique<ningmail::AccountStore>(*database);
        folderStore = std::make_unique<ningmail::FolderStore>(*database);
        messageStore = std::make_unique<ningmail::MessageStore>(*database);
        draftStore = std::make_unique<ningmail::DraftStore>(*database);
        credentials = std::make_unique<ningmail::MemoryCredentialStore>();
        backendFactory = std::make_unique<ningmail::FakeMailBackendFactory>();
        syncService =
            std::make_unique<ningmail::SyncService>(backendFactory.get(), credentials.get());
        controller = std::make_unique<ningmail::AppController>(
            *accountStore, *folderStore, *messageStore, *syncService, *credentials);
    }

    /// R57 happy-path setup: one account, its secret, and a fake seed. Callers
    /// then drive controller->initialize()/refresh().
    void seedAccount() {
        ningmail::Account account;
        account.displayName = QStringLiteral("演示账号");
        account.email = QStringLiteral("demo@ningmail.local");
        account.imapHost = QStringLiteral("demo.ningmail.local");
        account.username = account.email;
        account.credentialRef = credentialRef;
        const auto created = accountStore->create(account);
        REQUIRE(created.ok());
        accountId = *created.value;
        REQUIRE(credentials->write(credentialRef, QStringLiteral("secret")));
        backendFactory->seed(accountId);
    }
};

/// A fixture that has already completed one successful refresh() round trip.
struct SyncedFixture {
    ControllerFixture fixture;
    QSignalSpy finished;

    explicit SyncedFixture()
        : fixture(), finished(&*fixture.syncService, &ningmail::SyncService::syncFinished) {
        fixture.seedAccount();
        fixture.controller->initialize();
        fixture.controller->refresh();
        QTRY_COMPARE(finished.count(), 1);
    }
};

ningmail::FolderId findFolderDbId(const ningmail::FolderStore &folderStore,
                                  ningmail::AccountId accountId, const QString &fullName) {
    const auto foldersList = folderStore.list(accountId);
    REQUIRE(foldersList.ok());
    for (const ningmail::Folder &folder : *foldersList.value) {
        if (folder.fullName == fullName) {
            return folder.id;
        }
    }
    return 0;
}

QList<QString> sortedKeys(const QVariantMap &map) {
    QList<QString> keys = map.keys();
    std::sort(keys.begin(), keys.end());
    return keys;
}

/// R60: the reader map keeps exactly these 8 keys, always.
QList<QString> readerKeySet() {
    return {QStringLiteral("date"),      QStringLiteral("error"),  QStringLiteral("from"),
            QStringLiteral("html"),      QStringLiteral("id"),     QStringLiteral("loading"),
            QStringLiteral("plainText"), QStringLiteral("subject")};
}

} // namespace

TEST_CASE("AppController initialize without accounts asks for setup", "[controller]") {
    ControllerFixture fixture;
    REQUIRE(fixture.accountStore->list().value->isEmpty());

    fixture.controller->initialize();

    CHECK(fixture.controller->folders()->rowCount() == 0);
    CHECK(fixture.controller->syncing() == false);
    CHECK(fixture.controller->statusText().contains(QStringLiteral("请先添加账号")));
}

TEST_CASE("AppController syncs the account into the local store", "[controller]") {
    ControllerFixture fixture;
    REQUIRE(fixture.accountStore->list().value->isEmpty());
    fixture.seedAccount();

    QSignalSpy finished(&*fixture.syncService, &ningmail::SyncService::syncFinished);
    QSignalSpy syncingChanged(&*fixture.controller, &ningmail::AppController::syncingChanged);

    fixture.controller->initialize();
    // R59: startup must never auto-sync.
    CHECK(finished.count() == 0);
    CHECK(fixture.controller->folders()->rowCount() == 0);

    fixture.controller->refresh();

    QTRY_COMPARE(finished.count(), 1);
    CHECK(finished.at(0).at(0).value<qint64>() == fixture.accountId);
    CHECK(finished.at(0).at(1).value<ningmail::MailError>().code == ningmail::ErrorCode::None);
    CHECK(fixture.controller->syncing() == false);
    CHECK(fixture.controller->statusText() == QStringLiteral("同步完成"));
    CHECK(syncingChanged.count() >= 2); // flipped on in syncStarted, off in syncFinished

    // R57: the folder model shows the two synced folders.
    CHECK(fixture.controller->folders()->rowCount() == 2);

    // R58 mapping: resolve the INBOX DB id through the folder store itself.
    const ningmail::FolderId inboxDbId =
        findFolderDbId(*fixture.folderStore, fixture.accountId, QStringLiteral("INBOX"));
    REQUIRE(inboxDbId > 0);
    const ningmail::FolderId sentDbId =
        findFolderDbId(*fixture.folderStore, fixture.accountId, QStringLiteral("Sent"));
    REQUIRE(sentDbId > 0);

    // Pagination contract (T9a): the first page only, then loadMore() to the end.
    fixture.controller->messages()->setFolder(inboxDbId);
    CHECK(fixture.controller->messages()->rowCount() ==
          ningmail::MessageListModel::PAGE_SIZE); // first page = 100 rows
    fixture.controller->messages()->loadMore();
    fixture.controller->messages()->loadMore();
    CHECK(fixture.controller->messages()->rowCount() == 250); // full folder now loaded
    CHECK_FALSE(fixture.controller->messages()->canFetchMore());

    // DB header sample: account_id/folder_id must be the real stored ids (R58);
    // the fake's internal folder id (1) never reaches messages.folder_id.
    const QVariant firstId = fixture.controller->messages()->data(
        fixture.controller->messages()->index(0, 0), ningmail::MessageListModel::IdRole);
    REQUIRE(firstId.isValid());
    const auto header = fixture.messageStore->header(firstId.toLongLong());
    REQUIRE(header.ok());
    CHECK(header.value->accountId == fixture.accountId);
    CHECK(header.value->folderId == inboxDbId);

    // Real row counts after syncFinished (accounts/folders/messages).
    CHECK(fixture.accountStore->list().value->size() == 1);
    CHECK(fixture.folderStore->list(fixture.accountId).value->size() == 2);
    CHECK(fixture.messageStore->count(inboxDbId) == 250);
    CHECK(fixture.messageStore->count(sentDbId) == 0);
}

TEST_CASE("AppController openMessage reports unknown messages", "[controller]") {
    SyncedFixture fixture;

    fixture.fixture.controller->openMessage(999999);

    const QVariantMap reader = fixture.fixture.controller->reader();
    CHECK(sortedKeys(reader) == readerKeySet());
    CHECK(reader.value(QStringLiteral("id")).toLongLong() == 999999);
    CHECK(reader.value(QStringLiteral("loading")).toBool() == false);
    CHECK(reader.value(QStringLiteral("error")).toString() == QStringLiteral("消息不存在"));
    CHECK(reader.value(QStringLiteral("subject")).toString().isEmpty());
    CHECK(reader.value(QStringLiteral("from")).toString().isEmpty());
    CHECK(reader.value(QStringLiteral("html")).toString().isEmpty());
    CHECK(reader.value(QStringLiteral("plainText")).toString().isEmpty());
}

TEST_CASE("AppController fetches and stores the body on openMessage", "[controller]") {
    SyncedFixture fixture;

    const ningmail::FolderId inboxDbId = findFolderDbId(
        *fixture.fixture.folderStore, fixture.fixture.accountId, QStringLiteral("INBOX"));
    fixture.fixture.controller->selectFolder(inboxDbId);
    const QVariant firstId = fixture.fixture.controller->messages()->data(
        fixture.fixture.controller->messages()->index(0, 0), ningmail::MessageListModel::IdRole);

    QSignalSpy bodyFetched(&*fixture.fixture.syncService, &ningmail::SyncService::bodyFetched);
    fixture.fixture.controller->openMessage(firstId.toLongLong());
    QTRY_COMPARE(bodyFetched.count(), 1);

    // SyncService::fetchBody delivered a body for the requested header id, and the
    // controller persisted it through MessageStore::saveBody.
    const auto body = fixture.fixture.messageStore->body(firstId.toLongLong());
    REQUIRE(body.ok());
    CHECK_FALSE(body.value->plainText.isEmpty());

    // bodyState moved to Ready on the stored header.
    const auto header = fixture.fixture.messageStore->header(firstId.toLongLong());
    REQUIRE(header.ok());
    CHECK(header.value->bodyState == ningmail::BodyState::Ready);

    // The reader left the loading state and shows the body content.
    const QVariantMap reader = fixture.fixture.controller->reader();
    CHECK(sortedKeys(reader) == readerKeySet());
    CHECK(reader.value(QStringLiteral("loading")).toBool() == false);
    CHECK(reader.value(QStringLiteral("error")).toString().isEmpty());
    // The fake backend delivers text/plain, so both views must be available.
    CHECK_FALSE(reader.value(QStringLiteral("html")).toString().isEmpty());
    CHECK_FALSE(reader.value(QStringLiteral("plainText")).toString().isEmpty());
}

TEST_CASE("AppController discards body results of messages no longer open", "[controller]") {
    SyncedFixture fixture;

    const ningmail::FolderId inboxDbId = findFolderDbId(
        *fixture.fixture.folderStore, fixture.fixture.accountId, QStringLiteral("INBOX"));
    fixture.fixture.controller->selectFolder(inboxDbId);
    const QVariant topId = fixture.fixture.controller->messages()->data(
        fixture.fixture.controller->messages()->index(0, 0), ningmail::MessageListModel::IdRole);
    const QVariant nextId = fixture.fixture.controller->messages()->data(
        fixture.fixture.controller->messages()->index(1, 0), ningmail::MessageListModel::IdRole);
    REQUIRE(topId.isValid());
    REQUIRE(nextId.isValid());
    REQUIRE(topId != nextId);

    QSignalSpy bodyFetched(&*fixture.fixture.syncService, &ningmail::SyncService::bodyFetched);

    // Race window the controller ruling pins (fix round 1): open one message
    // and immediately a second one, before the first body arrives. The second
    // fetchBody dispatch raises the SyncService epoch (R48), so exactly the
    // second message's delivery survives; the reader must end up filled from
    // that delivery alone, never from the raced-away message's path.
    fixture.fixture.controller->openMessage(topId.toLongLong());
    fixture.fixture.controller->openMessage(nextId.toLongLong());
    QTRY_COMPARE(bodyFetched.count(), 1);
    CHECK(bodyFetched.at(0).at(0).value<qint64>() == nextId.toLongLong());

    const QVariantMap racedReader = fixture.fixture.controller->reader();
    CHECK(racedReader.value(QStringLiteral("id")).toLongLong() == nextId.toLongLong());
    CHECK(racedReader.value(QStringLiteral("loading")).toBool() == false);
    CHECK(racedReader.value(QStringLiteral("error")).toString().isEmpty());
    CHECK_FALSE(racedReader.value(QStringLiteral("html")).toString().isEmpty());

    // The surviving delivery is recorded for its own message only. The
    // superseded fetch's body was never delivered, so no content of it can sit
    // behind the reader either (a missing body row reads back as empty — the
    // store documents that as empty content, not an error).
    const auto racedBody = fixture.fixture.messageStore->body(nextId.toLongLong());
    REQUIRE(racedBody.ok());
    CHECK_FALSE(racedBody.value->plainText.isEmpty());
    CHECK(fixture.fixture.messageStore->header(nextId.toLongLong()).value->bodyState ==
          ningmail::BodyState::Ready);
    const auto supersededBody = fixture.fixture.messageStore->body(topId.toLongLong());
    CHECK(supersededBody.ok());
    CHECK(supersededBody.value->plainText.isEmpty());

    // Guard window: launch a fetch for topId and move the reader elsewhere on
    // a path that dispatches no further fetch (an unknown message). When the
    // in-flight delivery arrives, the reader shows a different id: the store
    // path must still run unconditionally, while the reader must stay exactly
    // as the user left it — a removed visible guard would fill this reader.
    fixture.fixture.controller->openMessage(topId.toLongLong());
    fixture.fixture.controller->openMessage(999999);
    QTRY_COMPARE(bodyFetched.count(), 2);
    CHECK(bodyFetched.at(1).at(0).value<qint64>() == topId.toLongLong());

    const auto guardedBody = fixture.fixture.messageStore->body(topId.toLongLong());
    REQUIRE(guardedBody.ok());
    CHECK_FALSE(guardedBody.value->plainText.isEmpty());
    CHECK(fixture.fixture.messageStore->header(topId.toLongLong()).value->bodyState ==
          ningmail::BodyState::Ready);

    const QVariantMap guardedReader = fixture.fixture.controller->reader();
    CHECK(guardedReader.value(QStringLiteral("id")).toLongLong() == 999999);
    CHECK(guardedReader.value(QStringLiteral("loading")).toBool() == false);
    CHECK(guardedReader.value(QStringLiteral("error")).toString() == QStringLiteral("消息不存在"));
    CHECK(guardedReader.value(QStringLiteral("html")).toString().isEmpty());
    CHECK(guardedReader.value(QStringLiteral("plainText")).toString().isEmpty());

    // And the message the reader is actually waiting for still fills the
    // reader once its own delivery lands.
    const QVariant thirdId = fixture.fixture.controller->messages()->data(
        fixture.fixture.controller->messages()->index(2, 0), ningmail::MessageListModel::IdRole);
    REQUIRE(thirdId.isValid());
    fixture.fixture.controller->openMessage(thirdId.toLongLong());
    QTRY_COMPARE(bodyFetched.count(), 3);
    CHECK(bodyFetched.at(2).at(0).value<qint64>() == thirdId.toLongLong());

    const QVariantMap openedReader = fixture.fixture.controller->reader();
    CHECK(openedReader.value(QStringLiteral("id")).toLongLong() == thirdId.toLongLong());
    CHECK(openedReader.value(QStringLiteral("loading")).toBool() == false);
    CHECK(openedReader.value(QStringLiteral("error")).toString().isEmpty());
    CHECK_FALSE(openedReader.value(QStringLiteral("html")).toString().isEmpty());
}

TEST_CASE("AppController wraps plain text bodies into a pre block", "[controller]") {
    SyncedFixture fixture;

    const ningmail::FolderId inboxDbId = findFolderDbId(
        *fixture.fixture.folderStore, fixture.fixture.accountId, QStringLiteral("INBOX"));
    fixture.fixture.controller->selectFolder(inboxDbId);
    const QVariant firstId = fixture.fixture.controller->messages()->data(
        fixture.fixture.controller->messages()->index(0, 0), ningmail::MessageListModel::IdRole);

    QSignalSpy bodyFetched(&*fixture.fixture.syncService, &ningmail::SyncService::bodyFetched);
    fixture.fixture.controller->openMessage(firstId.toLongLong());
    QTRY_COMPARE(bodyFetched.count(), 1);

    const QVariantMap reader = fixture.fixture.controller->reader();
    // <pre style="white-space:pre-wrap"> keeps spacing and still wraps (R72).
    CHECK(reader.value(QStringLiteral("html"))
              .toString()
              .startsWith(QStringLiteral("<pre style=\"white-space:pre-wrap\">")));
}

TEST_CASE("AppController openMessage fills the reader header fields", "[controller]") {
    SyncedFixture fixture;

    const ningmail::FolderId inboxDbId = findFolderDbId(
        *fixture.fixture.folderStore, fixture.fixture.accountId, QStringLiteral("INBOX"));
    fixture.fixture.controller->selectFolder(inboxDbId);
    const QVariant firstId = fixture.fixture.controller->messages()->data(
        fixture.fixture.controller->messages()->index(0, 0), ningmail::MessageListModel::IdRole);

    fixture.fixture.controller->openMessage(firstId.toLongLong());

    const QVariantMap reader = fixture.fixture.controller->reader();
    CHECK(sortedKeys(reader) == readerKeySet()); // exactly 8 keys
    CHECK(reader.value(QStringLiteral("loading")).toBool() == true);
    CHECK(reader.value(QStringLiteral("id")).toLongLong() == firstId.toLongLong());
    CHECK(reader.value(QStringLiteral("subject")).toString() ==
          QStringLiteral("Seed message 250")); // newest-first: uid 250 on top
    CHECK(reader.value(QStringLiteral("from")).toString() == QStringLiteral("Seed Sender"));
    CHECK(reader.value(QStringLiteral("date")).canConvert<QDateTime>());
    CHECK(reader.value(QStringLiteral("error")).toString().isEmpty());
    CHECK(reader.value(QStringLiteral("html")).toString().isEmpty());
    CHECK(reader.value(QStringLiteral("plainText")).toString().isEmpty());
}

// Task 11: the R78 three-branch saveDraft contract. The store pointer itself
// is invisible to the QML layer; the branch order is no-account first, then
// the missing store, then success.
TEST_CASE("AppController saveDraft runs the three-branch contract", "[controller][draft]") {
    ControllerFixture fixture;

    // Branch 1: no account -> -1 plus the setup hint.
    CHECK(fixture.controller->saveDraft(-1, QStringList{"rong@example.com"}, QStringList{},
                                        QStringLiteral("Quarterly notes"),
                                        QStringLiteral("First line")) == -1);
    CHECK(fixture.controller->statusText() == QStringLiteral("请先添加账号"));

    fixture.seedAccount();
    fixture.controller->initialize();

    // Branch 2: no draft store injected -> -1 plus the feature hint.
    CHECK(fixture.controller->saveDraft(-1, QStringList{"rong@example.com"}, QStringList{},
                                        QStringLiteral("Quarterly notes"),
                                        QStringLiteral("First line")) == -1);
    CHECK(fixture.controller->statusText() == QStringLiteral("草稿保存未启用"));

    // Branch 3: wired store -> the new row id comes back as a positive value.
    fixture.controller->setDraftStore(fixture.draftStore.get());
    const QStringList to{QStringLiteral("rong@example.com"), QStringLiteral("plain@example.com")};
    const QStringList cc{QStringLiteral("cc@example.com")};
    const qint64 draftId = fixture.controller->saveDraft(
        -1, to, cc, QStringLiteral("Quarterly notes"), QStringLiteral("First line"));
    CHECK(draftId > 0);
    CHECK(fixture.controller->statusText() == QStringLiteral("草稿已保存"));

    const auto stored = fixture.draftStore->get(draftId);
    REQUIRE(stored.ok());
    CHECK(stored.value->accountId == fixture.accountId);
    CHECK(stored.value->subject == QStringLiteral("Quarterly notes"));
    REQUIRE(stored.value->recipients.size() == 2);
    CHECK(stored.value->recipients.at(0).name.isEmpty());
    CHECK(stored.value->recipients.at(0).address == QStringLiteral("rong@example.com"));
    REQUIRE(stored.value->cc.size() == 1);
    CHECK(stored.value->cc.at(0).address == QStringLiteral("cc@example.com"));

    // draftId > 0 updates the same row (QML writes the returned id back).
    const qint64 updatedId = fixture.controller->saveDraft(
        draftId, to, cc, QStringLiteral("Renamed"), QStringLiteral("Edited body"));
    CHECK(updatedId == draftId);
    const auto after = fixture.draftStore->get(draftId);
    REQUIRE(after.ok());
    CHECK(after.value->subject == QStringLiteral("Renamed"));
    CHECK(fixture.draftStore->list(fixture.accountId).value->size() == 1);
}

// Task 11: without a SendService (task 12) sending stays a guarded no-op.
TEST_CASE("AppController sendMessage reports the missing send service", "[controller][draft]") {
    ControllerFixture fixture;
    fixture.seedAccount();
    fixture.controller->initialize();

    fixture.controller->sendMessage(QStringList{"rong@example.com"}, QStringList{},
                                    QStringLiteral("Quarterly notes"),
                                    QStringLiteral("First line"));
    CHECK(fixture.controller->statusText() == QStringLiteral("发送服务未启用"));
}

// Task 11 fix round 1: saveDraft's store-failure branch must not be silent -
// statusText carries the failure in the same style as the sync paths.
TEST_CASE("AppController saveDraft reports store failures", "[controller][draft]") {
    ControllerFixture fixture;
    fixture.seedAccount();
    fixture.controller->initialize();

    // A database that was never opened makes every store call fail without
    // needing a poisoned fixture: transaction() fails first and save() maps
    // that to a Storage MailError through the regular error path.
    ningmail::Database broken(fixture.dir.filePath("unopened.db"));
    ningmail::DraftStore brokenStore(broken);
    fixture.controller->setDraftStore(&brokenStore);

    const qint64 result = fixture.controller->saveDraft(
        -1, QStringList{QStringLiteral("a@b.c")}, QStringList{}, QStringLiteral("Quarterly notes"),
        QStringLiteral("First line"));
    CHECK(result == -1);
    CHECK(fixture.controller->statusText().startsWith(QStringLiteral("草稿保存失败")));
}
