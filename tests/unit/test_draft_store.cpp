#include "Store/AccountStore.h"
#include "Store/Database.h"
#include "Store/DraftStore.h"

#include <QSqlError>
#include <QSqlQuery>
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

ningmail::Draft sampleDraft() {
    ningmail::Draft draft;
    draft.subject = "Quarterly notes";
    draft.bodyText = "First line\nSecond line";
    draft.recipients.append(ningmail::MailAddress{"Rong Li", "rong@example.com"});
    draft.recipients.append(ningmail::MailAddress{QString(), "plain@example.com"});
    draft.cc.append(ningmail::MailAddress{"Loops", "cc@example.com"});
    return draft;
}

ningmail::Account sampleAccount() {
    ningmail::Account account;
    account.displayName = "Work";
    account.email = "work@example.com";
    account.imapHost = "imap.example.com";
    account.username = "work@example.com";
    account.credentialRef = "account/work";
    return account;
}

} // namespace

TEST_CASE("draft store saves a new draft and reads every field back", "[draft]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    auto database = migratedDatabase(dir);
    ningmail::AccountStore accountStore(*database);
    const auto account = accountStore.create(sampleAccount());
    REQUIRE(account.ok());
    ningmail::DraftStore store(*database);

    ningmail::Draft draft = sampleDraft();
    draft.accountId = *account.value;
    const auto saved = store.save(draft);
    REQUIRE(saved.ok());
    REQUIRE(*saved.value > 0);

    const auto fetched = store.get(*saved.value);
    REQUIRE(fetched.ok());
    REQUIRE(fetched.value->id == *saved.value);
    REQUIRE(fetched.value->accountId == *account.value);
    REQUIRE(fetched.value->subject == sampleDraft().subject);
    REQUIRE(fetched.value->bodyText == sampleDraft().bodyText);
    // Recipient and cc address lists must round-trip unchanged, including the
    // name-less address form and the empty cc distinction.
    REQUIRE(fetched.value->recipients.size() == 2);
    REQUIRE(fetched.value->recipients.at(0).name == "Rong Li");
    REQUIRE(fetched.value->recipients.at(0).address == "rong@example.com");
    REQUIRE(fetched.value->recipients.at(1).name.isEmpty());
    REQUIRE(fetched.value->recipients.at(1).address == "plain@example.com");
    REQUIRE(fetched.value->cc.size() == 1);
    REQUIRE(fetched.value->cc.at(0).name == "Loops");
    REQUIRE(fetched.value->cc.at(0).address == "cc@example.com");
    // save() must stamp updatedAt (R29/R78): now in UTC milliseconds, nonzero.
    REQUIRE(fetched.value->updatedAt > 0);
    // The stored format must match MessageStore's address JSON convention.
    QSqlQuery jsonProbe(database->connection());
    QString recipientsJson;
    QString ccJson;
    REQUIRE(
        jsonProbe.exec(QStringLiteral("SELECT recipients_json, cc_json FROM drafts WHERE id = %1")
                           .arg(*saved.value)));
    REQUIRE(jsonProbe.next());
    // QJsonDocument sorts object keys ("address" before "name"); the assertion
    // pins exactly the byte format MessageStore's helpers also emit.
    REQUIRE(jsonProbe.value(0).toString() ==
            QStringLiteral(R"([{"address":"rong@example.com","name":"Rong Li"},)"
                           R"({"address":"plain@example.com","name":""}])"));
    REQUIRE(jsonProbe.value(1).toString() ==
            QStringLiteral(R"([{"address":"cc@example.com","name":"Loops"}])"));
}

TEST_CASE("draft store updates an existing draft id in place", "[draft]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    auto database = migratedDatabase(dir);
    ningmail::AccountStore accountStore(*database);
    const auto account = accountStore.create(sampleAccount());
    REQUIRE(account.ok());
    ningmail::DraftStore store(*database);

    ningmail::Draft draft;
    draft.accountId = *account.value;
    draft.subject = "First version";
    draft.bodyText = "Original body";
    const auto created = store.save(draft);
    REQUIRE(created.ok());

    // Second save with the same id is an UPDATE that returns the same id.
    ningmail::Draft changed = draft;
    changed.id = *created.value;
    changed.subject = "Second version";
    changed.bodyText = "Edited body";
    changed.cc.append(ningmail::MailAddress{"Copied", "cc@example.com"});
    const auto updated = store.save(changed);
    REQUIRE(updated.ok());
    REQUIRE(*updated.value == *created.value);

    const auto fetched = store.get(*updated.value);
    REQUIRE(fetched.ok());
    REQUIRE(fetched.value->subject == "Second version");
    REQUIRE(fetched.value->bodyText == "Edited body");
    REQUIRE(fetched.value->cc.size() == 1);
    // The id<=0 branch must never have created a second row.
    const auto list = store.list(*account.value);
    REQUIRE(list.ok());
    REQUIRE(list.value->size() == 1);
}

