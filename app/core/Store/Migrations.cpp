#include "Store/Migrations.h"

#include <QSqlError>
#include <QSqlQuery>

namespace ningmail {

namespace Migrations {

namespace {

/*
 * Storage conventions (schema v1):
 *
 * - timestamps: INTEGER, Unix milliseconds (QDateTime::toMSecsSinceEpoch); nullable:
 *   `next_retry_at` (outbox), `fetched_at` (bodies).
 * - all foreign keys cascade on delete: accounts → folders/drafts/outbox, accounts → messages,
 *   folders → messages, messages → bodies/attachments.
 * - address lists (from/to/cc/bcc, draft/outbox recipients/cc): JSON array text.
 * - folders.sync_state: TEXT, free-form (e.g. JSON) IMAP sync state, nullable.
 * - outbox.raw_mime: BLOB.
 *
 * - enums: INTEGER, values follow the C++ declaration order:
 *   - Security (accounts.imap_security/smtp_security): None=0, StartTls=1, Tls=2
 *   - AuthType (accounts.auth_type): Password=0, OAuth2=1
 *   - FolderRole (folders.role): Inbox=0, Sent=1, Drafts=2, Trash=3, Junk=4, Archive=5, Custom=6
 *   - BodyState (messages.body_state): None=0, Fetching=1, Ready=2, Error=3
 *   - MailFlags (messages.flags) bitmask: Seen=1, Flagged=2, Answered=4, Draft=8, Deleted=16
 *   - AttachmentState (attachments.state): Queued=0, Downloading=1, Done=2, Error=3
 *   - OutboxState (outbox.state): Pending=0, Sending=1, Sent=2, Retry=3, Failed=4
 */

constexpr int kCurrentVersion = 1;

const char *kSchemaStatements[] = {
    R"sql(CREATE TABLE accounts (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        display_name TEXT NOT NULL,
        email TEXT NOT NULL,
        imap_host TEXT NOT NULL,
        imap_port INTEGER NOT NULL,
        imap_security INTEGER NOT NULL,
        smtp_host TEXT NOT NULL,
        smtp_port INTEGER NOT NULL,
        smtp_security INTEGER NOT NULL,
        auth_type INTEGER NOT NULL,
        username TEXT NOT NULL,
        credential_ref TEXT NOT NULL,
        signature_html TEXT NOT NULL DEFAULT '',
        sort_order INTEGER NOT NULL DEFAULT 0
    ))sql",
    R"sql(CREATE TABLE folders (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        account_id INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
        full_name TEXT NOT NULL,
        display_name TEXT NOT NULL,
        role INTEGER NOT NULL,
        delimiter TEXT NOT NULL,
        uid_validity INTEGER NOT NULL,
        uid_next INTEGER NOT NULL,
        highest_mod_seq INTEGER NOT NULL,
        sync_state TEXT,
        UNIQUE(account_id, full_name)
    ))sql",
    R"sql(CREATE TABLE messages (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        account_id INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
        folder_id INTEGER NOT NULL REFERENCES folders(id) ON DELETE CASCADE,
        uid INTEGER NOT NULL,
        rfc_message_id TEXT NOT NULL,
        thread_id TEXT NOT NULL,
        subject TEXT NOT NULL,
        from_name TEXT NOT NULL,
        from_addr TEXT NOT NULL,
        to_addrs TEXT NOT NULL,
        cc_addrs TEXT NOT NULL,
        bcc_addrs TEXT NOT NULL,
        sent_at INTEGER NOT NULL,
        received_at INTEGER NOT NULL,
        size INTEGER NOT NULL,
        flags INTEGER NOT NULL,
        has_attachments INTEGER NOT NULL,
        snippet TEXT NOT NULL,
        body_state INTEGER NOT NULL,
        created_at INTEGER NOT NULL,
        UNIQUE(folder_id, uid)
    ))sql",
    R"sql(CREATE INDEX idx_messages_folder_sent_at ON messages (folder_id, sent_at DESC, id DESC))sql",
    R"sql(CREATE INDEX idx_messages_account_thread_id ON messages (account_id, thread_id, sent_at))sql",
    R"sql(CREATE INDEX idx_messages_folder_flags ON messages (folder_id, flags))sql",
    R"sql(CREATE INDEX idx_messages_rfc_message_id ON messages (rfc_message_id))sql",
    R"sql(CREATE TABLE bodies (
        message_pk INTEGER PRIMARY KEY REFERENCES messages(id) ON DELETE CASCADE,
        content_type TEXT NOT NULL,
        charset TEXT NOT NULL,
        html_raw TEXT NOT NULL,
        text_plain TEXT NOT NULL,
        fetched_at INTEGER,
        size INTEGER NOT NULL
    ))sql",
    R"sql(CREATE TABLE attachments (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        message_pk INTEGER NOT NULL REFERENCES messages(id) ON DELETE CASCADE,
        filename TEXT NOT NULL,
        mime_type TEXT NOT NULL,
        size INTEGER NOT NULL,
        content_id TEXT NOT NULL,
        is_inline INTEGER NOT NULL,
        storage_key TEXT,
        sha256 TEXT,
        state INTEGER NOT NULL
    ))sql",
    R"sql(CREATE TABLE drafts (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        account_id INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
        subject TEXT NOT NULL,
        body_text TEXT NOT NULL,
        recipients_json TEXT NOT NULL,
        cc_json TEXT NOT NULL,
        updated_at INTEGER NOT NULL
    ))sql",
    R"sql(CREATE TABLE outbox (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        account_id INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
        recipients_json TEXT NOT NULL,
        cc_json TEXT NOT NULL,
        subject TEXT NOT NULL,
        body_text TEXT NOT NULL,
        raw_mime BLOB,
        state INTEGER NOT NULL,
        attempts INTEGER NOT NULL,
        next_retry_at INTEGER,
        last_error TEXT,
        rfc_message_id TEXT,
        created_at INTEGER NOT NULL
    ))sql",
    R"sql(CREATE TABLE schema_version (version INTEGER NOT NULL))sql",
    R"sql(INSERT INTO schema_version (version) VALUES (1))sql",
};

MailError rollbackFailure(QSqlDatabase &db, const QSqlError &error, const char *context) {
    db.rollback();
    return MailError{ErrorCode::Storage,
                     QStringLiteral("Migration failed: %1").arg(QLatin1String(context)),
                     error.text()};
}

} // namespace

MailError apply(QSqlDatabase &db) {
    int version = 0;
    {
        QSqlQuery query(db);
        if (query.exec(QStringLiteral("SELECT version FROM schema_version LIMIT 1")) &&
            query.next()) {
            version = query.value(0).toInt();
        }
        query.finish();
    }

    if (version > kCurrentVersion) {
        return MailError{
            ErrorCode::Storage,
            QStringLiteral("Database schema version %1 is newer than supported version %2")
                .arg(version)
                .arg(kCurrentVersion),
            QStringLiteral("Upgrade the application before using this database")};
    }
    if (version == kCurrentVersion) {
        return MailError{};
    }

    if (!db.transaction()) {
        return rollbackFailure(db, db.lastError(), "begin transaction");
    }

    for (const char *statement : kSchemaStatements) {
        QSqlQuery query(db);
        if (!query.exec(QLatin1String(statement))) {
            const QSqlError error = query.lastError();
            query.finish();
            return rollbackFailure(db, error, "create schema v1");
        }
        query.finish();
    }

    if (!db.commit()) {
        return rollbackFailure(db, db.lastError(), "commit transaction");
    }
    return MailError{};
}

} // namespace Migrations

} // namespace ningmail
