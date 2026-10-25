#include "SendService.h"
#include "Store/AccountStore.h"
#include "Store/Database.h"
#include "Store/OutboxStore.h"

#include "FakeMailBackend.h"
#include "MailBackend.h"
#include "MailError.h"
#include "MemoryCredentialStore.h"
#include "MimeParser.h"
#include "Types.h"

#include <QSignalSpy>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>

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

ningmail::Account sampleAccount() {
    ningmail::Account account;
    account.displayName = "Writer";
    account.email = "writer@example.com";
    account.imapHost = "imap.example.com";
    account.username = "writer@example.com";
    account.credentialRef = "account/writer";
    return account;
}

/// A pending item that mirrors what SendService::enqueue would store.
ningmail::OutboxItem sampleItem(ningmail::AccountId accountId) {
    ningmail::OutboxItem item;
    item.accountId = accountId;
    item.recipients.append(ningmail::MailAddress{"Rong Li", "rong@example.com"});
    item.recipients.append(ningmail::MailAddress{QString(), "plain@example.com"});
    item.cc.append(ningmail::MailAddress{"Loops", "cc@example.com"});
    item.subject = "Quarterly notes";
    item.bodyText = "First line\nSecond line";
    item.rfcMessageId = "<generated@test>";
    item.rawMime = QByteArray("RAW \x01 MIME \r\n BYTES");
    return item;
}

/// Stores + backend factory + credentials + outbox + one SendService over them.
struct SendFixture {
    QTemporaryDir dir;
    std::unique_ptr<ningmail::Database> database;
    std::unique_ptr<ningmail::AccountStore> accountStore;
    std::unique_ptr<ningmail::MemoryCredentialStore> credentials;
    std::unique_ptr<ningmail::FakeMailBackendFactory> factory;
    std::unique_ptr<ningmail::OutboxStore> outbox;
    std::unique_ptr<ningmail::SendService> service;
    ningmail::AccountId accountId = 0;

    SendFixture()
        : database(migratedDatabase(dir)),
          accountStore(std::make_unique<ningmail::AccountStore>(*database)),
          credentials(std::make_unique<ningmail::MemoryCredentialStore>()),
          factory(std::make_unique<ningmail::FakeMailBackendFactory>()),
          outbox(std::make_unique<ningmail::OutboxStore>(*database)),
          service(std::make_unique<ningmail::SendService>(*outbox, *factory, *credentials)) {
        auto account = sampleAccount();
        const auto created = accountStore->create(account);
        REQUIRE(created.ok());
        accountId = *created.value;
        REQUIRE(credentials->write("account/writer", "secret"));
    }

    /// An OutgoingMessage addressed as the login account would compose it.
    ningmail::OutgoingMessage message() {
        const auto account = accountStore->get(accountId);
        REQUIRE(account.ok());
        ningmail::OutgoingMessage message;
        message.account = *account.value;
        message.to.append(ningmail::MailAddress{QString::fromUtf8("Rong Li"),
                                                QString::fromUtf8("rong@example.com")});
        message.cc.append(ningmail::MailAddress{QString(), QString::fromUtf8("cc@example.com")});
        message.subject = QString::fromUtf8("季度汇报");
        message.bodyText = QString::fromUtf8("第一行\n第二行");
        return message;
    }
};

} // namespace

