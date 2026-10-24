#pragma once

#include "MailError.h"
#include "Types.h"

#include <QList>

#include <optional>

namespace ningmail {

class Database;

/// Keyset pagination cursor: list only messages strictly older than this one.
struct MessageCursor {
    qint64 sentAtMs;
    MessageId id;
};

/// CRUD for the `messages` and `bodies` tables. Identity is (folder_id, uid);
/// duplicates update in place. The list page is the M1 inbox view and uses
/// keyset pagination over (sent_at DESC, id DESC).
class MessageStore {
  public:
    explicit MessageStore(Database &database);

    /// Inserts a header, or updates flags/subject/snippet/body_state of the
    /// existing row when (folderId, uid) already exists; returns the row id.
    Result<MessageId> insert(const MessageHeader &header);
    /// Applies insert() to every header in one transaction; rolls back and
    /// returns the first error.
    MailError insertBatch(const QList<MessageHeader> &headers);

    MailError updateFlags(MessageId id, MailFlags flags);
    MailError setBodyState(MessageId id, BodyState state);

    Result<MessageHeader> header(MessageId id) const;
    /// Newest-first page for a folder; `before` continues an earlier page.
    Result<QList<MessageHeader>> list(FolderId folderId, int limit,
                                      std::optional<MessageCursor> before = std::nullopt) const;
    int count(FolderId folderId) const;
    int unreadCount(FolderId folderId) const;

    /// Inserts or replaces the body row of `body.messageId`.
    MailError saveBody(const MessageBody &body);
    /// Empty body when nothing has been fetched yet; empty content, not an error.
    Result<MessageBody> body(MessageId id) const;

    /// Deletes every message of a folder; bodies and attachments cascade.
    MailError clearFolder(FolderId folderId);

  private:
    Database &m_database;
};

} // namespace ningmail
