#include "Store/AccountStore.h"
#include "Store/Database.h"
#include "Store/FolderStore.h"
#include "Store/MessageStore.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QList>
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

struct MailboxFixture {
    QTemporaryDir dir;
    std::unique_ptr<ningmail::Database> database;
    ningmail::AccountId accountId = 0;
    ningmail::FolderId folderId = 0;
};

/// Real account + folder: the messages tables carry foreign keys to both.
MailboxFixture makeMailbox() {
    MailboxFixture fixture;
    fixture.database = migratedDatabase(fixture.dir);

    ningmail::AccountStore accounts(*fixture.database);
    ningmail::Account account;
    account.email = "work@example.com";
    const auto createdAccount = accounts.create(account);
    REQUIRE(createdAccount.ok());
    fixture.accountId = *createdAccount.value;

    ningmail::FolderStore folders(*fixture.database);
    ningmail::Folder folder;
    folder.accountId = fixture.accountId;
    folder.fullName = "INBOX";
    folder.displayName = "INBOX";
    folder.role = ningmail::FolderRole::Inbox;
    const auto createdFolder = folders.upsert(folder);
    REQUIRE(createdFolder.ok());
    fixture.folderId = *createdFolder.value;
    return fixture;
}

ningmail::MessageHeader sampleHeader(ningmail::AccountId accountId, ningmail::FolderId folderId,
                                     quint32 uid, qint64 sentAtMs) {
    ningmail::MessageHeader header;
    header.accountId = accountId;
    header.folderId = folderId;
    header.uid = uid;
    header.rfcMessageId = QStringLiteral("<uid%1@example.com>").arg(uid);
    header.subject = QStringLiteral("Subject %1").arg(uid);
    header.from = {QStringLiteral("Sender"), QStringLiteral("sender@example.com")};
    header.to.append({QStringLiteral("Receiver"), QStringLiteral("receiver@example.com")});
    header.sentAt = QDateTime::fromMSecsSinceEpoch(sentAtMs);
    header.receivedAt = header.sentAt;
    header.size = 100 + static_cast<qint64>(uid);
    header.snippet = QStringLiteral("Snippet %1").arg(uid);
    return header;
}

} // namespace

TEST_CASE("message store pages by keyset", "[message-store]") {
    auto fixture = makeMailbox();
    ningmail::MessageStore messages(*fixture.database);

    QList<ningmail::MessageHeader> headers;
    const qint64 baseSentAt = 1700000000000;
    for (int i = 0; i < 250; ++i) {
        headers.append(sampleHeader(fixture.accountId, fixture.folderId, i + 1,
                                    baseSentAt - static_cast<qint64>(i) * 1000));
    }
    REQUIRE_FALSE(messages.insertBatch(headers));
    REQUIRE(messages.count(fixture.folderId) == 250);

    auto page1 = messages.list(fixture.folderId, 100);
    REQUIRE(page1.ok());
    REQUIRE(page1.value->size() == 100);
    // Newest first: uid 1 carries the largest sent_at.
    REQUIRE(page1.value->first().uid == 1);
    REQUIRE(page1.value->last().uid == 100);
    const auto &last = page1.value->last();

    auto page2 = messages.list(fixture.folderId, 100,
                               ningmail::MessageCursor{last.sentAt.toMSecsSinceEpoch(), last.id});
    REQUIRE(page2.ok());
    REQUIRE(page2.value->size() == 100);
    REQUIRE(page2.value->first().id != last.id);
    // No overlap with page 1.
    REQUIRE(page2.value->first().uid == 101);
    REQUIRE(page2.value->last().uid == 200);

    const auto &page2Last = page2.value->last();
    auto page3 =
        messages.list(fixture.folderId, 100,
                      ningmail::MessageCursor{page2Last.sentAt.toMSecsSinceEpoch(), page2Last.id});
    REQUIRE(page3.ok());
    REQUIRE(page3.value->size() == 50);
    REQUIRE(page3.value->first().uid == 201);
    REQUIRE(page3.value->last().uid == 250);
}