TEST_CASE("outbox store saves a pending item and reads every field back", "[outbox]") {
    QTemporaryDir dir;
    auto database = migratedDatabase(dir);
    ningmail::AccountStore accountStore(*database);
    const auto account = accountStore.create(sampleAccount());
    REQUIRE(account.ok());
    ningmail::OutboxStore store(*database);

    ningmail::OutboxItem item = sampleItem(*account.value);
    const auto saved = store.save(item);
    REQUIRE(saved.ok());
    REQUIRE(*saved.value > 0);

    const auto fetched = store.get(*saved.value);
    REQUIRE(fetched.ok());
    REQUIRE(fetched.value->id == *saved.value);
    REQUIRE(fetched.value->accountId == *account.value);
    REQUIRE(fetched.value->subject == "Quarterly notes");
    REQUIRE(fetched.value->bodyText == "First line\nSecond line");
    REQUIRE(fetched.value->recipients.size() == 2);
    REQUIRE(fetched.value->recipients.at(0).name == "Rong Li");
    REQUIRE(fetched.value->recipients.at(1).address == "plain@example.com");
    REQUIRE(fetched.value->cc.size() == 1);
    REQUIRE(fetched.value->cc.at(0).address == "cc@example.com");
    // New rows start in Pending with no attempt and no retry schedule.
    REQUIRE(fetched.value->state == ningmail::OutboxState::Pending);
    REQUIRE(fetched.value->attempts == 0);
    REQUIRE_FALSE(fetched.value->nextRetryAt.isValid());
    REQUIRE(fetched.value->lastError.isEmpty());
    REQUIRE(fetched.value->rfcMessageId == "<generated@test>");
    // raw_mime is a BLOB: bytes must survive verbatim, including control bytes.
    REQUIRE(fetched.value->rawMime == QByteArray("RAW \x01 MIME \r\n BYTES"));
}

TEST_CASE("outbox store persists rows across a reopened database", "[outbox]") {
    QTemporaryDir dir;
    qint64 savedId = 0;
    // "Reboot": tear the first database down before the second one opens.
    {
        auto first = migratedDatabase(dir);
        ningmail::AccountStore accountStore(*first);
        const auto account = accountStore.create(sampleAccount());
        REQUIRE(account.ok());
        ningmail::OutboxStore store(*first);
        const auto saved = store.save(sampleItem(*account.value));
        REQUIRE(saved.ok());
        savedId = *saved.value;
    }

    auto reopened = migratedDatabase(dir);
    ningmail::OutboxStore store(*reopened);
    const auto fetched = store.get(savedId);
    REQUIRE(fetched.ok());
    REQUIRE(fetched.value->subject == "Quarterly notes");
    REQUIRE(fetched.value->rawMime == QByteArray("RAW \x01 MIME \r\n BYTES"));
}

TEST_CASE("outbox store updateState writes the final state and clears an empty error", "[outbox]") {
    QTemporaryDir dir;
    auto database = migratedDatabase(dir);
    ningmail::AccountStore accountStore(*database);
    const auto account = accountStore.create(sampleAccount());
    REQUIRE(account.ok());
    ningmail::OutboxStore store(*database);
    const auto saved = store.save(sampleItem(*account.value));
    REQUIRE(saved.ok());

    // The error parameter is the final value: a non-empty string replaces the
    // column, an empty string clears it.
    REQUIRE(store
                .updateState(*saved.value, ningmail::OutboxState::Retry,
                             QStringLiteral("Simulated network failure"))
                .code == ningmail::ErrorCode::None);
    auto after = store.get(*saved.value);
    REQUIRE(after.ok());
    REQUIRE(after.value->state == ningmail::OutboxState::Retry);
    REQUIRE(after.value->lastError == QStringLiteral("Simulated network failure"));

    REQUIRE(store.updateState(*saved.value, ningmail::OutboxState::Pending, QString()).code ==
            ningmail::ErrorCode::None);
    after = store.get(*saved.value);
    REQUIRE(after.ok());
    REQUIRE(after.value->state == ningmail::OutboxState::Pending);
    REQUIRE(after.value->lastError.isEmpty());

    // Unknown ids fail with a Storage error.
    REQUIRE(store.updateState(424242, ningmail::OutboxState::Sent).code ==
            ningmail::ErrorCode::Storage);

    // updateState never touches attempts.
    REQUIRE(after.value->attempts == 0);
}

