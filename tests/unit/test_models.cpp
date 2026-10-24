#include "Store/AccountStore.h"
#include "Store/Database.h"
#include "Store/FolderStore.h"
#include "Store/MessageStore.h"
#include "models/FolderListModel.h"
#include "models/MessageListModel.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QList>
#include <QSet>
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

TEST_CASE("folder model roleNames match the role enum", "[models]") {
    MailboxFixture fixture;
    ningmail::FolderStore folderStore(*fixture.database);
    ningmail::FolderListModel model(folderStore);

    const auto names = model.roleNames();
    REQUIRE(names.size() == 5);
    REQUIRE(names.value(ningmail::FolderListModel::IdRole) == QByteArrayLiteral("id"));
    REQUIRE(names.value(ningmail::FolderListModel::AccountIdRole) ==
            QByteArrayLiteral("accountId"));
    REQUIRE(names.value(ningmail::FolderListModel::NameRole) == QByteArrayLiteral("name"));
    REQUIRE(names.value(ningmail::FolderListModel::RoleRole) == QByteArrayLiteral("role"));
    REQUIRE(names.value(ningmail::FolderListModel::UnreadRole) == QByteArrayLiteral("unread"));
}

TEST_CASE("folder model lists the folders of one account", "[models]") {
    auto fixture = makeMailbox();
    ningmail::FolderStore folderStore(*fixture.database);

    // A second folder sorted after INBOX (FolderStore::list orders by id).
    ningmail::Folder archive;
    archive.accountId = fixture.accountId;
    archive.fullName = "Archive";
    archive.displayName = "Archive";
    archive.role = ningmail::FolderRole::Archive;
    const auto createdArchive = folderStore.upsert(archive);
    REQUIRE(createdArchive.ok());

    ningmail::FolderListModel model(folderStore);
    REQUIRE(model.rowCount() == 0);

    model.reload(fixture.accountId);
    REQUIRE(model.rowCount() == 2);

    const auto inbox = model.index(0, 0);
    // NameRole is folder.fullName per the task ruling.
    REQUIRE(model.data(inbox, ningmail::FolderListModel::NameRole).toString() ==
            QStringLiteral("INBOX"));
    REQUIRE(model.data(inbox, ningmail::FolderListModel::IdRole).toLongLong() == fixture.folderId);
    REQUIRE(model.data(inbox, ningmail::FolderListModel::AccountIdRole).toLongLong() ==
            fixture.accountId);
    REQUIRE(model.data(inbox, ningmail::FolderListModel::RoleRole).toInt() ==
            static_cast<int>(ningmail::FolderRole::Inbox));
    // M1 placeholder: the model cannot read MessageStore, so unread is always 0.
    REQUIRE(model.data(inbox, ningmail::FolderListModel::UnreadRole).toInt() == 0);

    const auto archiveRow = model.index(1, 0);
    REQUIRE(model.data(archiveRow, ningmail::FolderListModel::NameRole).toString() ==
            QStringLiteral("Archive"));
}

TEST_CASE("folder model reload of an unknown account clears to zero rows", "[models]") {
    auto fixture = makeMailbox();
    ningmail::FolderStore folderStore(*fixture.database);
    ningmail::FolderListModel model(folderStore);

    model.reload(fixture.accountId);
    REQUIRE(model.rowCount() == 1);

    model.reload(999999);
    REQUIRE(model.rowCount() == 0);
    // Out-of-range reads must not crash.
    REQUIRE(model.data(model.index(0, 0), ningmail::FolderListModel::NameRole).isNull());
}

TEST_CASE("message model roleNames match the role enum", "[models]") {
    MailboxFixture fixture;
    ningmail::MessageStore messageStore(*fixture.database);
    ningmail::MessageListModel model(messageStore);

    const auto names = model.roleNames();
    REQUIRE(names.size() == 9);
    REQUIRE(names.value(ningmail::MessageListModel::IdRole) == QByteArrayLiteral("id"));
    REQUIRE(names.value(ningmail::MessageListModel::SubjectRole) == QByteArrayLiteral("subject"));
    REQUIRE(names.value(ningmail::MessageListModel::FromRole) == QByteArrayLiteral("from"));
    REQUIRE(names.value(ningmail::MessageListModel::FromAddressRole) ==
            QByteArrayLiteral("fromAddress"));
    REQUIRE(names.value(ningmail::MessageListModel::DateRole) == QByteArrayLiteral("date"));
    REQUIRE(names.value(ningmail::MessageListModel::UnreadRole) == QByteArrayLiteral("unread"));
    REQUIRE(names.value(ningmail::MessageListModel::FlaggedRole) == QByteArrayLiteral("flagged"));
    REQUIRE(names.value(ningmail::MessageListModel::HasAttachmentsRole) ==
            QByteArrayLiteral("hasAttachments"));
    REQUIRE(names.value(ningmail::MessageListModel::SnippetRole) == QByteArrayLiteral("snippet"));
}

