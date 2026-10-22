#include "Store/Database.h"

#include <QDir>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtGlobal>

#include <catch2/catch_test_macros.hpp>

namespace {

/// Captures Qt warnings (and above) for the scope; restores the previous handler.
class WarningCapture {
  public:
    WarningCapture() : m_previous(qInstallMessageHandler(&WarningCapture::handle)) {
        current = this;
    }

    ~WarningCapture() {
        qInstallMessageHandler(m_previous);
        current = nullptr;
    }

    WarningCapture(const WarningCapture &) = delete;
    WarningCapture &operator=(const WarningCapture &) = delete;

    const QStringList &messages() const {
        return m_messages;
    }

  private:
    static void handle(QtMsgType type, const QMessageLogContext &context, const QString &message) {
        if (current && (type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg)) {
            current->m_messages.append(message);
        }
        if (current && current->m_previous) {
            current->m_previous(type, context, message);
        }
    }

    QtMessageHandler m_previous = nullptr;
    QStringList m_messages;
    inline static WarningCapture *current = nullptr;
};

QString pragmaValue(ningmail::Database &db, const char *pragma) {
    QSqlQuery query(db.connection());
    REQUIRE(query.exec(QLatin1String(pragma)));
    REQUIRE(query.next());
    return query.value(0).toString();
}

} // namespace

TEST_CASE("database migrates to schema v1", "[db]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    ningmail::Database db(dir.filePath("test.db"));
    REQUIRE_FALSE(db.open());
    REQUIRE_FALSE(db.migrate());
    REQUIRE(db.schemaVersion() == 1);
    const auto tables = db.connection().tables();
    for (auto name : {"accounts", "folders", "messages", "bodies", "attachments", "drafts",
                      "outbox", "schema_version"})
        REQUIRE(tables.contains(name));
}

TEST_CASE("migration is idempotent", "[db]") {
    QTemporaryDir dir;
    ningmail::Database db(dir.filePath("test.db"));
    REQUIRE_FALSE(db.open());
    REQUIRE_FALSE(db.migrate());
    REQUIRE_FALSE(db.migrate());
    REQUIRE(db.schemaVersion() == 1);
}

TEST_CASE("migration failure rolls back and can be retried", "[db]") {
    QTemporaryDir dir;
    ningmail::Database db(dir.filePath("test.db"));
    REQUIRE_FALSE(db.open());
    QSqlQuery query(db.connection());
    REQUIRE(query.exec("CREATE TABLE folders (id INTEGER PRIMARY KEY)"));

    const auto failed = db.migrate();
    INFO(failed.message.toStdString());
    REQUIRE(failed.code == ningmail::ErrorCode::Storage);
    REQUIRE_FALSE(failed.detail.isEmpty());
    REQUIRE(db.schemaVersion() == 0);
    const auto tables = db.connection().tables(); // only the sabotaging table may remain
    REQUIRE(tables.contains(QStringLiteral("folders")));
    REQUIRE_FALSE(tables.contains(QStringLiteral("accounts")));
    REQUIRE_FALSE(tables.contains(QStringLiteral("schema_version")));

    REQUIRE(query.exec("DROP TABLE folders"));
    REQUIRE_FALSE(db.migrate());
    REQUIRE(db.schemaVersion() == 1);
}