TEST_CASE("outbox store bumpAttempts increments and missing ids fail", "[outbox]") {
    QTemporaryDir dir;
    auto database = migratedDatabase(dir);
    ningmail::AccountStore accountStore(*database);
    const auto account = accountStore.create(sampleAccount());
    REQUIRE(account.ok());
    ningmail::OutboxStore store(*database);
    const auto saved = store.save(sampleItem(*account.value));
    REQUIRE(saved.ok());

    REQUIRE(store.bumpAttempts(*saved.value).code == ningmail::ErrorCode::None);
    REQUIRE(store.bumpAttempts(*saved.value).code == ningmail::ErrorCode::None);
    const auto fetched = store.get(*saved.value);
    REQUIRE(fetched.ok());
    REQUIRE(fetched.value->attempts == 2);

    REQUIRE(store.bumpAttempts(12345).code == ningmail::ErrorCode::Storage);
    REQUIRE(store.updateState(12345, ningmail::OutboxState::Sent).code ==
            ningmail::ErrorCode::Storage);
}

TEST_CASE("outbox store remove deletes the row and missing ids fail", "[outbox]") {
    QTemporaryDir dir;
    auto database = migratedDatabase(dir);
    ningmail::AccountStore accountStore(*database);
    const auto account = accountStore.create(sampleAccount());
    REQUIRE(account.ok());
    ningmail::OutboxStore store(*database);
    const auto saved = store.save(sampleItem(*account.value));
    REQUIRE(saved.ok());

    REQUIRE(store.remove(*saved.value).code == ningmail::ErrorCode::None);
    REQUIRE_FALSE(store.get(*saved.value).ok());
    REQUIRE(store.get(*saved.value).error.code == ningmail::ErrorCode::Storage);
    REQUIRE(store.remove(*saved.value).code == ningmail::ErrorCode::Storage);
}

TEST_CASE("outbox store rejects items of unknown accounts via the foreign key", "[outbox]") {
    QTemporaryDir dir;
    auto database = migratedDatabase(dir);
    ningmail::OutboxStore store(*database);

    const auto saved = store.save(sampleItem(98765)); // no such account row
    REQUIRE_FALSE(saved.ok());
    REQUIRE(saved.error.code == ningmail::ErrorCode::Storage);
}

TEST_CASE("outbox store listPending returns Pending and Retry rows in FIFO order", "[outbox]") {
    QTemporaryDir dir;
    auto database = migratedDatabase(dir);
    ningmail::AccountStore accountStore(*database);
    const auto account = accountStore.create(sampleAccount());
    REQUIRE(account.ok());
    ningmail::OutboxStore store(*database);

    const ningmail::AccountId accountId = *account.value;
    qint64 pendingId = 0;
    qint64 retryId = 0;
    {
        ningmail::OutboxItem pending = sampleItem(accountId);
        pending.subject = "first";
        const auto saved = store.save(pending);
        REQUIRE(saved.ok());
        pendingId = *saved.value;

        ningmail::OutboxItem sent = pending;
        sent.subject = "second";
        const auto sentRow = store.save(sent);
        REQUIRE(sentRow.ok());
        REQUIRE(store.updateState(*sentRow.value, ningmail::OutboxState::Sent).code ==
                ningmail::ErrorCode::None);

        ningmail::OutboxItem retry = pending;
        retry.subject = "third";
        const auto retryRow = store.save(retry);
        REQUIRE(retryRow.ok());
        REQUIRE(store
                    .updateState(*retryRow.value, ningmail::OutboxState::Retry,
                                 QStringLiteral("backoff"))
                    .code == ningmail::ErrorCode::None);
        retryId = *retryRow.value;

        ningmail::OutboxItem failed = pending;
        failed.subject = "fourth";
        const auto failedRow = store.save(failed);
        REQUIRE(failedRow.ok());
        REQUIRE(store
                    .updateState(*failedRow.value, ningmail::OutboxState::Failed,
                                 QStringLiteral("gone"))
                    .code == ningmail::ErrorCode::None);

        ningmail::OutboxItem sending = pending;
        sending.subject = "fifth";
        const auto sendingRow = store.save(sending);
        REQUIRE(sendingRow.ok());
        REQUIRE(store.updateState(*sendingRow.value, ningmail::OutboxState::Sending).code ==
                ningmail::ErrorCode::None);
    }

    const auto list = store.listPending(accountId);
    REQUIRE(list.ok());
    // Only the two sendable states survive; created_at ASC decides here, so the
    // first inserted pending row leads.
    REQUIRE(list.value->size() == 2);
    REQUIRE(list.value->at(0).id == pendingId);
    REQUIRE(list.value->at(0).state == ningmail::OutboxState::Pending);
    REQUIRE(list.value->at(1).id == retryId);
    REQUIRE(list.value->at(1).state == ningmail::OutboxState::Retry);

    // Rows of other accounts must not leak in.
    const auto otherAccount = accountStore.create(sampleAccount());
    REQUIRE(otherAccount.ok());
    const auto otherList = store.listPending(*otherAccount.value);
    REQUIRE(otherList.ok());
    REQUIRE(otherList.value->isEmpty());
}

