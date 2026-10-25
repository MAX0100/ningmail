#pragma once

#include "MailError.h"
#include "Types.h"

#include <QDateTime>
#include <QList>
#include <QMetaType>

namespace ningmail {

class Database;

/// Lifecycle of one outbox row (outbox.state, INTEGER; values follow the C++
/// declaration order per the schema-v1 convention): Pending → Sending →
/// Sent, or Sending → Retry (next sendNow attempt) / Failed. Updating from
/// Pending straight to Failed happens when the account snapshot is missing.
enum class OutboxState { Pending, Sending, Sent, Retry, Failed };

/// One queued outgoing message of the `outbox` table.
struct OutboxItem {
    qint64 id = 0;
    AccountId accountId = 0;
    QList<MailAddress> recipients;
    QList<MailAddress> cc;
    QString subject;
    QString bodyText;
    OutboxState state = OutboxState::Pending;
    int attempts = 0;
    /// Null in M1: no retry scheduler exists yet, so the column stays unset.
    QDateTime nextRetryAt;
    QString lastError;
    QString rfcMessageId;
    QByteArray rawMime;
};

/// CRUD for the `outbox` table. Address lists use the same JSON convention as
/// the drafts table (see DraftStore.cpp); raw_mime round-trips as a BLOB.
class OutboxStore {
  public:
    explicit OutboxStore(Database &database);

    /// Inserts a new item when item.id <= 0 and returns the new row id;
    /// updates the existing row in place otherwise (a missing id is an error,
    /// never an upsert). The account must exist (foreign key). created_at is
    /// store-owned bookkeeping, always the current UTC time in milliseconds.
    Result<qint64> save(const OutboxItem &item);
    /// Missing ids fail with a Storage error.
    Result<OutboxItem> get(qint64 id) const;
    /// Sendable queue of one account: state IN (Pending, Retry), oldest first
    /// (created_at ASC, id ASC — the id breaks same-millisecond ties).
    Result<QList<OutboxItem>> listPending(AccountId accountId) const;
    /// Writes state and last_error; `error` is the final value, so an empty
    /// string clears the column. Attempts and next_retry_at are untouched.
    /// Unknown ids fail with a Storage error. 状态转换合法性由上层
    /// SendService 保证（本 store 层为通用 CRUD，不校验状态图）。
    MailError updateState(qint64 id, OutboxState state, const QString &error = {});
    /// Bumps attempts by exactly one; unknown ids fail with a Storage error.
    MailError bumpAttempts(qint64 id);
    /// Deletes one row; success is the default MailError{} (code None).
    MailError remove(qint64 id);

  private:
    Database &m_database;
};

} // namespace ningmail

Q_DECLARE_METATYPE(ningmail::OutboxState)
