// libetpan-backed MailBackend (task 13): IMAP4 (TLS / STARTTLS / plain)
// plus SMTP send, on top of the pure helpers. All the decode/copy/error
// logic lives in LibetpanHelpers; this class only drives the protocol.

#pragma once

#include "LibetpanHelpers.h"
#include "app/core/MailBackend.h"

#include <QByteArray>
#include <QHash>
#include <QString>

#include <memory>

#ifdef NINGMAIL_WITH_LIBETPAN

namespace ningmail {

/// Real protocol backend. One instance holds at most one IMAP and one SMTP
/// session; folders are cached per connection keyed by their decoded name
/// so SELECT uses the server's own mUTF-7 spelling.
class LibetpanBackend final : public MailBackend {
  public:
    LibetpanBackend() = default;
    ~LibetpanBackend() override;

    LibetpanBackend(const LibetpanBackend &) = delete;
    LibetpanBackend &operator=(const LibetpanBackend &) = delete;

    // MailBackend interface (R88): TLS picks the implicit-SSL connect,
    // StartTls upgrades the plain socket, None is plain.
    MailError connect(const Account &account, const QString &secret) override;
    Result<QList<Folder>> listFolders() override;
    // First headers with uid >= fromUid, capped at `limit` (fetch set).
    Result<QList<MessageHeader>> fetchHeaders(const Folder &folder, quint32 fromUid,
                                              int limit) override;
    Result<MessageBody> fetchBody(const MessageHeader &header) override;
    MailError send(const OutgoingMessage &message) override;
    MailError appendSent(const OutgoingMessage &message, const QByteArray &rawMime) override;
    void disconnect() override;

  private:
    /// Redoes LIST and rebuilds the mailbox caches; ids are assigned
    /// 1..n in fullName order so they survive reconnects.
    Result<QList<Folder>> refreshFolders();
    /// Wire spelling of a mailbox; falls back to self-encoding.
    QByteArray rawMailboxFor(const QString &fullName);
    /// Connects + greets + authenticates SMTP on first send.
    MailError openSmtp();

    std::unique_ptr<ImapSession> m_imap;
    std::unique_ptr<SmtpSession> m_smtp;
    bool m_imapConnected = false;
    bool m_smtpConnected = false;

    Account m_account;
    QString m_login;  // username or email (IMAP LOGIN)
    QString m_sender; // MAIL FROM identity
    QString m_secret; // taken from connect(), cleared by disconnect()

    QHash<QString, QByteArray> m_rawMailboxes; // decoded fullName → wire name
    QHash<QString, qint64> m_folderIds;        // decoded fullName → folder id
    /// Raw (mUTF-7) name of the folder of the last successful fetchHeaders:
    /// the MailBackend interface gives fetchBody no folder back-reference
    /// (⚠ documented in the task report, real-device note T17).
    /// Round 1 item 7: mUTF-7 names are pure ASCII on the wire by definition
    /// (the malformed-input fallback keeps server bytes verbatim), so the
    /// fromUtf8() store here and the toUtf8() reclaim in fetchBody are
    /// byte-identical roundtrips — encoding drift cannot swap the folder.
    QString m_lastFetchFolder;
    QString m_sentFolder; // decoded name of the Sent folder (for APPEND)
};

#endif // NINGMAIL_WITH_LIBETPAN

} // namespace ningmail