TEST_CASE("message model pages one folder with fetchMore and stops at the folder count",
          "[models]") {
    auto fixture = makeMailbox();
    ningmail::MessageStore messageStore(*fixture.database);

    QList<ningmail::MessageHeader> headers;
    const qint64 baseSentAt = 1700000000000;
    for (int i = 0; i < 250; ++i) {
        headers.append(sampleHeader(fixture.accountId, fixture.folderId, i + 1,
                                    baseSentAt - static_cast<qint64>(i) * 1000));
    }
    REQUIRE_FALSE(messageStore.insertBatch(headers));

    ningmail::MessageListModel model(messageStore);
    REQUIRE(model.rowCount() == 0);

    // setFolder only loads the first page (PAGE_SIZE), never the whole folder.
    model.setFolder(fixture.folderId);
    REQUIRE(model.rowCount() == ningmail::MessageListModel::PAGE_SIZE);
    REQUIRE(model.canFetchMore(QModelIndex()));

    // Newest first: uid 1 carries the largest sent_at.
    REQUIRE(model.data(model.index(0, 0), ningmail::MessageListModel::SubjectRole).toString() ==
            QStringLiteral("Subject 1"));

    model.fetchMore(QModelIndex());
    REQUIRE(model.rowCount() == 200);
    REQUIRE(model.canFetchMore(QModelIndex()));

    model.fetchMore(QModelIndex());
    REQUIRE(model.rowCount() == 250);
    REQUIRE_FALSE(model.canFetchMore(QModelIndex()));

    // loadMore is fetchMore({}); past the end it must be a no-op.
    model.loadMore();
    REQUIRE(model.rowCount() == 250);

    // reload() resets the model to the first page only.
    model.reload();
    REQUIRE(model.rowCount() == ningmail::MessageListModel::PAGE_SIZE);
    REQUIRE(model.data(model.index(0, 0), ningmail::MessageListModel::SubjectRole).toString() ==
            QStringLiteral("Subject 1"));
}

TEST_CASE("message model exposes header fields through its roles", "[models]") {
    auto fixture = makeMailbox();
    ningmail::MessageStore messageStore(*fixture.database);

    ningmail::MessageHeader read;
    read.accountId = fixture.accountId;
    read.folderId = fixture.folderId;
    read.uid = 1;
    read.subject = QStringLiteral("已读的季度报告");
    read.from = {QStringLiteral("王小明"), QStringLiteral("wang@example.com")};
    read.sentAt = QDateTime::fromMSecsSinceEpoch(1635000000123);
    read.receivedAt = read.sentAt;
    read.flags = ningmail::MailFlag::Seen | ningmail::MailFlag::Flagged;
    read.hasAttachments = true;
    read.snippet = QStringLiteral("你好，这是正文摘要……");
    const auto readId = messageStore.insert(read);
    REQUIRE(readId.ok());

    ningmail::MessageHeader unnamed;
    unnamed.accountId = fixture.accountId;
    unnamed.folderId = fixture.folderId;
    unnamed.uid = 2;
    unnamed.subject = QStringLiteral("unread no-name sender");
    unnamed.from = {QString(), QStringLiteral("anon@example.com")};
    unnamed.sentAt = QDateTime::fromMSecsSinceEpoch(1635000000124);
    unnamed.receivedAt = unnamed.sentAt;
    const auto unnamedId = messageStore.insert(unnamed);
    REQUIRE(unnamedId.ok());

    ningmail::MessageListModel model(messageStore);
    model.setFolder(fixture.folderId);
    REQUIRE(model.rowCount() == 2);

    // Row 0 is the newest message (the unnamed one).
    const auto row0 = model.index(0, 0);
    REQUIRE(model.data(row0, ningmail::MessageListModel::IdRole).toLongLong() == *unnamedId.value);
    REQUIRE(model.data(row0, ningmail::MessageListModel::SubjectRole).toString() ==
            QStringLiteral("unread no-name sender"));
    // FromRole: name when present, otherwise the address as-is.
    REQUIRE(model.data(row0, ningmail::MessageListModel::FromRole).toString() ==
            QStringLiteral("anon@example.com"));
    REQUIRE(model.data(row0, ningmail::MessageListModel::FromAddressRole).toString() ==
            QStringLiteral("anon@example.com"));
    // UnreadRole = !(flags & Seen); flags are empty here.
    REQUIRE(model.data(row0, ningmail::MessageListModel::UnreadRole).toBool());
    REQUIRE_FALSE(model.data(row0, ningmail::MessageListModel::FlaggedRole).toBool());
    REQUIRE_FALSE(model.data(row0, ningmail::MessageListModel::HasAttachmentsRole).toBool());

    const auto row1 = model.index(1, 0);
    REQUIRE(model.data(row1, ningmail::MessageListModel::IdRole).toLongLong() == *readId.value);
    REQUIRE(model.data(row1, ningmail::MessageListModel::SubjectRole).toString() ==
            QStringLiteral("已读的季度报告"));
    REQUIRE(model.data(row1, ningmail::MessageListModel::FromRole).toString() ==
            QStringLiteral("王小明"));
    REQUIRE(model.data(row1, ningmail::MessageListModel::FromAddressRole).toString() ==
            QStringLiteral("wang@example.com"));
    // DateRole ships the full QDateTime for QML's Qt.formatDateTime.
    const QVariant dateVariant = model.data(row1, ningmail::MessageListModel::DateRole);
    REQUIRE(dateVariant.canConvert<QDateTime>());
    REQUIRE(dateVariant.toDateTime().toMSecsSinceEpoch() == 1635000000123);
    // flags carry Seen, so the message is not unread.
    REQUIRE_FALSE(model.data(row1, ningmail::MessageListModel::UnreadRole).toBool());
    REQUIRE(model.data(row1, ningmail::MessageListModel::FlaggedRole).toBool());
    REQUIRE(model.data(row1, ningmail::MessageListModel::HasAttachmentsRole).toBool());
    REQUIRE(model.data(row1, ningmail::MessageListModel::SnippetRole).toString() ==
            QStringLiteral("你好，这是正文摘要……"));
}

