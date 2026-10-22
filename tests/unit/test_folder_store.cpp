#include "Store/AccountStore.h"
#include "Store/Database.h"
#include "Store/FolderStore.h"

#include <QTemporaryDir>
#include <QtGlobal>

#include <catch2/catch_test_macros.hpp>

#include <memory>

namespace {

/// Opens a schema v1 database in a temporary directory.
std::unique_ptr<ningmail::Database> migratedDatabase(const QTemporaryDir &dir) {
    auto db = std::make_unique<ningmail::Database>(dir.filePath("test.db"));
    REQUIRE_FALSE(db->open());
    REQUIRE_FALSE(db->migrate());
    return db;
}

ningmail::Folder sampleFolder(ningmail::AccountId accountId) {
    ningmail::Folder folder;
    folder.accountId = accountId;
    folder.fullName = "INBOX";
    folder.displayName = "INBOX";
    folder.role = ningmail::FolderRole::Inbox;
    folder.delimiter = u'/';
    folder.uidValidity = 42;
    folder.uidNext = 100;
    folder.highestModSeq = 7;
    return folder;
}

} // namespace

TEST_CASE("folder store upsert reuses id and keeps sync state", "[folder]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    auto database = migratedDatabase(dir);
    ningmail::AccountStore accounts(*database);
    ningmail::FolderStore folders(*database);

    ningmail::Account account;
    account.email = "work@example.com";
    const auto createdAccount = accounts.create(account);
    REQUIRE(createdAccount.ok());
    const ningmail::AccountId accountId = *createdAccount.value;

    const auto created = folders.upsert(sampleFolder(accountId));
    REQUIRE(created.ok());
    const ningmail::FolderId folderId = *created.value;

    // Same (account_id, full_name) with different values must update in place.
    ningmail::Folder changed = sampleFolder(accountId);
    changed.displayName = "Inbox renamed";
    changed.role = ningmail::FolderRole::Archive;
    changed.delimiter = u'.';
    changed.uidValidity = 999;
    changed.uidNext = 999;
    changed.highestModSeq = 999;

    const auto reUpserted = folders.upsert(changed);
    REQUIRE(reUpserted.ok());
    REQUIRE(*reUpserted.value == folderId);

    const auto list = folders.list(accountId);
    REQUIRE(list.ok());
    REQUIRE(list.value->size() == 1);
    const ningmail::Folder &stored = list.value->at(0);
    REQUIRE(stored.id == folderId);
    REQUIRE(stored.displayName == QStringLiteral("Inbox renamed"));
    REQUIRE(stored.role == ningmail::FolderRole::Archive);
    REQUIRE(stored.delimiter == u'.');
    // Sync state is only changed by updateSyncState().
    REQUIRE(stored.uidValidity == 42);
    REQUIRE(stored.uidNext == 100);
    REQUIRE(stored.highestModSeq == 7);

    REQUIRE_FALSE(folders.updateSyncState(folderId, 43, 101, 8));
    const auto afterSync = folders.list(accountId);
    REQUIRE(afterSync.ok());
    REQUIRE(afterSync.value->at(0).uidValidity == 43);
    REQUIRE(afterSync.value->at(0).uidNext == 101);
    REQUIRE(afterSync.value->at(0).highestModSeq == 8);
}

TEST_CASE("folder store lists per account ordered by id", "[folder]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    auto database = migratedDatabase(dir);
    ningmail::AccountStore accounts(*database);
    ningmail::FolderStore folders(*database);

    ningmail::Account account;
    account.email = "work@example.com";
    const auto createdWorkAccount = accounts.create(account);
    REQUIRE(createdWorkAccount.ok());

    ningmail::Account other = account;
    other.email = "home@example.com";
    const auto createdHomeAccount = accounts.create(other);
    REQUIRE(createdHomeAccount.ok());

    const ningmail::AccountId workAccountId = *createdWorkAccount.value;
    const ningmail::AccountId homeAccountId = *createdHomeAccount.value;

    ningmail::Folder inbox = sampleFolder(workAccountId);
    const auto createdInbox = folders.upsert(inbox);
    REQUIRE(createdInbox.ok());

    ningmail::Folder sent = inbox;
    sent.fullName = "Sent";
    sent.displayName = "Sent";
    sent.role = ningmail::FolderRole::Sent;
    const auto createdSent = folders.upsert(sent);
    REQUIRE(createdSent.ok());

    ningmail::Folder homeInbox = inbox;
    homeInbox.accountId = homeAccountId;
    REQUIRE(folders.upsert(homeInbox).ok());

    const auto list = folders.list(workAccountId);
    REQUIRE(list.ok());
    REQUIRE(list.value->size() == 2);
    REQUIRE(list.value->at(0).id == *createdInbox.value);
    REQUIRE(list.value->at(0).fullName == QStringLiteral("INBOX"));
    REQUIRE(list.value->at(1).id == *createdSent.value);
    REQUIRE(list.value->at(1).fullName == QStringLiteral("Sent"));
    REQUIRE(folders.list(homeAccountId).value->size() == 1);
}

TEST_CASE("deleting an account cascades to its folders", "[folder]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    auto database = migratedDatabase(dir);
    ningmail::AccountStore accounts(*database);
    ningmail::FolderStore folders(*database);

    ningmail::Account account;
    account.email = "work@example.com";
    const auto createdAccount = accounts.create(account);
    REQUIRE(createdAccount.ok());
    const ningmail::AccountId accountId = *createdAccount.value;

    REQUIRE(folders.upsert(sampleFolder(accountId)).ok());
    REQUIRE(folders.list(accountId).value->size() == 1);

    REQUIRE_FALSE(accounts.remove(accountId));
    const auto afterDelete = folders.list(accountId);
    REQUIRE(afterDelete.ok());
    REQUIRE(afterDelete.value->isEmpty());
}

TEST_CASE("folder store updateSyncState on a missing folder fails", "[folder]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    auto database = migratedDatabase(dir);
    ningmail::FolderStore folders(*database);

    REQUIRE(folders.updateSyncState(12345, 1, 2, 3).code == ningmail::ErrorCode::Storage);
}