TEST_CASE("draft store lists newest first with a stable id tie-break", "[draft]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    auto database = migratedDatabase(dir);
    ningmail::AccountStore accountStore(*database);
    const auto account = accountStore.create(sampleAccount());
    REQUIRE(account.ok());
    ningmail::DraftStore store(*database);

    ningmail::Draft draft;
    draft.accountId = *account.value;
    const auto created1 = store.save(draft);
    REQUIRE(created1.ok());
    // Saving "in the same millisecond" would make the tie-break the only
    // discriminator; equalize the timestamps explicitly so the SQL tie-break
    // (id DESC) is exercised deterministically.
    const auto created2 = store.save(draft);
    REQUIRE(created2.ok());
    REQUIRE(*created2.value != *created1.value);

    QSqlQuery equalizer(database->connection());
    REQUIRE(
        equalizer.exec(QStringLiteral("UPDATE drafts SET updated_at = 5000 WHERE account_id = %1")
                           .arg(*account.value)));

    const auto list = store.list(*account.value);
    REQUIRE(list.ok());
    REQUIRE(list.value->size() == 2);
    // Same updated_at: id DESC decides, so the later insert comes first.
    REQUIRE(list.value->at(0).id == *created2.value);
    REQUIRE(list.value->at(1).id == *created1.value);

    // Rows of other accounts must not leak in.
    const auto otherAccount = accountStore.create(sampleAccount());
    REQUIRE(otherAccount.ok());
    const auto emptyList = store.list(*otherAccount.value);
    REQUIRE(emptyList.ok());
    REQUIRE(emptyList.value->isEmpty());
}

TEST_CASE("draft store remove deletes the row and missing ids fail", "[draft]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    auto database = migratedDatabase(dir);
    ningmail::AccountStore accountStore(*database);
    const auto account = accountStore.create(sampleAccount());
    REQUIRE(account.ok());
    ningmail::DraftStore store(*database);

    ningmail::Draft draft;
    draft.accountId = *account.value;
    const auto created = store.save(draft);
    REQUIRE(created.ok());

    // R78: remove success is the default MailError{} (code None).
    REQUIRE(store.remove(*created.value).code == ningmail::ErrorCode::None);
    const auto removed = store.get(*created.value);
    REQUIRE_FALSE(removed.ok());
    REQUIRE(removed.error.code == ningmail::ErrorCode::Storage);

    // Unknown ids fail with a Storage error, both for reads and for removal.
    const auto unknown = store.get(12345);
    REQUIRE_FALSE(unknown.ok());
    REQUIRE(unknown.error.code == ningmail::ErrorCode::Storage);
    REQUIRE(store.remove(12345).code == ningmail::ErrorCode::Storage);
}

TEST_CASE("draft store rejects drafts of unknown accounts via the foreign key", "[draft]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    auto database = migratedDatabase(dir);
    ningmail::DraftStore store(*database);

    ningmail::Draft draft;
    draft.accountId = 98765; // no such account row
    const auto saved = store.save(draft);
    REQUIRE_FALSE(saved.ok());
    REQUIRE(saved.error.code == ningmail::ErrorCode::Storage);
}

TEST_CASE("draft store updates of missing rows fail instead of inserting", "[draft]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    auto database = migratedDatabase(dir);
    ningmail::AccountStore accountStore(*database);
    const auto account = accountStore.create(sampleAccount());
    REQUIRE(account.ok());
    ningmail::DraftStore store(*database);

    ningmail::Draft draft;
    draft.id = 4242; // no such row: the UPDATE branch must fail, not upsert
    draft.accountId = *account.value;
    const auto updated = store.save(draft);
    REQUIRE_FALSE(updated.ok());
    REQUIRE(updated.error.code == ningmail::ErrorCode::Storage);
}

// R80: draft ownership is fixed forever. A later save that arrives under a
// different account must fail instead of silently migrating the row, and the
// stored content must stay untouched.
TEST_CASE("draft store rejects cross-account updates and keeps the row", "[draft]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    auto database = migratedDatabase(dir);
    ningmail::AccountStore accountStore(*database);
    const auto owner = accountStore.create(sampleAccount());
    const auto other = accountStore.create(sampleAccount());
    REQUIRE(owner.ok());
    REQUIRE(other.ok());
    REQUIRE(*other.value != *owner.value);
    ningmail::DraftStore store(*database);

    ningmail::Draft draft;
    draft.accountId = *owner.value;
    draft.subject = "Owner's draft";
    draft.bodyText = "Original body";
    const auto created = store.save(draft);
    REQUIRE(created.ok());

    // A save arrives for the same row id but claims the other account. The
    // UPDATE must miss (ownership lives in the WHERE not the SET).
    ningmail::Draft hijack = draft;
    hijack.id = *created.value;
    hijack.accountId = *other.value;
    hijack.subject = "Stolen!";
    hijack.bodyText = "Reassigned body";
    const auto hijacked = store.save(hijack);
    REQUIRE_FALSE(hijacked.ok());
    REQUIRE(hijacked.error.code == ningmail::ErrorCode::Storage);

    // The row stayed with its original account and content.
    const auto row = store.get(*created.value);
    REQUIRE(row.ok());
    REQUIRE(row.value->accountId == *owner.value);
    REQUIRE(row.value->subject == "Owner's draft");
    REQUIRE(row.value->bodyText == "Original body");

    // And the owner can still update the very same row afterwards.
    ningmail::Draft update = draft;
    update.id = *created.value;
    update.subject = "Second version";
    const auto updated = store.save(update);
    REQUIRE(updated.ok());
    REQUIRE(*updated.value == *created.value);
    const auto after = store.get(*created.value);
    REQUIRE(after.ok());
    REQUIRE(after.value->subject == "Second version");
    REQUIRE(after.value->accountId == *owner.value);
}