TEST_CASE("message model no-ops for an empty folder and an unset folder", "[models]") {
    auto fixture = makeMailbox();
    ningmail::MessageStore messageStore(*fixture.database);
    ningmail::MessageListModel model(messageStore);

    // No folder set yet: every call must be a harmless no-op.
    REQUIRE(model.rowCount() == 0);
    REQUIRE_FALSE(model.canFetchMore(QModelIndex()));
    model.fetchMore(QModelIndex());
    model.loadMore();
    REQUIRE(model.rowCount() == 0);

    // An empty folder resets to zero rows and cannot fetch more.
    model.setFolder(fixture.folderId);
    REQUIRE(model.rowCount() == 0);
    REQUIRE_FALSE(model.canFetchMore(QModelIndex()));
    model.fetchMore(QModelIndex());
    model.loadMore();
    REQUIRE(model.rowCount() == 0);

    // Resetting to no folder keeps the model empty.
    model.setFolder(0);
    REQUIRE(model.rowCount() == 0);
    REQUIRE_FALSE(model.canFetchMore(QModelIndex()));
}

TEST_CASE("message model loads 100 of 10000 on first open under 200ms and paging stays incremental",
          "[models][perf]") {
    auto fixture = makeMailbox();
    ningmail::MessageStore messageStore(*fixture.database);

    QList<ningmail::MessageHeader> headers;
    headers.reserve(10000);
    const qint64 baseSentAt = 1700000000000;
    for (int i = 0; i < 10000; ++i) {
        headers.append(sampleHeader(fixture.accountId, fixture.folderId, i + 1,
                                    baseSentAt - static_cast<qint64>(i) * 1000));
    }
    REQUIRE_FALSE(messageStore.insertBatch(headers));
    REQUIRE(messageStore.count(fixture.folderId) == 10000);

    ningmail::MessageListModel model(messageStore);

    // The setup above must not count towards the first-screen budget.
    QElapsedTimer timer;
    timer.start();
    model.setFolder(fixture.folderId);
    const qint64 elapsedMs = timer.elapsed();

    REQUIRE(model.rowCount() == 100);
    REQUIRE(model.canFetchMore(QModelIndex()));
    INFO("first page of 10000 messages took " << elapsedMs << "ms");
    REQUIRE(elapsedMs < 200);

    // Paging stays incremental: two fetches hold 300 rows, never the library.
    model.fetchMore(QModelIndex());
    REQUIRE(model.rowCount() == 200);
    model.fetchMore(QModelIndex());
    REQUIRE(model.rowCount() == 300);
    REQUIRE(model.canFetchMore(QModelIndex()));
    REQUIRE(messageStore.count(fixture.folderId) == 10000);
}