TEST_CASE("duplicate uid updates instead of duplicating", "[message-store]") {
    auto fixture = makeMailbox();
    ningmail::MessageStore messages(*fixture.database);

    auto header = sampleHeader(fixture.accountId, fixture.folderId, 7, 1234567890);
    const auto id1 = messages.insert(header);
    REQUIRE(id1.ok());

    header.flags |= ningmail::MailFlag::Seen;
    header.subject = QStringLiteral("Updated subject");
    header.snippet = QStringLiteral("Updated snippet");
    const auto id2 = messages.insert(header);
    REQUIRE(id2.ok());

    REQUIRE(*id1.value == *id2.value);
    REQUIRE(messages.count(fixture.folderId) == 1);
    REQUIRE(messages.unreadCount(fixture.folderId) == 0);

    const auto found = messages.header(*id2.value);
    REQUIRE(found.ok());
    REQUIRE(found.value->subject == QStringLiteral("Updated subject"));
    REQUIRE(found.value->snippet == QStringLiteral("Updated snippet"));
    REQUIRE(found.value->flags.testFlag(ningmail::MailFlag::Seen));
}

TEST_CASE("header lookup by id", "[message-store]") {
    auto fixture = makeMailbox();
    ningmail::MessageStore messages(*fixture.database);

    const auto header = sampleHeader(fixture.accountId, fixture.folderId, 42, 999);
    const auto id = messages.insert(header);
    REQUIRE(id.ok());

    const auto found = messages.header(*id.value);
    REQUIRE(found.ok());
    REQUIRE(found.value->subject == header.subject);

    const auto missing = messages.header(12345);
    REQUIRE_FALSE(missing.ok());
    REQUIRE(missing.error.code == ningmail::ErrorCode::Storage);
}

TEST_CASE("insertBatch handles 500 headers under 200ms", "[message-store][perf]") {
    auto fixture = makeMailbox();
    ningmail::MessageStore messages(*fixture.database);

    QList<ningmail::MessageHeader> headers;
    const qint64 baseSentAt = 1700000000000;
    for (int i = 0; i < 500; ++i) {
        headers.append(sampleHeader(fixture.accountId, fixture.folderId, i + 1,
                                    baseSentAt - static_cast<qint64>(i) * 1000));
    }

    QElapsedTimer timer;
    timer.start();
    REQUIRE_FALSE(messages.insertBatch(headers));
    const qint64 elapsedMs = timer.elapsed();
    REQUIRE(messages.count(fixture.folderId) == 500);
    REQUIRE(elapsedMs < 200);
}

TEST_CASE("header round-trips every field", "[message-store]") {
    auto fixture = makeMailbox();
    ningmail::MessageStore messages(*fixture.database);

    ningmail::MessageHeader header;
    header.accountId = fixture.accountId;
    header.folderId = fixture.folderId;
    header.uid = 777;
    header.rfcMessageId = QStringLiteral("<abc-123@example.com>");
    header.threadId = QStringLiteral("thread-1");
    header.subject = QStringLiteral("主题：季度报告");
    header.from = {QStringLiteral("王小明"), QStringLiteral("wang@example.com")};
    header.to.append({QStringLiteral("Li \"Lee\" 李"), QStringLiteral("li@example.com")});
    header.to.append({QStringLiteral("赵四"), QStringLiteral("zhao@example.com")});
    header.cc.append({QStringLiteral("抄送人"), QStringLiteral("cc@example.com")});
    header.bcc.append({QStringLiteral("密送人"), QStringLiteral("bcc@example.com")});
    const QDateTime sentAt = QDateTime::fromMSecsSinceEpoch(1635000000123);
    header.sentAt = sentAt;
    header.receivedAt = sentAt;
    header.size = 98765;
    header.flags =
        ningmail::MailFlag::Seen | ningmail::MailFlag::Flagged | ningmail::MailFlag::Answered;
    header.hasAttachments = true;
    header.snippet = QStringLiteral("你好，这是正文摘要……");
    header.bodyState = ningmail::BodyState::Ready;

    const auto inserted = messages.insert(header);
    REQUIRE(inserted.ok());

    const auto found = messages.header(*inserted.value);
    REQUIRE(found.ok());
    const auto &stored = *found.value;
    REQUIRE(stored.accountId == header.accountId);
    REQUIRE(stored.folderId == header.folderId);
    REQUIRE(stored.uid == header.uid);
    REQUIRE(stored.rfcMessageId == header.rfcMessageId);
    REQUIRE(stored.threadId == header.threadId);
    REQUIRE(stored.subject == header.subject);
    REQUIRE(stored.from == header.from);
    REQUIRE(stored.to == header.to);
    REQUIRE(stored.cc == header.cc);
    REQUIRE(stored.bcc == header.bcc);
    REQUIRE(stored.sentAt.toMSecsSinceEpoch() == header.sentAt.toMSecsSinceEpoch());
    REQUIRE(stored.receivedAt.toMSecsSinceEpoch() == header.receivedAt.toMSecsSinceEpoch());
    REQUIRE(stored.size == header.size);
    REQUIRE(stored.flags == header.flags);
    REQUIRE(stored.hasAttachments);
    REQUIRE(stored.snippet == header.snippet);
    REQUIRE(stored.bodyState == header.bodyState);
}

