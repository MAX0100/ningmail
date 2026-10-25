// Pure helpers and RAII wrappers for the libetpan protocol adapter
// (task 13, R86): everything decidable without a live server lives here and
// is unit-tested against hand-built libetpan C structures. The
// protocol-speaking wrapper (LibetpanBackend) is a thin shell over these.

#pragma once

#include "app/core/MailError.h"
#include "app/core/Types.h"

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QString>

#ifdef NINGMAIL_WITH_LIBETPAN

// libetpan's C headers must land at global scope (their symbols are
// extern "C"); the libetpan-spoken helpers below therefore come after it.
#include <libetpan/libetpan.h>

#endif // NINGMAIL_WITH_LIBETPAN

namespace ningmail {

// These are pure Qt transforms and usable without libetpan.
QString decodeMailbox(const QByteArray &value);
QByteArray encodeMailbox(const QString &value);
FolderRole folderRoleFromName(const QString &fullName, const QList<QString> &flags = {});
/// Prunes unknown bits from an already-MailFlags bit pattern (round 1 item
/// 3: msgAttFlagsToMask produces MailFlags directly, so no libetpan enum
/// re-encoding happens here — keywords/foreign bits cannot leak into the
/// core set).
MailFlags mailFlagsFromLibetpan(uint32_t flags);
QString formatAddress(const MailAddress &address);
MailError errorFromMailimap(int code);
MailError errorFromMailsmtp(int code);

/// Extracts the RFC 5322 Message-ID from the header region of raw message
/// bytes (round 1 item 1b, refined in round 2): matching is case-insensitive,
/// folded lines are unfolded before matching, and the first non-empty
/// <...> token is returned as-is (surrounding RFC 5322 CFWS comments are
/// ignored); without such a token the trimmed line value is returned
/// (weird-server tolerance); missing ids return an empty string.
QString extractRfcMessageId(const QByteArray &raw);

/// Comparison-side normalization (round 2 item 1): the inner token of the
/// first <...> pair (empty pair → empty string), or the trimmed value when
/// no angle pair exists. fetchBody compares normalized ids and keeps the
/// raw ones for the error message.
QString normalizedMessageId(const QString &value);

/// Builds the "loud failure" error for an envelope/body Message-ID mismatch
/// (round 1 item 1b): both ids end up in the message, an empty extracted id
/// shows up as "(none)".
MailError messageIdMismatchError(const QString &envelopeId, const QString &extractedId);

#ifdef NINGMAIL_WITH_LIBETPAN

// libetpan lists and envelopes are only pronounced by the adapter, so the
// remaining helpers plus RAII wrappers join the build together with it.

/// Maps a FLAGS fetch response directly onto MailFlags (round 1 item 3): the
/// switch below is the single place that knows both encodings, so no fragile
/// intermediate libetpan-bit mask exists anymore. KEYWORD/EXTENSION/RECENT
/// enter as no MailFlag.
MailFlags msgAttFlagsToMask(const struct mailimap_msg_att_dynamic *att);

/// Converts an IMAP INTERNALDATE into a UTC QDateTime; NULL yields an
/// invalid QDateTime so missing dates stay explicit.
QDateTime dateFromInternaldate(const struct mailimap_date_time *date);

/// Builds a header from a server-provided ENVELOPE. Message ids are stamped
/// from the FETCH uid afterwards for FakeMailBackend parity; thread ids fall
/// back to the RFC 822 message id (no References run yet, M1).
MessageHeader headerFromEnvelope(const struct mailimap_envelope *envelope, qint64 accountId,
                                 qint64 folderId);

/// Builds headers from a uid-FETCH response: ascending ids and only
/// uid >= fromUid rows (the row cap lives in the backend, which limits the
/// fetch set).
QList<MessageHeader> headersFromFetchList(const clist *attrs, qint64 accountId, qint64 folderId,
                                          quint32 fromUid);

/// Builds folders from a LIST response; names are mUTF-7 decoded and roles
/// assigned. Mailbox ids stay 0 - the backend assigns stable ids.
QList<Folder> foldersFromList(const clist *list, qint64 accountId);

/// Extracts the BODY[] payload (raw RFC 822 bytes) of one FETCH attribute.
QByteArray bodyFromMsgAtt(const struct mailimap_msg_att *att);

/// Owns an IMAP session; logout plus free happen at scope exit even when an
/// operation failed halfway through connect (brief step 3, RAII).
class ImapSession {
  public:
    ImapSession();
    ~ImapSession();
    ImapSession(const ImapSession &) = delete;
    ImapSession &operator=(const ImapSession &) = delete;

    mailimap *get() const;
    /// Drops the session without logging out (connect failure path).
    void release();
    /// Logs out and frees the session; safe to call twice.
    void close();

  private:
    mailimap *m_session;
};

/// Owns an SMTP session; mailsmtp_free sends the trailing QUIT if the
/// connection is still open.
class SmtpSession {
  public:
    SmtpSession();
    ~SmtpSession();
    SmtpSession(const SmtpSession &) = delete;
    SmtpSession &operator=(const SmtpSession &) = delete;

    mailsmtp *get() const;
    void release();

  private:
    mailsmtp *m_session;
};

/// Frees a uid-FETCH clist (mailimap_fetch_list_free) at scope exit.
class ImapFetchList {
  public:
    explicit ImapFetchList(clist *list);
    ~ImapFetchList();
    ImapFetchList(const ImapFetchList &) = delete;
    ImapFetchList &operator=(const ImapFetchList &) = delete;

    const clist *get() const;

  private:
    clist *m_list;
};

/// Frees a LIST clist (mailimap_list_result_free) at scope exit.
class ImapMailboxList {
  public:
    explicit ImapMailboxList(clist *list);
    ~ImapMailboxList();
    ImapMailboxList(const ImapMailboxList &) = delete;
    ImapMailboxList &operator=(const ImapMailboxList &) = delete;

    const clist *get() const;

  private:
    clist *m_list;
};

#endif // NINGMAIL_WITH_LIBETPAN

} // namespace ningmail