TEST_CASE("message model cursors through a tie group at the page boundary", "[models]") {
    auto fixture = makeMailbox();
    ningmail::MessageStore messageStore(*fixture.database);

    // 150 rows share one sent_at, so the tie group spans rows 100/101 of the
    // first page; ties break by id DESC and the cursor must not repeat a row.
    const qint64 sharedSentAt = 1700000000000;
    QList<ningmail::MessageHeader> headers;
    headers.reserve(150);
    for (int i = 0; i < 150; ++i) {
        headers.append(sampleHeader(fixture.accountId, fixture.folderId, i + 1, sharedSentAt));
    }
    REQUIRE_FALSE(messageStore.insertBatch(headers));

    ningmail::MessageListModel model(messageStore);
    model.setFolder(fixture.folderId);
    REQUIRE(model.rowCount() == 100);

    // Same sentAt for every row: ordering falls back to id DESC (uid DESC).
    // sampleHeader names the subject after the uid, so row 0 must be uid 150.
    for (int i = 0; i < 100; ++i) {
        INFO("row " << i);
        REQUIRE(model.data(model.index(i, 0), ningmail::MessageListModel::SubjectRole).toString() ==
                QStringLiteral("Subject %1").arg(150 - i));
    }

    model.fetchMore(QModelIndex());
    REQUIRE(model.rowCount() == 150);

    // The cursor advanced one row at a time inside the tie group: exactly the
    // older 50 rows follow, no duplicates, no repeats of page 1.
    QSet<qint64> seenIds;
    for (int i = 0; i < 150; ++i) {
        const qint64 id =
            model.data(model.index(i, 0), ningmail::MessageListModel::IdRole).toLongLong();
        INFO("row " << i << " id " << id);
        REQUIRE_FALSE(seenIds.contains(id));
        seenIds.insert(id);
        REQUIRE(model.data(model.index(i, 0), ningmail::MessageListModel::SubjectRole).toString() ==
                QStringLiteral("Subject %1").arg(150 - i));
    }
    REQUIRE(seenIds.size() == 150);
    // All 150 rows are loaded, and canFetchMore is false (loaded == count).
    REQUIRE_FALSE(model.canFetchMore(QModelIndex()));

    // Past the end loadMore() stays a no-op.
    model.loadMore();
    REQUIRE(model.rowCount() == 150);
}

TEST_CASE("message model switching between non-empty folders keeps no stale rows", "[models]") {
    auto fixture = makeMailbox();
    ningmail::MessageStore messageStore(*fixture.database);
    ningmail::FolderStore folderStore(*fixture.database);

    ningmail::Folder second;
    second.accountId = fixture.accountId;
    second.fullName = "Archive";
    second.displayName = "Archive";
    second.role = ningmail::FolderRole::Archive;
    const auto secondFolder = folderStore.upsert(second);
    REQUIRE(secondFolder.ok());

    QList<ningmail::MessageHeader> folderA;
    QList<ningmail::MessageHeader> folderB;
    const qint64 baseSentAt = 1700000000000;
    for (int i = 0; i < 120; ++i) {
        auto header = sampleHeader(fixture.accountId, fixture.folderId, i + 1,
                                   baseSentAt - static_cast<qint64>(i) * 1000);
        header.subject = QStringLiteral("A %1").arg(i + 1);
        folderA.append(header);
    }
    for (int i = 0; i < 30; ++i) {
        auto header = sampleHeader(fixture.accountId, *secondFolder.value, i + 1,
                                   baseSentAt - static_cast<qint64>(i) * 1000);
        header.subject = QStringLiteral("B %1").arg(i + 1);
        folderB.append(header);
    }
    REQUIRE_FALSE(messageStore.insertBatch(folderA));
    REQUIRE_FALSE(messageStore.insertBatch(folderB));

    ningmail::MessageListModel model(messageStore);

    model.setFolder(fixture.folderId);
    REQUIRE(model.rowCount() == 100);
    REQUIRE(model.data(model.index(0, 0), ningmail::MessageListModel::SubjectRole).toString() ==
            QStringLiteral("A 1"));
    REQUIRE(model.canFetchMore(QModelIndex()));

    // Switching resets to the new folder: no page-1 rows of A survive.
    model.setFolder(*secondFolder.value);
    REQUIRE(model.rowCount() == 30);
    REQUIRE(model.data(model.index(0, 0), ningmail::MessageListModel::SubjectRole).toString() ==
            QStringLiteral("B 1"));
    REQUIRE(model.data(model.index(0, 0), ningmail::MessageListModel::IdRole).toLongLong() ==
            messageStore.list(*secondFolder.value, 10).value->first().id);
    // 30 rows cover the whole folder, so there is nothing left to fetch.
    REQUIRE_FALSE(model.canFetchMore(QModelIndex()));
    for (int i = 0; i < 30; ++i) {
        INFO("row " << i);
        REQUIRE(model.data(model.index(i, 0), ningmail::MessageListModel::SubjectRole).toString() ==
                QStringLiteral("B %1").arg(i + 1));
    }

    // Switching back reopens A on its first page only.
    model.setFolder(fixture.folderId);
    REQUIRE(model.rowCount() == 100);
    REQUIRE(model.data(model.index(0, 0), ningmail::MessageListModel::SubjectRole).toString() ==
            QStringLiteral("A 1"));
    REQUIRE(model.canFetchMore(QModelIndex()));
}