TEST_CASE("insert into a missing folder fails with a storage error", "[message-store]") {
    auto fixture = makeMailbox();
    ningmail::MessageStore messages(*fixture.database);

    const auto header = sampleHeader(fixture.accountId, 999999, 1, 1000);
    const auto result = messages.insert(header);
    REQUIRE_FALSE(result.ok());
    REQUIRE(result.error.code == ningmail::ErrorCode::Storage);
    // The failing insert must not leave half-written state behind.
    REQUIRE_FALSE(messages.header(999999).ok());
    REQUIRE(messages.count(fixture.folderId) == 0);
}

TEST_CASE("body is empty until saved and saveBody upserts", "[message-store]") {
    auto fixture = makeMailbox();
    ningmail::MessageStore messages(*fixture.database);

    const auto inserted =
        messages.insert(sampleHeader(fixture.accountId, fixture.folderId, 1, 100));
    REQUIRE(inserted.ok());
    const ningmail::MessageId id = *inserted.value;

    // Nothing fetched yet: an empty body, not an error.
    const auto missing = messages.body(id);
    REQUIRE(missing.ok());
    REQUIRE(missing.value->messageId == id);
    REQUIRE(missing.value->contentType.isEmpty());
    REQUIRE(missing.value->html.isEmpty());
    REQUIRE(missing.value->plainText.isEmpty());

    ningmail::MessageBody body;
    body.messageId = id;
    body.contentType = QStringLiteral("text/html");
    body.charset = QStringLiteral("utf-8");
    body.html = QStringLiteral("<p>你好</p>");
    body.plainText = QStringLiteral("你好");
    REQUIRE_FALSE(messages.saveBody(body));

    const auto fetched = messages.body(id);
    REQUIRE(fetched.ok());
    REQUIRE(fetched.value->messageId == id);
    REQUIRE(fetched.value->contentType == body.contentType);
    REQUIRE(fetched.value->charset == body.charset);
    REQUIRE(fetched.value->html == body.html);
    REQUIRE(fetched.value->plainText == body.plainText);

    // A second save replaces the row in place.
    body.html = QStringLiteral("<p>更新后的正文</p>");
    REQUIRE_FALSE(messages.saveBody(body));
    const auto updated = messages.body(id);
    REQUIRE(updated.ok());
    REQUIRE(updated.value->html == QStringLiteral("<p>更新后的正文</p>"));
    REQUIRE(updated.value->plainText == body.plainText);
}

TEST_CASE("list with a non-positive limit returns an empty page", "[message-store]") {
    auto fixture = makeMailbox();
    ningmail::MessageStore messages(*fixture.database);

    QList<ningmail::MessageHeader> headers;
    for (int i = 0; i < 3; ++i) {
        headers.append(sampleHeader(fixture.accountId, fixture.folderId, i + 1, 1000 + i * 100));
    }
    REQUIRE_FALSE(messages.insertBatch(headers));

    auto zero = messages.list(fixture.folderId, 0);
    REQUIRE(zero.ok());
    REQUIRE(zero.value->isEmpty());
    auto negative = messages.list(fixture.folderId, -1);
    REQUIRE(negative.ok());
    REQUIRE(negative.value->isEmpty());
    auto capped = messages.list(fixture.folderId, 2);
    REQUIRE(capped.ok());
    REQUIRE(capped.value->size() == 2);
}

