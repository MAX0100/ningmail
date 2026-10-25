// Implementation of the libetpan protocol backend. Every decidable piece
// (mUTF-7, roles, flags, envelope mapping, error classes) lives in
// LibetpanHelpers; this file keeps only session sequencing.

#include "LibetpanBackend.h"

#ifdef NINGMAIL_WITH_LIBETPAN

#include "MimeBuilder.h"
#include "MimeParser.h"

#include <algorithm>
#include <utility>

namespace ningmail {

LibetpanBackend::~LibetpanBackend() {
    disconnect();
}

MailError LibetpanBackend::connect(const Account &account, const QString &secret) {
    disconnect();
    if (account.imapHost.isEmpty() || account.smtpHost.isEmpty()) {
        return MailError{ErrorCode::Network, QString::fromUtf8("Account is not configured"),
                         QString::fromUtf8("libetpan")};
    }
    if (account.authType != AuthType::Password) {
        return MailError{ErrorCode::Auth, QString::fromUtf8("Login method not supported"),
                         QString::fromUtf8("password only in M1")};
    }

    m_account = account;
    m_login = account.username.isEmpty() ? account.email : account.username;
    m_sender = account.email;
    m_secret = secret; // same credential speaks SMTP (provider mandate)

    m_imap = std::make_unique<ImapSession>();
    mailimap *session = m_imap->get();
    const QByteArray hostUtf8 = account.imapHost.toUtf8();

    int r = MAILIMAP_ERROR_CONNECTION_REFUSED;
    switch (account.imapSecurity) {
    case Security::Tls:
        r = mailimap_ssl_connect(session, hostUtf8.constData(), account.imapPort);
        break;
    case Security::StartTls:
        r = mailimap_socket_connect(session, hostUtf8.constData(), account.imapPort);
        if (r == MAILIMAP_NO_ERROR) {
            r = mailimap_socket_starttls(session);
        }
        break;
    case Security::None:
    default:
        r = mailimap_socket_connect(session, hostUtf8.constData(), account.imapPort);
        break;
    }
    if (r != MAILIMAP_NO_ERROR) {
        m_imapConnected = false;
        m_imap->release();
        MailError error = errorFromMailimap(r);
        error.detail = QStringLiteral("imap %1:%2").arg(account.imapHost).arg(account.imapPort);
        return error;
    }

    const QByteArray loginUtf8 = m_login.toUtf8();
    const QByteArray secretUtf8 = secret.toUtf8();
    r = mailimap_login(session, loginUtf8.constData(), secretUtf8.constData());
    if (r != MAILIMAP_NO_ERROR) {
        m_imapConnected = false;
        m_imap->release();
        MailError error = errorFromMailimap(r);
        error.message = QString::fromUtf8("Login failed");
        return error;
    }

    m_imapConnected = true;
    // Best effort: fills the sidebar caches; LIST failures are not fatal
    // because every later operation re-runs refreshFolders (R88).
    (void)refreshFolders();
    return MailError{};
}

Result<QList<Folder>> LibetpanBackend::refreshFolders() {
    clist *listResult = nullptr;
    const int r = mailimap_list(m_imap->get(), "", "*", &listResult);
    if (r != MAILIMAP_NO_ERROR) {
        return Result<QList<Folder>>::failure(errorFromMailimap(r));
    }
    ImapMailboxList owned(listResult);
    QList<Folder> folders = foldersFromList(owned.get(), m_account.id);
    // Stable ids in fullName order so they survive reconnects and the
    // store keeps matching rows across runs.
    std::sort(folders.begin(), folders.end(), [](const Folder &left, const Folder &right) {
        return left.fullName < right.fullName;
    });
    m_rawMailboxes.clear();
    m_folderIds.clear();
    m_sentFolder.clear();
    for (int index = 0; index < folders.size(); ++index) {
        folders[index].id = index + 1;
        m_folderIds.insert(folders.at(index).fullName, index + 1);
        if (folders.at(index).role == FolderRole::Sent && m_sentFolder.isEmpty()) {
            m_sentFolder = folders.at(index).fullName;
        }
    }
    for (clistiter *it = clist_begin(owned.get()); it != nullptr; it = clist_next(it)) {
        const auto *mb = static_cast<const struct mailimap_mailbox_list *>(clist_content(it));
        if (mb == nullptr || mb->mb_name == nullptr) {
            continue;
        }
        // SELECT / APPEND reuse the wire spelling the server sent us.
        m_rawMailboxes.insert(decodeMailbox(QByteArray(mb->mb_name)), QByteArray(mb->mb_name));
    }
    return Result<QList<Folder>>::success(folders);
}

Result<QList<Folder>> LibetpanBackend::listFolders() {
    if (!m_imapConnected || m_imap == nullptr) {
        return Result<QList<Folder>>::failure(MailError{
            ErrorCode::Network, QString::fromUtf8("Not connected"), QString::fromUtf8("libetpan")});
    }
    return refreshFolders();
}

QByteArray LibetpanBackend::rawMailboxFor(const QString &fullName) {
    if (m_rawMailboxes.contains(fullName)) {
        return m_rawMailboxes.value(fullName);
    }
    // Unknown name (folder created elsewhere): refresh the cache once.
    (void)refreshFolders();
    if (m_rawMailboxes.contains(fullName)) {
        return m_rawMailboxes.value(fullName);
    }
    return encodeMailbox(fullName);
}

Result<QList<MessageHeader>> LibetpanBackend::fetchHeaders(const Folder &folder, quint32 fromUid,
                                                           int limit) {
    if (!m_imapConnected || m_imap == nullptr) {
        return Result<QList<MessageHeader>>::failure(MailError{
            ErrorCode::Network, QString::fromUtf8("Not connected"), QString::fromUtf8("libetpan")});
    }
    if (limit <= 0) {
        // A non-positive limit succeeds with an empty list (MailBackend).
        return Result<QList<MessageHeader>>::success({});
    }
    const QByteArray raw = rawMailboxFor(folder.fullName);
    mailimap *session = m_imap->get();
    // CONDSTORE-aware SELECT first; plain SELECT when the server refuses
    // (both fill session->imap_selection_info on success, verified 1.9.4).
    uint64_t modseq = 0;
    int r = mailimap_select_condstore(session, raw.constData(), &modseq);
    if (r != MAILIMAP_NO_ERROR) {
        r = mailimap_select(session, raw.constData());
    }
    if (r != MAILIMAP_NO_ERROR) {
        return Result<QList<MessageHeader>>::failure(errorFromMailimap(r));
    }
    // fetchBody has no folder argument; remember where we picked headers
    // from (⚠ single-folder limitation, see task report / T17).
    m_lastFetchFolder = QString::fromUtf8(raw.constData(), raw.size());

    // UID range covering `limit` messages from fromUid upward.
    quint32 lastUid = fromUid;
    if (limit > 1) {
        const quint32 span = static_cast<quint32>(limit - 1);
        lastUid = span > UINT32_MAX - fromUid ? UINT32_MAX : fromUid + span;
    }
    mailimap_set *set = mailimap_set_new_interval(fromUid, lastUid);
    mailimap_fetch_type *fetchType = mailimap_fetch_type_new_fetch_att_list_empty();
    clist *fetchResult = nullptr;
    Result<QList<MessageHeader>> result;
    if (set == nullptr || fetchType == nullptr) {
        result = Result<QList<MessageHeader>>::failure(
            MailError{ErrorCode::Storage, QString::fromUtf8("Out of memory"),
                      QString::fromUtf8("fetch headers")});
    } else {
        // ENVELOPE + FLAGS + INTERNALDATE + RFC822.SIZE + UID fill every
        // MessageHeader field we support (the body is fetched on demand).
        const bool added =
            mailimap_fetch_type_new_fetch_att_list_add(
                fetchType, mailimap_fetch_att_new_envelope()) == MAILIMAP_NO_ERROR &&
            mailimap_fetch_type_new_fetch_att_list_add(fetchType, mailimap_fetch_att_new_flags()) ==
                MAILIMAP_NO_ERROR &&
            mailimap_fetch_type_new_fetch_att_list_add(
                fetchType, mailimap_fetch_att_new_internaldate()) == MAILIMAP_NO_ERROR &&
            mailimap_fetch_type_new_fetch_att_list_add(
                fetchType, mailimap_fetch_att_new_rfc822_size()) == MAILIMAP_NO_ERROR &&
            mailimap_fetch_type_new_fetch_att_list_add(fetchType, mailimap_fetch_att_new_uid()) ==
                MAILIMAP_NO_ERROR;
        r = added ? mailimap_uid_fetch(session, set, fetchType, &fetchResult)
                  : MAILIMAP_ERROR_MEMORY;
        if (r == MAILIMAP_NO_ERROR) {
            ImapFetchList owned(fetchResult);
            result = Result<QList<MessageHeader>>::success(
                headersFromFetchList(owned.get(), m_account.id, folder.id, fromUid));
        } else {
            result = Result<QList<MessageHeader>>::failure(errorFromMailimap(r));
        }
    }
    // The fetch chain (attribute list included) is owned by fetchType.
    mailimap_fetch_type_free(fetchType);
    mailimap_set_free(set);
    return result;
}

Result<MessageBody> LibetpanBackend::fetchBody(const MessageHeader &header) {
    if (!m_imapConnected || m_imap == nullptr) {
        return Result<MessageBody>::failure(MailError{
            ErrorCode::Network, QString::fromUtf8("Not connected"), QString::fromUtf8("libetpan")});
    }
    if (m_lastFetchFolder.isEmpty()) {
        // No folder context: the interface passes none through (R70 note).
        return Result<MessageBody>::failure(MailError{
            ErrorCode::Protocol, QString::fromUtf8("fetchBody requires fetchHeaders first"),
            QString::fromUtf8("libetpan")});
    }
    mailimap *session = m_imap->get();
    const QByteArray raw = m_lastFetchFolder.toUtf8();
    // Full message BODY.PEEK[]: a NULL section renders the empty section so
    // libetpan's sender emits BODY.PEEK[] (verified 1.9.4).
    mailimap_set *set = mailimap_set_new_single(header.uid);
    mailimap_fetch_type *fetchType =
        mailimap_fetch_type_new_fetch_att(mailimap_fetch_att_new_body_peek_section(nullptr));
    clist *fetchResult = nullptr;
    const int r = mailimap_uid_fetch(session, set, fetchType, &fetchResult);
    Result<MessageBody> result;
    if (r != MAILIMAP_NO_ERROR) {
        result = Result<MessageBody>::failure(errorFromMailimap(r));
    } else {
        ImapFetchList owned(fetchResult);
        QByteArray rawMessage;
        for (clistiter *it = clist_begin(owned.get()); it != nullptr && rawMessage.isEmpty();
             it = clist_next(it)) {
            rawMessage =
                bodyFromMsgAtt(static_cast<const struct mailimap_msg_att *>(clist_content(it)));
        }
        if (rawMessage.isEmpty()) {
            // Round 1 item 1a: no matching BODY[] section (or an empty one)
            // is a protocol failure, never a silent empty body: cross-folder
            // uid mix-ups must fail loudly instead of showing a wrong or
            // blank message.
            result = Result<MessageBody>::failure(
                MailError{ErrorCode::Protocol,
                          QStringLiteral("No BODY[] content for uid %1 (folder %2)")
                              .arg(header.uid)
                              .arg(m_lastFetchFolder),
                          QString::fromUtf8("BODY[]")});
        } else {
            const auto parsed = MimeParser::parse(rawMessage);
            if (!parsed.ok()) {
                result = Result<MessageBody>::failure(MailError{
                    ErrorCode::Protocol, parsed.error.message, QString::fromUtf8("MimeParser")});
            } else if (const QString extractedId = extractRfcMessageId(rawMessage);
                       !header.rfcMessageId.isEmpty() &&
                       // Round 2 item 1: compare normalized ids so legal
                       // RFC 5322 CFWS comments on either side cannot fake a
                       // mismatch; the error message keeps the raw values.
                       normalizedMessageId(header.rfcMessageId) !=
                           normalizedMessageId(extractedId)) {
                // Round 1 item 1b: the ENVELOPE stamp and the real header
                // must agree, otherwise a cross-folder uid mix-up slipped
                // through — fail with both ids in the message.
                result = Result<MessageBody>::failure(
                    messageIdMismatchError(header.rfcMessageId, extractedId));
            } else {
                MessageBody body;
                body.messageId = header.id; // id ≡ uid carries through (R70 note)
                body.contentType = QString::fromUtf8("text/plain");
                body.charset = QString::fromUtf8("utf-8");
                body.html = parsed.value->html;
                body.plainText = parsed.value->plainText;
                result = Result<MessageBody>::success(std::move(body));
            }
        }
    }
    mailimap_fetch_type_free(fetchType);
    mailimap_set_free(set);
    return result;
}

MailError LibetpanBackend::openSmtp() {
    if (m_smtpConnected && m_smtp != nullptr) {
        return MailError{};
    }
    // Round 1 item 4: build the session in a local RAII holder, so every
    // failure path below tears it down in its own scope and no half-connect-
    // ed session is ever parked in m_smtp for the next call to reuse.
    std::unique_ptr<SmtpSession> session = std::make_unique<SmtpSession>();
    mailsmtp *smtp = session->get();
    const QByteArray smtpHost = m_account.smtpHost.toUtf8();

    int r = MAILSMTP_ERROR_CONNECTION_REFUSED;
    switch (m_account.smtpSecurity) {
    case Security::Tls:
        r = mailsmtp_ssl_connect(smtp, smtpHost.constData(), m_account.smtpPort);
        break;
    case Security::StartTls:
    case Security::None:
    default:
        r = mailsmtp_socket_connect(smtp, smtpHost.constData(), m_account.smtpPort);
        break;
    }
    if (r != MAILSMTP_NO_ERROR) {
        return errorFromMailsmtp(r);
    }
    // EHLO with HELO fallback (mailsmtp_init in 1.9.4).
    r = mailsmtp_init(smtp);
    if (r != MAILSMTP_NO_ERROR) {
        return errorFromMailsmtp(r);
    }
    if (m_account.smtpSecurity == Security::StartTls) {
        if ((smtp->esmtp & MAILSMTP_ESMTP_STARTTLS) == 0) {
            return MailError{ErrorCode::Tls, QString::fromUtf8("SMTP STARTTLS not supported"),
                             m_account.smtpHost};
        }
        // Round 1 item 6 — 1.9.4 STARTTLS sequencing: mailsmtp_socket_starttls
        // sends the STARTTLS command (the capability advertised by the EHLO
        // above) and switches the stream to TLS; extension negotiation then
        // happens over the plain stream only until the second mailsmtp_init
        // re-greets on the TLS stream. Known (documented) ordering assumption
        // for M1: esmtp flags read before the upgrade belong to the pre-TLS
        // handshake and are re-assessed by that second init; nothing is sent
        // between the upgrade and the re-EHLO. Reverify against real QQ/163
        // servers (task ledger T17).
        r = mailsmtp_socket_starttls(smtp);
        if (r != MAILSMTP_NO_ERROR) {
            return errorFromMailsmtp(r);
        }
        // Say hello again over the upgraded stream (verified 1.9.4 flow).
        r = mailsmtp_init(smtp);
        if (r != MAILSMTP_NO_ERROR) {
            return errorFromMailsmtp(r);
        }
    }
    const QByteArray loginUtf8 = m_login.toUtf8();
    const QByteArray secretUtf8 = m_secret.toUtf8();
    r = mailsmtp_auth(smtp, loginUtf8.constData(), secretUtf8.constData());
    if (r != MAILSMTP_NO_ERROR) {
        return errorFromMailsmtp(r);
    }
    m_smtp = std::move(session); // only the fully connected session is kept
    m_smtpConnected = true;
    return MailError{};
}

MailError LibetpanBackend::send(const OutgoingMessage &message) {
    const MailError started = openSmtp();
    if (started) {
        return started;
    }
    mailsmtp *smtp = m_smtp->get();
    const QByteArray from = m_sender.toUtf8();
    const QByteArray mime = MimeBuilder::build(message);

    clist *addresses = smtp_address_list_new();
    if (addresses == nullptr) {
        return MailError{ErrorCode::Storage, QString::fromUtf8("Out of memory"),
                         QString::fromUtf8("send recipients")};
    }
    for (const MailAddress &address : message.to + message.cc + message.bcc) {
        if (address.address.isEmpty()) {
            continue;
        }
        smtp_address_list_add(addresses, strdup(address.address.toUtf8().constData()));
    }
    // ESMTP servers get SIZE/DSN-aware sending; plain ones use
    // MAIL/RCPT/DATA directly (mailsmtp_send) via the esmtp flag.
    const int r = smtp->esmtp != 0
                      ? mailesmtp_send(smtp, from.constData(), 0, nullptr, addresses,
                                       mime.constData(), static_cast<size_t>(mime.size()))
                      : mailsmtp_send(smtp, from.constData(), addresses, mime.constData(),
                                      static_cast<size_t>(mime.size()));
    smtp_address_list_free(addresses); // addresses are strdup'ed copies
    if (r != MAILSMTP_NO_ERROR) {
        return errorFromMailsmtp(r);
    }
    return MailError{};
}

MailError LibetpanBackend::appendSent(const OutgoingMessage &message, const QByteArray &rawMime) {
    Q_UNUSED(message)
    if (!m_imapConnected || m_imap == nullptr) {
        return MailError{ErrorCode::Network, QString::fromUtf8("Not connected"),
                         QString::fromUtf8("libetpan")};
    }
    if (m_sentFolder.isEmpty()) {
        // Cached at connect; refresh once in case a Sent folder appeared.
        (void)refreshFolders();
    }
    if (m_sentFolder.isEmpty()) {
        return MailError{ErrorCode::Protocol, QString::fromUtf8("Sent folder not found"),
                         QString::fromUtf8("libetpan")};
    }
    const QByteArray raw = rawMailboxFor(m_sentFolder);
    mailimap_flag_list *flagList = mailimap_flag_list_new_empty();
    if (flagList == nullptr) {
        return MailError{ErrorCode::Storage, QString::fromUtf8("Out of memory"),
                         QString::fromUtf8("append sent")};
    }
    struct FlagListGuard {
        mailimap_flag_list *list;
        ~FlagListGuard() {
            if (list != nullptr) {
                mailimap_flag_list_free(list);
            }
        }
    } guard{flagList};
    (void)mailimap_flag_list_add(flagList, mailimap_flag_new_seen());
    // Date stays NULL (internal date assigned by the server).
    const int r = mailimap_append(m_imap->get(), raw.constData(), flagList, nullptr,
                                  rawMime.constData(), static_cast<size_t>(rawMime.size()));
    if (r != MAILIMAP_NO_ERROR) {
        return errorFromMailimap(r);
    }
    return MailError{};
}

void LibetpanBackend::disconnect() {
    m_imapConnected = false;
    m_smtpConnected = false;
    m_secret.clear();
    m_lastFetchFolder.clear();
    m_sentFolder.clear();
    m_rawMailboxes.clear();
    m_folderIds.clear();
    // SMTP first (its QUIT lands fastest), then IMAP (logout + free).
    m_smtp.reset();
    m_imap.reset();
}

} // namespace ningmail

#endif // NINGMAIL_WITH_LIBETPAN