// The id tie-break keeps the queue order stable when insertions land in the
// same millisecond: identical created_at must order strictly by id ASC.
TEST_CASE("outbox store listPending breaks created_at ties by id ASC", "[outbox]") {
    QTemporaryDir dir;
    auto database = migratedDatabase(dir);
    ningmail::AccountStore accountStore(*database);
    const auto account = accountStore.create(sampleAccount());
    REQUIRE(account.ok());
    ningmail::OutboxStore store(*database);
    const ningmail::AccountId accountId = *account.value;

    const auto first = store.save(sampleItem(accountId));
    const auto second = store.save(sampleItem(accountId));
    REQUIRE(first.ok());
    REQUIRE(second.ok());
    REQUIRE(*second.value != *first.value);

    QSqlQuery equalizer(database->connection());
    REQUIRE(
        equalizer.exec(QStringLiteral("UPDATE outbox SET created_at = 5000 WHERE account_id = %1")
                           .arg(accountId)));

    const auto list = store.listPending(accountId);
    REQUIRE(list.ok());
    REQUIRE(list.value->size() == 2);
    REQUIRE(list.value->at(0).id == *first.value);
    REQUIRE(list.value->at(1).id == *second.value);
}

// ---------------------------------------------------------------------------
// SendService (R81): enqueue registers an account snapshot and stores the
// pending row; sendNow drives one backend round trip.
// ---------------------------------------------------------------------------

#include <QSqlQuery>

TEST_CASE("SendService enqueue stores a pending item with matching MIME and message id", "[send]") {
    SendFixture fixture;

    const ningmail::OutgoingMessage message = fixture.message();
    const qint64 id = fixture.service->enqueue(message);
    REQUIRE(id > 0);

    const auto stored = fixture.outbox->get(id);
    REQUIRE(stored.ok());
    REQUIRE(stored.value->state == ningmail::OutboxState::Pending);
    REQUIRE(stored.value->accountId == fixture.accountId);
    REQUIRE(stored.value->subject == message.subject);
    REQUIRE(stored.value->bodyText == message.bodyText);
    REQUIRE(stored.value->attempts == 0);
    // M1 has no retry schedule: next_retry_at stays unset.
    REQUIRE_FALSE(stored.value->nextRetryAt.isValid());
    REQUIRE(stored.value->lastError.isEmpty());

    // The generated id is a full <local-part@domain> and both the column and
    // the Message-ID header carry the exact same value.
    REQUIRE(stored.value->rfcMessageId.startsWith('<'));
    REQUIRE(stored.value->rfcMessageId.contains("@example.com>"));
    REQUIRE(stored.value->rawMime.contains(QByteArray("Message-ID: ") +
                                           stored.value->rfcMessageId.toUtf8()));

    // The stored body must decode back into the composed text.
    const auto parsed = ningmail::MimeParser::parse(stored.value->rawMime);
    REQUIRE(parsed.ok());
    CHECK(parsed.value->plainText == message.bodyText);
    CHECK(parsed.value->subject == message.subject);
}