TEST_CASE("invalid sentAt is stored as epoch zero", "[message-store]") {
    auto fixture = makeMailbox();
    ningmail::MessageStore messages(*fixture.database);

    auto header = sampleHeader(fixture.accountId, fixture.folderId, 5, 12345);
    header.sentAt = QDateTime();
    header.receivedAt = QDateTime();
    const auto inserted = messages.insert(header);
    REQUIRE(inserted.ok());

    const auto found = messages.header(*inserted.value);
    REQUIRE(found.ok());
    REQUIRE(found.value->sentAt.toMSecsSinceEpoch() == 0);
    REQUIRE(found.value->sentAt.isValid());
}

TEST_CASE("updateFlags and setBodyState persist", "[message-store]") {
    auto fixture = makeMailbox();
    ningmail::MessageStore messages(*fixture.database);

    const auto inserted =
        messages.insert(sampleHeader(fixture.accountId, fixture.folderId, 1, 100));
    REQUIRE(inserted.ok());
    const ningmail::MessageId id = *inserted.value;
    REQUIRE(messages.unreadCount(fixture.folderId) == 1);

    REQUIRE_FALSE(messages.updateFlags(id, ningmail::MailFlag::Seen | ningmail::MailFlag::Flagged));
    REQUIRE_FALSE(messages.setBodyState(id, ningmail::BodyState::Ready));

    const auto found = messages.header(id);
    REQUIRE(found.ok());
    REQUIRE(found.value->flags == (ningmail::MailFlag::Seen | ningmail::MailFlag::Flagged));
    REQUIRE(found.value->bodyState == ningmail::BodyState::Ready);
    REQUIRE(messages.unreadCount(fixture.folderId) == 0);

    // Unknown ids must fail with a storage error, not crash.
    REQUIRE(messages.updateFlags(4242, ningmail::MailFlag::Seen).code ==
            ningmail::ErrorCode::Storage);
    REQUIRE(messages.setBodyState(4242, ningmail::BodyState::Error).code ==
            ningmail::ErrorCode::Storage);
}

TEST_CASE("clearFolder deletes messages and cascades to bodies and attachments",
          "[message-store]") {
    auto fixture = makeMailbox();
    ningmail::MessageStore messages(*fixture.database);

    QList<ningmail::MessageHeader> headers;
    for (int i = 0; i < 2; ++i) {
        headers.append(sampleHeader(fixture.accountId, fixture.folderId, i + 1, 1000 + i));
    }
    REQUIRE_FALSE(messages.insertBatch(headers));

    ningmail::MessageId firstMessageId = 0;
    ningmail::MessageBody body;
    body.contentType = QStringLiteral("text/html");
    body.html = QStringLiteral("<p>body</p>");
    for (const auto &header : headers) {
        // The (folder_id, uid) rows already exist from the batch, so insert()
        // resolves to the same row and hands back its id.
        auto inserted = messages.insert(header);
        REQUIRE(inserted.ok());
        body.messageId = *inserted.value;
        if (firstMessageId == 0) {
            firstMessageId = body.messageId;
        }
        REQUIRE_FALSE(messages.saveBody(body));
    }

    // A directly-inserted attachments row must cascade away with its message.
    {
        QSqlQuery query(fixture.database->connection());
        REQUIRE(query.exec(
            "INSERT INTO attachments (message_pk, filename, mime_type, size, content_id,"
            " is_inline, storage_key, sha256, state) SELECT id, 'f.txt', 'text/plain', 10, '', 0,"
            " NULL, NULL, 0 FROM messages LIMIT 1"));
    }

    REQUIRE_FALSE(messages.clearFolder(fixture.folderId));
    REQUIRE(messages.count(fixture.folderId) == 0);
    REQUIRE(messages.unreadCount(fixture.folderId) == 0);
    auto found = messages.header(firstMessageId);
    REQUIRE_FALSE(found.ok());
    auto gone = messages.body(firstMessageId);
    REQUIRE(gone.ok());
    REQUIRE(gone.value->html.isEmpty());

    QSqlQuery count(fixture.database->connection());
    REQUIRE(count.exec(QStringLiteral("SELECT COUNT(*) FROM bodies")));
    REQUIRE(count.next());
    REQUIRE(count.value(0).toLongLong() == 0);
    REQUIRE(count.exec(QStringLiteral("SELECT COUNT(*) FROM attachments")));
    REQUIRE(count.next());
    REQUIRE(count.value(0).toLongLong() == 0);
}

