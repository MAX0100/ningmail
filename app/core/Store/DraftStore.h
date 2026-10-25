#pragma once

#include "MailError.h"
#include "Types.h"

#include <QList>

namespace ningmail {

class Database;

/// One autosaved plain-text draft of the `drafts` table.
struct Draft {
    qint64 id = 0;
    AccountId accountId = 0;
    QString subject;
    QString bodyText;
    QList<MailAddress> recipients;
    QList<MailAddress> cc;
    qint64 updatedAt = 0;
};

/// CRUD for the `drafts` table. save() always stamps updatedAt with the
/// current UTC time in milliseconds (R29) so list() can order by
/// updated_at DESC, id DESC — the id breaks ties between same-millisecond
/// saves, keeping the order stable (R78).
class DraftStore {
  public:
    explicit DraftStore(Database &database);

    /// Inserts a new draft when draft.id <= 0 and returns the new row id;
    /// updates the existing row in place otherwise (a missing id is an error,
    /// never an upsert). The account must exist (foreign key).
    Result<qint64> save(const Draft &draft);
    /// Missing ids fail with a Storage error.
    Result<Draft> get(qint64 id) const;
    /// Deletes one row; success is the default MailError{} (code None).
    MailError remove(qint64 id);
    /// Newest-first page for one account; same-millisecond rows order by id.
    Result<QList<Draft>> list(AccountId accountId) const;

  private:
    Database &m_database;
};

} // namespace ningmail