TEST_CASE("deleting an account cascades to folders, messages, bodies and attachments", "[db]") {
    QTemporaryDir dir;
    ningmail::Database db(dir.filePath("test.db"));
    REQUIRE_FALSE(db.open());
    REQUIRE_FALSE(db.migrate());

    QSqlQuery query(db.connection());
    REQUIRE(query.exec(
        "INSERT INTO accounts (display_name, email, imap_host, imap_port, imap_security, smtp_host,"
        " smtp_port, smtp_security, auth_type, username, credential_ref, signature_html, "
        "sort_order)"
        " VALUES ('Test', 'test@example.com', 'imap.example.com', 993, 2, 'smtp.example.com', 465, "
        "2,"
        " 0, 'user', 'ref', '', 0)"));
    REQUIRE(query.exec(
        "INSERT INTO folders (account_id, full_name, display_name, role, delimiter, uid_validity,"
        " uid_next, highest_mod_seq, sync_state) VALUES (1, 'INBOX', 'INBOX', 0, '/', 1, 2, 0, "
        "NULL)"));
    REQUIRE(query.exec(
        "INSERT INTO messages (account_id, folder_id, uid, rfc_message_id, thread_id, subject,"
        " from_name, from_addr, to_addrs, cc_addrs, bcc_addrs, sent_at, received_at, size, flags,"
        " has_attachments, snippet, body_state, created_at) VALUES (1, 1, 1, '<a@example.com>', "
        "'t1',"
        " 'Subject', 'From', 'from@example.com', '[]', '[]', '[]', 1000, 1001, 10, 0, 1, 'snippet',"
        " 2, 0)"));
    REQUIRE(query.exec(
        "INSERT INTO bodies (message_pk, content_type, charset, html_raw, text_plain, fetched_at,"
        " size) VALUES (1, 'text/html', 'utf-8', '<p>hi</p>', 'hi', NULL, 10)"));
    REQUIRE(query.exec(
        "INSERT INTO attachments (message_pk, filename, mime_type, size, content_id, is_inline,"
        " storage_key, sha256, state) VALUES (1, 'f.txt', 'text/plain', 10, '', 0, NULL, NULL, "
        "0)"));

    REQUIRE(query.exec("DELETE FROM accounts WHERE id = 1"));

    for (auto name : {"folders", "messages", "bodies", "attachments"}) {
        QSqlQuery count(db.connection());
        count.exec(QStringLiteral("SELECT COUNT(*) FROM %1").arg(QLatin1String(name)));
        count.next();
        REQUIRE(count.value(0).toLongLong() == 0);
    }
}

TEST_CASE("open enables WAL, foreign keys and busy timeout", "[db]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    ningmail::Database db(dir.filePath("test.db"));
    REQUIRE_FALSE(db.open());
    REQUIRE_FALSE(db.migrate());

    REQUIRE(pragmaValue(db, "PRAGMA journal_mode") == QStringLiteral("wal"));
    REQUIRE(pragmaValue(db, "PRAGMA foreign_keys").toInt() == 1);
    REQUIRE(pragmaValue(db, "PRAGMA busy_timeout").toInt() == 5000);
}

TEST_CASE("migrate rejects a newer schema version than supported", "[db]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    ningmail::Database db(dir.filePath("test.db"));
    REQUIRE_FALSE(db.open());
    REQUIRE_FALSE(db.migrate());

    QSqlQuery query(db.connection());
    REQUIRE(query.exec("UPDATE schema_version SET version = 2"));

    const auto error = db.migrate();
    INFO(error.message.toStdString());
    REQUIRE(error.code == ningmail::ErrorCode::Storage);
    REQUIRE_FALSE(error.detail.isEmpty());
    REQUIRE(db.schemaVersion() == 2); // destructive migration must not happen
}

TEST_CASE("open creates missing parent directories and defaultPath uses ningmail.db", "[db]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    ningmail::Database db(dir.filePath("nested/dirs/test.db"));
    REQUIRE_FALSE(db.open());
    REQUIRE_FALSE(db.migrate());
    REQUIRE(db.schemaVersion() == 1);
    REQUIRE(QDir(dir.filePath("nested/dirs")).exists());
    REQUIRE(ningmail::Database::defaultPath().endsWith(QStringLiteral("/ningmail.db")));
}

TEST_CASE("open failure and retry emits no Qt connection warnings", "[db]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    // A directory cannot be opened as a database file, so open() fails.
    const QString directoryPath = dir.filePath("not-a-database");
    REQUIRE(QDir().mkpath(directoryPath));

    WarningCapture capture;
    ningmail::Database db(directoryPath);
    auto first = db.open();
    INFO(first.message.toStdString());
    REQUIRE(first.code == ningmail::ErrorCode::Storage);
    auto second = db.open(); // retry must not duplicate the Qt connection name
    INFO(second.message.toStdString());
    REQUIRE(second.code == ningmail::ErrorCode::Storage);

    INFO(capture.messages().join(QStringLiteral(" | ")).toStdString());
    REQUIRE(capture.messages().isEmpty());
}

TEST_CASE("destroying a never-opened database emits no warnings", "[db]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());

    WarningCapture capture;
    { ningmail::Database db(dir.filePath("test.db")); }

    INFO(capture.messages().join(QStringLiteral(" | ")).toStdString());
    REQUIRE(capture.messages().isEmpty());
}