TEST_CASE("list cursor breaks sent_at ties by id descending", "[message-store]") {
    auto fixture = makeMailbox();
    ningmail::MessageStore messages(*fixture.database);

    // Three rows share one sent_at (the keyset tie branch) plus one newer row.
    const qint64 sameSentAt = 1700000000000;
    QList<ningmail::MessageHeader> headers;
    for (int i = 0; i < 3; ++i) {
        headers.append(sampleHeader(fixture.accountId, fixture.folderId, i + 1, sameSentAt));
    }
    headers.append(sampleHeader(fixture.accountId, fixture.folderId, 4, sameSentAt + 1000));
    REQUIRE_FALSE(messages.insertBatch(headers));
    REQUIRE(messages.count(fixture.folderId) == 4);

    auto all = messages.list(fixture.folderId, 10);
    REQUIRE(all.ok());
    REQUIRE(all.value->size() == 4);
    // The newer row first; ties fall back to id descending, nothing lost.
    REQUIRE(all.value->at(0).uid == 4);
    REQUIRE(all.value->at(1).uid == 3);
    REQUIRE(all.value->at(2).uid == 2);
    REQUIRE(all.value->at(3).uid == 1);

    // A cursor on the middle row of the tie group yields exactly the older half.
    const auto &tieMiddle = all.value->at(2);
    auto olderHalf =
        messages.list(fixture.folderId, 10,
                      ningmail::MessageCursor{tieMiddle.sentAt.toMSecsSinceEpoch(), tieMiddle.id});
    REQUIRE(olderHalf.ok());
    REQUIRE(olderHalf.value->size() == 1);
    REQUIRE(olderHalf.value->first().uid == 1);

    // A cursor pointing at a deleted row still returns a legal prefix page:
    // the rows that were strictly before it, no duplicates, no gaps.
    const ningmail::MessageId deletedId = all.value->at(1).id;
    {
        QSqlQuery query(fixture.database->connection());
        REQUIRE(query.prepare(QStringLiteral("DELETE FROM messages WHERE id = :id")));
        query.bindValue(QStringLiteral(":id"), deletedId);
        REQUIRE(query.exec());
    }
    auto afterDelete =
        messages.list(fixture.folderId, 10, ningmail::MessageCursor{sameSentAt, deletedId});
    REQUIRE(afterDelete.ok());
    REQUIRE(afterDelete.value->size() == 2);
    REQUIRE(afterDelete.value->at(0).uid == 2);
    REQUIRE(afterDelete.value->at(1).uid == 1);
}

TEST_CASE("insertBatch rolls back the whole batch on failure", "[message-store]") {
    auto fixture = makeMailbox();
    ningmail::MessageStore messages(*fixture.database);

    // Valid rows before and after a foreign-key violation; nothing must land.
    QList<ningmail::MessageHeader> headers;
    headers.append(sampleHeader(fixture.accountId, fixture.folderId, 1, 1000));
    headers.append(sampleHeader(fixture.accountId, 999999, 2, 2000));
    headers.append(sampleHeader(fixture.accountId, fixture.folderId, 3, 3000));

    const auto failure = messages.insertBatch(headers);
    INFO(failure.message.toStdString());
    REQUIRE(failure.code == ningmail::ErrorCode::Storage);
    // The transaction rolled back: not even the valid rows exist.
    REQUIRE(messages.count(fixture.folderId) == 0);
    auto orphan = messages.header(1);
    REQUIRE_FALSE(orphan.ok());

    // The store stays usable after the failed batch.
    auto retry = messages.insert(headers.first());
    REQUIRE(retry.ok());
    REQUIRE(messages.count(fixture.folderId) == 1);
}