TEST_CASE("SendService respects a preset rfcMessageId on enqueue", "[send]") {
    SendFixture fixture;

    ningmail::OutgoingMessage message = fixture.message();
    message.rfcMessageId = "<preset@ningmail.local>";
    const qint64 id = fixture.service->enqueue(message);
    REQUIRE(id > 0);

    const auto stored = fixture.outbox->get(id);
    REQUIRE(stored.ok());
    REQUIRE(stored.value->rfcMessageId == "<preset@ningmail.local>");
    REQUIRE(stored.value->rawMime.contains(QByteArray("Message-ID: <preset@ningmail.local>")));
}

TEST_CASE("SendService sendNow succeeds through the fake backend", "[send]") {
    SendFixture fixture;

    const ningmail::OutgoingMessage message = fixture.message();
    const qint64 id = fixture.service->enqueue(message);
    REQUIRE(id > 0);

    QSignalSpy spy(&*fixture.service, &ningmail::SendService::itemStateChanged);
    fixture.service->sendNow(id);

    // Sending is reported first, Sent is the final state.
    QTRY_COMPARE(spy.count(), 2);
    CHECK(spy.at(0).at(0).value<qint64>() == id);
    CHECK(spy.at(0).at(1).value<ningmail::OutboxState>() == ningmail::OutboxState::Sending);
    CHECK(spy.at(1).at(0).value<qint64>() == id);
    CHECK(spy.at(1).at(1).value<ningmail::OutboxState>() == ningmail::OutboxState::Sent);
    CHECK(spy.at(1).at(2).toString().isEmpty());

    // The row left the flight in Sent, without an attempt count bump (M1 bumps
    // attempts only on the failure path).
    const auto stored = fixture.outbox->get(id);
    REQUIRE(stored.ok());
    REQUIRE(stored.value->state == ningmail::OutboxState::Sent);
    REQUIRE(stored.value->attempts == 0);
    REQUIRE(stored.value->lastError.isEmpty());

    // The backend saw the send twice-logged by construction: send() records
    // its own fake entry, appendSent() records the raw MIME copy.
    const ningmail::FakeAccountSeed &seed = fixture.factory->seed(fixture.accountId);
    REQUIRE(seed.sent.size() == 2);
    CHECK(seed.sent.at(0).rawMime.isEmpty());                // send() entry
    CHECK(seed.sent.at(1).rawMime == stored.value->rawMime); // appendSent() copy
    CHECK(seed.sent.at(1).message.subject == message.subject);
}

TEST_CASE("SendService sendNow failure moves the item to Retry and bumps attempts", "[send]") {
    SendFixture fixture;
    fixture.factory->seed(fixture.accountId).failNextSend = true;

    const qint64 id = fixture.service->enqueue(fixture.message());
    REQUIRE(id > 0);

    QSignalSpy spy(&*fixture.service, &ningmail::SendService::itemStateChanged);
    fixture.service->sendNow(id);

    QTRY_COMPARE(spy.count(), 2);
    CHECK(spy.at(0).at(1).value<ningmail::OutboxState>() == ningmail::OutboxState::Sending);
    CHECK(spy.at(1).at(1).value<ningmail::OutboxState>() == ningmail::OutboxState::Retry);
    CHECK_FALSE(spy.at(1).at(2).toString().isEmpty());

    const auto stored = fixture.outbox->get(id);
    REQUIRE(stored.ok());
    REQUIRE(stored.value->state == ningmail::OutboxState::Retry);
    REQUIRE(stored.value->attempts == 1);
    REQUIRE_FALSE(stored.value->lastError.isEmpty());

    // A failed send must not leave a sent-folder copy behind.
    CHECK(fixture.factory->seed(fixture.accountId).sent.size() == 0);
}

TEST_CASE("SendService sendNow ignores a double click: only one attempt runs", "[send]") {
    SendFixture fixture;
    fixture.factory->seed(fixture.accountId).failNextSend = true;

    const qint64 id = fixture.service->enqueue(fixture.message());
    REQUIRE(id > 0);

    QSignalSpy spy(&*fixture.service, &ningmail::SendService::itemStateChanged);
    // The two calls land before the event loop can spin: the in-flight set
    // must swallow the second one.
    fixture.service->sendNow(id);
    fixture.service->sendNow(id);

    QTRY_COMPARE(spy.count(), 2); // Sending + Retry, never a third delivery
    // One failure exactly: no second attempt even after the event loop runs.
    QTest::qWait(50);
    CHECK(spy.count() == 2);
    const auto stored = fixture.outbox->get(id);
    REQUIRE(stored.ok());
    REQUIRE(stored.value->attempts == 1);
    REQUIRE(stored.value->state == ningmail::OutboxState::Retry);
    CHECK(fixture.factory->seed(fixture.accountId).sent.size() == 0);
}

TEST_CASE("SendService sendNow without a snapshot fails the item", "[send]") {
    SendFixture fixture;

    // The item belongs to this service, but a second service instance that
    // shares only the store has no snapshot for the account.
    const qint64 id = fixture.service->enqueue(fixture.message());
    REQUIRE(id > 0);
    ningmail::SendService orphan(*fixture.outbox, *fixture.factory, *fixture.credentials);

    QSignalSpy spy(&orphan, &ningmail::SendService::itemStateChanged);
    orphan.sendNow(id);

    QTRY_COMPARE(spy.count(), 2);
    CHECK(spy.at(0).at(1).value<ningmail::OutboxState>() == ningmail::OutboxState::Sending);
    CHECK(spy.at(1).at(1).value<ningmail::OutboxState>() == ningmail::OutboxState::Failed);
    CHECK(spy.at(1).at(2).toString().toStdString().find("account snapshot unavailable") !=
          std::string::npos);

    const auto stored = fixture.outbox->get(id);
    REQUIRE(stored.ok());
    REQUIRE(stored.value->state == ningmail::OutboxState::Failed);
    CHECK(stored.value->lastError == QStringLiteral("account snapshot unavailable"));
    // The snapshot-missing path never touches the backend.
    CHECK(fixture.factory->seed(fixture.accountId).sent.size() == 0);
}

TEST_CASE("SendService sendNow skips items that are neither Pending nor Retry", "[send]") {
    SendFixture fixture;

    const qint64 id = fixture.service->enqueue(fixture.message());
    REQUIRE(id > 0);
    REQUIRE(fixture.outbox->updateState(id, ningmail::OutboxState::Sent).code ==
            ningmail::ErrorCode::None);

    QSignalSpy spy(&*fixture.service, &ningmail::SendService::itemStateChanged);
    fixture.service->sendNow(id);

    // Skipped immediately: no state flip, no signal, no backend round trip.
    QTest::qWait(50);
    CHECK(spy.count() == 0);
    const auto stored = fixture.outbox->get(id);
    REQUIRE(stored.ok());
    REQUIRE(stored.value->state == ningmail::OutboxState::Sent);
    REQUIRE(fixture.factory->seed(fixture.accountId).sent.size() == 0);

    // A Retry row stays sendable: the same id can go out again.
    fixture.factory->seed(fixture.accountId).failNextSend = false;
    REQUIRE(fixture.outbox
                ->updateState(id, ningmail::OutboxState::Retry, QStringLiteral("previous failure"))
                .code == ningmail::ErrorCode::None);
    fixture.service->sendNow(id);
    QTRY_COMPARE(spy.count(), 2);
    CHECK(spy.at(1).at(1).value<ningmail::OutboxState>() == ningmail::OutboxState::Sent);
}
