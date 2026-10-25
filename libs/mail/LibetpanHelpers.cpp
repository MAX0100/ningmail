// Implementation of the libetpan pure helpers (task 13). The contracts live
// in LibetpanHelpers.h; everything here is either a pure transform of
// hand-buildable libetpan C structures into core types or an RAII wrapper
// for a protocol session/result.

#include "LibetpanHelpers.h"

namespace ningmail {

// Pure Qt section (no libetpan): header probes and error builders any
// backend can reuse (round 1 item 1b).

QString extractRfcMessageId(const QByteArray &raw) {
    // The header region ends at the first empty line ("\r\n\r\n" or "\n\n").
    qsizetype headerEnd = raw.size();
    for (qsizetype i = 0; i < raw.size(); ++i) {
        if (raw.at(i) == '\r' && raw.mid(i, 4) == QByteArrayLiteral("\r\n\r\n")) {
            headerEnd = i;
            break;
        }
        if (raw.at(i) == '\n' && raw.mid(i, 2) == QByteArrayLiteral("\n\n")) {
            headerEnd = i;
            break;
        }
    }
    // Split into logical lines, unfolding continuations per RFC 5322 2.2.3:
    // a CRLF (or LF) followed by WSP vanishes, the WSP stays within the line.
    QStringList lines;
    QByteArray current;
    qsizetype i = 0;
    const auto flush = [&lines, &current] {
        if (!current.isEmpty()) {
            lines.append(QString::fromLatin1(current.constData(), current.size()));
            current.clear();
        }
    };
    while (i < headerEnd) {
        const qsizetype lineStart = i;
        while (i < headerEnd && raw.at(i) != '\n') {
            ++i;
        }
        qsizetype lineEnd = i;
        if (lineEnd > lineStart && raw.at(lineEnd - 1) == '\r') {
            --lineEnd; // strip the CR of a CRLF
        }
        current += QByteArray(raw.constData() + lineStart, lineEnd - lineStart);
        ++i; // consume the LF (or the end of the header region)
        if (i >= headerEnd || (raw.at(i) != ' ' && raw.at(i) != '\t')) {
            flush();
        }
    }
    flush();

    for (const QString &line : lines) {
        if (!line.startsWith(QLatin1String("Message-Id:"), Qt::CaseInsensitive)) {
            continue;
        }
        const QString value = line.mid(line.indexOf(u':') + 1).trimmed();
        // Round 2 item 1: CFWS comments may surround the id ("(note) <id@x>
        // (comment)"); the first <...> token with content is the id itself,
        // plain line values only serve as the fallback for weird servers
        // (an empty "<>" pair does not count, see normalizedMessageId).
        qsizetype open = value.indexOf(u'<');
        while (open >= 0) {
            const qsizetype close = value.indexOf(u'>', open + 1);
            if (close < 0) {
                break;
            }
            if (close > open + 1) {
                return value.mid(open, close - open + 1);
            }
            // "<>": not an id token, keep looking past this angle pair.
            const qsizetype next = value.indexOf(u'<', close + 1);
            if (next < 0) {
                break;
            }
            // The loop condition re-reads open from the updated index.
            open = next;
        }
        return value;
    }
    return {};
}

QString normalizedMessageId(const QString &value) {
    // Round 2 item 1: comparison topcoat — the inner token between the first
    // '<' and the first '>' (an empty "<>" pair normalizes to empty), or the
    // trimmed raw value when no angle pair exists.
    const qsizetype open = value.indexOf(u'<');
    if (open >= 0) {
        const qsizetype close = value.indexOf(u'>', open + 1);
        if (close > open) {
            return value.mid(open + 1, close - open - 1);
        }
    }
    return value.trimmed();
}

MailError messageIdMismatchError(const QString &envelopeId, const QString &extractedId) {
    return MailError{
        ErrorCode::Protocol,
        QStringLiteral("Message-ID mismatch: envelope %1, body %2")
            .arg(envelopeId, extractedId.isEmpty() ? QStringLiteral("(none)") : extractedId),
        QStringLiteral("extractRfcMessageId")};
}

} // namespace ningmail

#ifdef NINGMAIL_WITH_LIBETPAN

#include "MimeParser.h"

#include <libetpan/libetpan.h>

#include <QChar>
#include <QHash>
#include <QStringList>
#include <QTimeZone>
#include <QVarLengthArray>
#include <algorithm>
#include <optional>

namespace ningmail {

namespace {

// RFC 3501 5.1.3 modified BASE64 alphabet: 0-25 A-Z, 26-51 a-z, 52-61 0-9,
// 62 '+' and 63 ','.
constexpr char16_t kMutf7Alphabet[] =
    u"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+,";

int decodeSextet(char value) {
    if (value >= 'A' && value <= 'Z') {
        return value - 'A';
    }
    if (value >= 'a' && value <= 'z') {
        return value - 'a' + 26;
    }
    if (value >= '0' && value <= '9') {
        return value - '0' + 52;
    }
    if (value == '+') {
        return 62;
    }
    if (value == ',') {
        return 63;
    }
    return -1;
}

/// Interprets big-endian UTF-16 bytes; surrogate pairs fall out naturally.
QString utf16FromBytes(const QByteArray &bytes) {
    const qsizetype units = bytes.size() / 2;
    QVarLengthArray<char16_t, 16> buffer(units);
    for (qsizetype unit = 0; unit < units; ++unit) {
        const uchar highByte = static_cast<uchar>(bytes.at(unit * 2));
        const uchar lowByte = static_cast<uchar>(bytes.at(unit * 2 + 1));
        buffer[unit] = static_cast<char16_t>(highByte << 8 | lowByte);
    }
    return QString::fromUtf16(buffer.constData(), units);
}

QString envelopeText(const char *value) {
    // ENVELOPE strings arrive undecoded; values are commonly RFC 2047
    // encoded, so the MimeParser weightlifting is reused here (R86).
    return value == nullptr ? QString() : MimeParser::decodeHeader(QByteArray(value));
}

MailAddress addressFromImap(const struct mailimap_address *addr) {
    if (addr == nullptr) {
        return MailAddress{};
    }
    const QString mailbox = envelopeText(addr->ad_mailbox_name);
    if (mailbox.isEmpty()) {
        // Group syntax marker (or a malformed address): nothing to show.
        return MailAddress{};
    }
    MailAddress result;
    result.name = envelopeText(addr->ad_personal_name);
    const QString host = envelopeText(addr->ad_host_name);
    // Some servers put the full address into the mailbox part (relay lists);
    // joining that with the host would duplicate the domain.
    result.address = host.isEmpty() || mailbox.contains(u'@') ? mailbox : mailbox + u'@' + host;
    return result;
}

QList<MailAddress> addressListFromImap(const clist *list) {
    QList<MailAddress> addresses;
    if (list == nullptr) {
        return addresses;
    }
    for (clistiter *it = clist_begin(list); it != nullptr; it = clist_next(it)) {
        const auto *addr = static_cast<const struct mailimap_address *>(clist_content(it));
        const MailAddress address = addressFromImap(addr);
        if (!address.address.isEmpty()) {
            addresses.append(address);
        }
    }
    return addresses;
}

struct AliasEntry {
    const char *alias;
    FolderRole role;
};

// English folder names as used by Dovecot, Gmail, Exchange and friends.
const AliasEntry kEnglishAliases[] = {
    {"inbox", FolderRole::Inbox},
    {"sent", FolderRole::Sent},
    {"sent items", FolderRole::Sent},
    {"sent mail", FolderRole::Sent},
    {"sent messages", FolderRole::Sent},
    {"drafts", FolderRole::Drafts},
    {"draft", FolderRole::Drafts},
    {"trash", FolderRole::Trash},
    {"deleted", FolderRole::Trash},
    {"deleted items", FolderRole::Trash},
    {"deleted messages", FolderRole::Trash},
    {"bin", FolderRole::Trash},
    {"junk", FolderRole::Junk},
    {"junk e-mail", FolderRole::Junk},
    {"junk email", FolderRole::Junk},
    {"spam", FolderRole::Junk},
    {"archive", FolderRole::Archive},
    {"archives", FolderRole::Archive},
};

// Coremail / QQ / 163 / 126 Chinese folder names.
const AliasEntry kChineseAliases[] = {
    {"\xE6\x94\xB6\xE4\xBB\xB6\xE7\xAE\xB1", FolderRole::Inbox}, // 收件箱
    {"\xE5\xB7\xB2\xE5\x8F\x91\xE9\x80\x81", FolderRole::Sent},  // 已发送
    {"\xE5\xB7\xB2\xE5\x8F\x91\xE9\x80\x81\xE9\x82\xAE\xE4\xBB\xB6",
     FolderRole::Sent},                                           // 已发送邮件
    {"\xE5\x8F\x91\xE4\xBB\xB6\xE7\xAE\xB1", FolderRole::Sent},   // 发件箱
    {"\xE8\x8D\x89\xE7\xA8\xBF\xE7\xAE\xB1", FolderRole::Drafts}, // 草稿箱
    {"\xE8\x8D\x89\xE7\xA8\xBF", FolderRole::Drafts},             // 草稿
    {"\xE5\xB7\xB2\xE5\x88\xA0\xE9\x99\xA4", FolderRole::Trash},  // 已删除
    {"\xE5\xB7\xB2\xE5\x88\xA0\xE9\x99\xA4\xE9\x82\xAE\xE4\xBB\xB6",
     FolderRole::Trash},                                                    // 已删除邮件
    {"\xE5\xBA\x9F\xE4\xBB\xB6\xE7\xAE\xB1", FolderRole::Trash},            // 废件箱
    {"\xE5\x9B\x9E\xE6\x94\xB6\xE7\xAB\x99", FolderRole::Trash},            // 回收站
    {"\xE5\x9E\x83\xE5\x9C\xBE\xE9\x82\xAE\xE4\xBB\xB6", FolderRole::Junk}, // 垃圾邮件
    {"\xE5\x9E\x83\xE5\x9C\xBE\xE7\xAE\xB1", FolderRole::Junk},             // 垃圾箱
    {"\xE7\x97\x85\xE6\xAF\x92\xE9\x82\xAE\xE4\xBB\xB6", FolderRole::Junk}, // 病毒邮件
    {"\xE5\xAD\x98\xE6\xA1\xA3", FolderRole::Archive},                      // 存档
    {"\xE5\xBD\x92\xE6\xA1\xA3", FolderRole::Archive},                      // 归档
};

/// Matches a (lowercased) value against the alias tables.
std::optional<FolderRole> aliasRole(const QString &value) {
    static const QHash<QString, FolderRole> aliases = [] {
        QHash<QString, FolderRole> table;
        for (const AliasEntry &entry : kEnglishAliases) {
            table.insert(QString::fromUtf8(entry.alias), entry.role);
        }
        for (const AliasEntry &entry : kChineseAliases) {
            table.insert(QString::fromUtf8(entry.alias), entry.role);
        }
        return table;
    }();
    const auto found = aliases.constFind(value);
    return found == aliases.constEnd() ? std::nullopt : std::optional<FolderRole>(found.value());
}

/// Splits a (lowercased) folder name into hierarchy segments: '/', '.' and
/// '\' all count (separators never mix inside one namespace).
QStringList folderSegments(const QString &lowerName) {
    QStringList segments;
    for (const QChar ch : lowerName) {
        if (ch == u'/' || ch == u'.' || ch == u'\\') {
            segments.append(QString());
            continue;
        }
        if (segments.isEmpty()) {
            segments.append(QString());
        }
        segments.last().append(ch);
    }
    while (!segments.isEmpty() && segments.last().isEmpty()) {
        segments.removeLast();
    }
    return segments;
}

} // namespace

QString decodeMailbox(const QByteArray &value) {
    QString result;
    result.reserve(value.size());
    qsizetype i = 0;
    bool malformed = false;
    while (i < value.size() && !malformed) {
        const char ch = value.at(i);
        if (ch != '&') {
            result.append(QLatin1Char(ch));
            ++i;
            continue;
        }
        if (i + 1 >= value.size()) {
            // A bare '&': never valid (RFC calls for the "&-" escape).
            malformed = true;
            break;
        }
        if (value.at(i + 1) == '-') {
            result.append(u'&');
            i += 2;
            continue;
        }
        qsizetype runEnd = i + 1;
        while (runEnd < value.size() && value.at(runEnd) != '-') {
            ++runEnd;
        }
        if (runEnd >= value.size()) {
            malformed = true; // unterminated BASE64 run
            break;
        }
        if (runEnd == i + 2) {
            malformed = true; // one BASE64 char can never build a 16-bit unit
            break;
        }
        quint32 bitBuffer = 0;
        int filled = 0;
        QByteArray bytes;
        bytes.reserve(static_cast<qsizetype>(runEnd - i - 1) * 3 / 4 + 1);
        bool runOk = true;
        for (qsizetype k = i + 1; k < runEnd; ++k) {
            const int sextet = decodeSextet(value.at(k));
            if (sextet < 0) {
                runOk = false;
                break;
            }
            bitBuffer = (bitBuffer << 6) | static_cast<quint32>(sextet);
            filled += 6;
            if (filled >= 8) {
                filled -= 8;
                bytes.append(static_cast<char>((bitBuffer >> filled) & 0xFFu));
            }
        }
        // Padding bits stay low-ordered and are discarded per the RFC; an
        // odd byte count is not a whole run of UTF-16 units.
        if (!runOk || bytes.size() % 2 != 0) {
            malformed = true;
            break;
        }
        result.append(utf16FromBytes(bytes));
        i = runEnd + 1; // skip the '-'
    }
    if (malformed) {
        // RFC 3501 5.1.3 example: "&Jjo!" is invalid; keep the raw bytes so a
        // broken server-side name stays recognizable instead of being
        // silently mis-decoded.
        return QString::fromUtf8(value.constData(), value.size());
    }
    return result;
}

QByteArray encodeMailbox(const QString &value) {
    QByteArray result;
    result.reserve(value.size() * 2);
    quint32 bitBuffer = 0;
    int filled = 0;
    bool inBase64 = false;
    const auto closeRun = [&]() {
        if (!inBase64) {
            return;
        }
        inBase64 = false;
        if (filled > 0) {
            result.append(static_cast<char>(kMutf7Alphabet[(bitBuffer << (6 - filled)) & 0x3F]));
        }
        result.append('-');
        bitBuffer = 0;
        filled = 0;
    };
    for (const QChar unit : value) {
        // Printable US-ASCII (0x20-0x7E) goes through raw; control bytes and
        // everything non-ASCII (surrogate halves included) join the run.
        const bool needsBase64 = unit.unicode() < 0x20 || unit.unicode() >= 0x7F;
        if (!needsBase64) {
            closeRun();
            if (unit.unicode() == u'&') {
                result.append("&-");
                continue;
            }
            result.append(static_cast<char>(unit.unicode()));
            continue;
        }
        if (!inBase64) {
            inBase64 = true;
            result.append('&');
        }
        for (const int byte : {unit.unicode() >> 8, unit.unicode() & 0xFF}) {
            bitBuffer = ((bitBuffer << 8) | static_cast<unsigned>(byte)) & 0xFFFFu;
            filled += 8;
            while (filled >= 6) {
                filled -= 6;
                result.append(static_cast<char>(kMutf7Alphabet[(bitBuffer >> filled) & 0x3F]));
            }
        }
    }
    closeRun();
    return result;
}

FolderRole folderRoleFromName(const QString &fullName, const QList<QString> &flags) {
    // 1) Mailbox flags win over any naming convention (R88 ladder).
    for (const QString &flag : flags) {
        const QString cleaned = flag.startsWith(u'\\') ? flag.mid(1) : flag;
        const QString lower = cleaned.toLower();
        if (lower == QLatin1String("inbox")) {
            return FolderRole::Inbox;
        }
        if (lower == QLatin1String("sent")) {
            return FolderRole::Sent;
        }
        if (lower == QLatin1String("drafts") || lower == QLatin1String("draft")) {
            return FolderRole::Drafts;
        }
        if (lower == QLatin1String("trash")) {
            return FolderRole::Trash;
        }
        if (lower == QLatin1String("junk") || lower == QLatin1String("spam")) {
            return FolderRole::Junk;
        }
        if (lower == QLatin1String("archive")) {
            return FolderRole::Archive;
        }
    }

    // 2)+3) English and Chinese aliases: whole name first, then the last
    // path segment ("Gmail/已发送", "INBOX.Drafts").
    const QString lowerWhole = fullName.toLower();
    QStringList segments = folderSegments(lowerWhole);
    const QString lowerLast = segments.isEmpty() ? QString() : segments.takeLast();
    if (const std::optional<FolderRole> role = aliasRole(lowerWhole)) {
        return *role;
    }
    if (const std::optional<FolderRole> role = aliasRole(lowerLast)) {
        return *role;
    }
    return FolderRole::Custom;
}

MailFlags mailFlagsFromLibetpan(uint32_t flags) {
    // Round 1 item 3: the input already carries MailFlags bits, so this only
    // prunes anything that is not one of the five known system flags.
    static constexpr uint32_t kKnownBits =
        static_cast<uint32_t>(MailFlag::Seen) | static_cast<uint32_t>(MailFlag::Flagged) |
        static_cast<uint32_t>(MailFlag::Answered) | static_cast<uint32_t>(MailFlag::Deleted) |
        static_cast<uint32_t>(MailFlag::Draft);
    return MailFlags(static_cast<MailFlag>(flags & kKnownBits));
}

MailFlags msgAttFlagsToMask(const struct mailimap_msg_att_dynamic *att) {
    MailFlags mapped;
    if (att == nullptr || att->att_list == nullptr) {
        return mapped;
    }
    for (clistiter *it = clist_begin(att->att_list); it != nullptr; it = clist_next(it)) {
        const auto *entry = static_cast<const struct mailimap_flag_fetch *>(clist_content(it));
        if (entry == nullptr || entry->fl_type != MAILIMAP_FLAG_FETCH_OTHER ||
            entry->fl_flag == nullptr) {
            continue; // \Recent carries no flag payload
        }
        // Round 1 item 3: explicit switch straight onto MailFlags — the only
        // place that knows both encodings (the libetpan enum ordering
        // ANSWERED=0..DRAFT=4 does not match the MailFlags bit layout).
        switch (entry->fl_flag->fl_type) {
        case MAILIMAP_FLAG_SEEN:
            mapped |= MailFlag::Seen;
            break;
        case MAILIMAP_FLAG_FLAGGED:
            mapped |= MailFlag::Flagged;
            break;
        case MAILIMAP_FLAG_ANSWERED:
            mapped |= MailFlag::Answered;
            break;
        case MAILIMAP_FLAG_DELETED:
            mapped |= MailFlag::Deleted;
            break;
        case MAILIMAP_FLAG_DRAFT:
            mapped |= MailFlag::Draft;
            break;
        default:
            break; // keywords/extensions never enter the core flag set
        }
    }
    return mapped;
}

QString formatAddress(const MailAddress &address) {
    if (address.name.isEmpty()) {
        return address.address;
    }
    if (address.address.isEmpty()) {
        return address.name;
    }
    return address.name + QStringLiteral(" <") + address.address + u'>';
}

QDateTime dateFromInternaldate(const struct mailimap_date_time *date) {
    if (date == nullptr) {
        return {};
    }
    const QDate day(date->dt_year, date->dt_month, date->dt_day);
    const QTime time(date->dt_hour, date->dt_min, date->dt_sec);
    if (!day.isValid() || !time.isValid()) {
        return {};
    }
    // Zone is +/-HHMM ("+0800" ⇒ +8*3600 + 0; "-0700" ⇒ -7*3600).
    const int zone = date->dt_zone;
    const int offsetSeconds = (zone / 100) * 3600 + (zone < 0 ? -1 : 1) * (qAbs(zone) % 100) * 60;
    return QDateTime(day, time, QTimeZone::UTC).addSecs(-offsetSeconds);
}

MessageHeader headerFromEnvelope(const struct mailimap_envelope *envelope, qint64 accountId,
                                 qint64 folderId) {
    MessageHeader header;
    header.accountId = accountId;
    header.folderId = folderId;
    if (envelope == nullptr) {
        return header;
    }
    header.subject = envelopeText(envelope->env_subject);
    header.rfcMessageId = envelopeText(envelope->env_message_id);
    // No References threading in M1: the message id is the thread anchor.
    header.threadId = header.rfcMessageId;
    if (envelope->env_from != nullptr) {
        const QList<MailAddress> from = addressListFromImap(envelope->env_from->frm_list);
        if (!from.isEmpty()) {
            header.from = from.first();
        }
    }
    if (envelope->env_to != nullptr) {
        header.to = addressListFromImap(envelope->env_to->to_list);
    }
    if (envelope->env_cc != nullptr) {
        header.cc = addressListFromImap(envelope->env_cc->cc_list);
    }
    if (envelope->env_bcc != nullptr) {
        header.bcc = addressListFromImap(envelope->env_bcc->bcc_list);
    }
    return header;
}

QList<MessageHeader> headersFromFetchList(const clist *attrs, qint64 accountId, qint64 folderId,
                                          quint32 fromUid) {
    QList<MessageHeader> headers;
    if (attrs == nullptr) {
        return headers;
    }
    for (clistiter *it = clist_begin(attrs); it != nullptr; it = clist_next(it)) {
        const auto *att = static_cast<const struct mailimap_msg_att *>(clist_content(it));
        if (att == nullptr) {
            continue;
        }
        const struct mailimap_envelope *envelope = nullptr;
        const struct mailimap_msg_att_dynamic *dynamic = nullptr;
        QDateTime receivedAt;
        qint64 size = -1;
        quint32 uid = 0;
        for (clistiter *itemIt = att->att_list == nullptr ? nullptr : clist_begin(att->att_list);
             itemIt != nullptr; itemIt = clist_next(itemIt)) {
            const auto *item =
                static_cast<const struct mailimap_msg_att_item *>(clist_content(itemIt));
            if (item == nullptr) {
                continue;
            }
            if (item->att_type == MAILIMAP_MSG_ATT_ITEM_DYNAMIC) {
                dynamic = item->att_data.att_dyn;
                continue;
            }
            if (item->att_type != MAILIMAP_MSG_ATT_ITEM_STATIC ||
                item->att_data.att_static == nullptr) {
                continue;
            }
            switch (item->att_data.att_static->att_type) {
            case MAILIMAP_MSG_ATT_ENVELOPE:
                envelope = item->att_data.att_static->att_data.att_env;
                break;
            case MAILIMAP_MSG_ATT_INTERNALDATE:
                receivedAt =
                    dateFromInternaldate(item->att_data.att_static->att_data.att_internal_date);
                break;
            case MAILIMAP_MSG_ATT_RFC822_SIZE:
                size = item->att_data.att_static->att_data.att_rfc822_size;
                break;
            case MAILIMAP_MSG_ATT_UID:
                uid = item->att_data.att_static->att_data.att_uid;
                break;
            default:
                break;
            }
        }
        if (uid == 0 || uid < fromUid) {
            continue;
        }
        MessageHeader header = headerFromEnvelope(envelope, accountId, folderId);
        if (dynamic != nullptr) {
            // Round 1 item 3: msgAttFlagsToMask already speaks MailFlags.
            header.flags = msgAttFlagsToMask(dynamic);
        }
        if (receivedAt.isValid()) {
            header.receivedAt = receivedAt;
        }
        if (size >= 0) {
            header.size = size;
        }
        header.uid = uid;
        // id ≡ uid keeps the store aligned with FakeMailBackend semantics.
        header.id = static_cast<qint64>(uid);
        headers.append(header);
    }
    std::sort(
        headers.begin(), headers.end(),
        [](const MessageHeader &left, const MessageHeader &right) { return left.uid < right.uid; });
    return headers;
}

QList<Folder> foldersFromList(const clist *list, qint64 accountId) {
    QList<Folder> folders;
    if (list == nullptr) {
        return folders;
    }
    for (clistiter *it = clist_begin(list); it != nullptr; it = clist_next(it)) {
        const auto *mb = static_cast<const struct mailimap_mailbox_list *>(clist_content(it));
        if (mb == nullptr || mb->mb_name == nullptr) {
            continue;
        }
        Folder folder;
        folder.accountId = accountId;
        folder.fullName = decodeMailbox(QByteArray(mb->mb_name));
        folder.delimiter = mb->mb_delimiter;
        const qsizetype separator = folder.fullName.lastIndexOf(folder.delimiter);
        folder.displayName = separator >= 0 ? folder.fullName.mid(separator + 1) : folder.fullName;
        QList<QString> flags;
        if (mb->mb_flag != nullptr) {
            if (mb->mb_flag->mbf_type == MAILIMAP_MBX_LIST_FLAGS_SFLAG) {
                switch (mb->mb_flag->mbf_sflag) {
                case MAILIMAP_MBX_LIST_SFLAG_MARKED:
                    flags.append(QStringLiteral("\\Marked"));
                    break;
                case MAILIMAP_MBX_LIST_SFLAG_NOSELECT:
                    flags.append(QStringLiteral("\\NoSelect"));
                    break;
                case MAILIMAP_MBX_LIST_SFLAG_UNMARKED:
                    flags.append(QStringLiteral("\\UnMarked"));
                    break;
                default:
                    break;
                }
            } else if (mb->mb_flag->mbf_oflags != nullptr) {
                for (clistiter *flagIt = clist_begin(mb->mb_flag->mbf_oflags); flagIt != nullptr;
                     flagIt = clist_next(flagIt)) {
                    const auto *oflag =
                        static_cast<const struct mailimap_mbx_list_oflag *>(clist_content(flagIt));
                    if (oflag != nullptr && oflag->of_type == MAILIMAP_MBX_LIST_OFLAG_FLAG_EXT &&
                        oflag->of_flag_ext != nullptr) {
                        flags.append(QString::fromUtf8(oflag->of_flag_ext));
                    }
                }
            }
        }
        folder.role = folderRoleFromName(folder.fullName, flags);
        folders.append(folder);
    }
    return folders;
}

QByteArray bodyFromMsgAtt(const struct mailimap_msg_att *att) {
    if (att == nullptr || att->att_list == nullptr) {
        return {};
    }
    for (clistiter *it = clist_begin(att->att_list); it != nullptr; it = clist_next(it)) {
        const auto *item = static_cast<const struct mailimap_msg_att_item *>(clist_content(it));
        if (item == nullptr || item->att_type != MAILIMAP_MSG_ATT_ITEM_STATIC ||
            item->att_data.att_static == nullptr ||
            item->att_data.att_static->att_type != MAILIMAP_MSG_ATT_BODY_SECTION) {
            continue;
        }
        const auto *section = item->att_data.att_static->att_data.att_body_section;
        if (section != nullptr && section->sec_body_part != nullptr && section->sec_length > 0) {
            return QByteArray(section->sec_body_part, static_cast<qsizetype>(section->sec_length));
        }
    }
    return {};
}

MailError errorFromMailimap(int code) {
    MailError error;
    switch (code) {
    case MAILIMAP_NO_ERROR:
    case MAILIMAP_NO_ERROR_AUTHENTICATED:
    case MAILIMAP_NO_ERROR_NON_AUTHENTICATED:
        return MailError{}; // success: empty error
    case MAILIMAP_ERROR_LOGIN:
    case MAILIMAP_ERROR_SASL:
        error.code = ErrorCode::Auth;
        break;
    case MAILIMAP_ERROR_SSL:
    case MAILIMAP_ERROR_STARTTLS:
        error.code = ErrorCode::Tls;
        break;
    case MAILIMAP_ERROR_STREAM:
    case MAILIMAP_ERROR_CONNECTION_REFUSED:
    case MAILIMAP_ERROR_DONT_ACCEPT_CONNECTION:
    case MAILIMAP_ERROR_NEEDS_MORE_DATA:
        error.code = ErrorCode::Network;
        break;
    case MAILIMAP_ERROR_MEMORY:
    case MAILIMAP_ERROR_FATAL:
        error.code = ErrorCode::Unknown;
        break;
    default:
        // Parse / protocol / command-specific failures.
        error.code = ErrorCode::Protocol;
        break;
    }
    return error;
}

MailError errorFromMailsmtp(int code) {
    MailError error;
    switch (code) {
    case MAILSMTP_NO_ERROR:
        return MailError{}; // success: empty error
    case MAILSMTP_ERROR_AUTH_NOT_SUPPORTED:
    case MAILSMTP_ERROR_AUTH_LOGIN:
    case MAILSMTP_ERROR_AUTH_REQUIRED:
    case MAILSMTP_ERROR_AUTH_TOO_WEAK:
    case MAILSMTP_ERROR_AUTH_TRANSITION_NEEDED:
    case MAILSMTP_ERROR_AUTH_TEMPORARY_FAILTURE:
    case MAILSMTP_ERROR_AUTH_ENCRYPTION_REQUIRED:
    case MAILSMTP_ERROR_AUTH_AUTHENTICATION_FAILED:
        error.code = ErrorCode::Auth;
        break;
    case MAILSMTP_ERROR_SSL:
    case MAILSMTP_ERROR_STARTTLS_NOT_SUPPORTED:
    case MAILSMTP_ERROR_STARTTLS_TEMPORARY_FAILURE:
        error.code = ErrorCode::Tls;
        break;
    case MAILSMTP_ERROR_STREAM:
    case MAILSMTP_ERROR_CONNECTION_REFUSED:
    case MAILSMTP_ERROR_HOSTNAME:
        error.code = ErrorCode::Network;
        break;
    case MAILSMTP_ERROR_MEMORY:
        error.code = ErrorCode::Unknown;
        break;
    default:
        // Unexpected replies, bad sequences, transaction failures.
        error.code = ErrorCode::Protocol;
        break;
    }
    return error;
}

ImapSession::ImapSession() : m_session(mailimap_new(0, nullptr)) {
}

ImapSession::~ImapSession() {
    close();
}

struct mailimap *ImapSession::get() const {
    return m_session;
}

void ImapSession::release() {
    if (m_session != nullptr) {
        // Connect failed: the stream is unusable, only free the descriptor.
        mailimap_free(m_session);
        m_session = nullptr;
    }
}

void ImapSession::close() {
    if (m_session != nullptr) {
        // Logout failures (already gone server) must not leak the session.
        mailimap_logout(m_session);
        mailimap_free(m_session);
        m_session = nullptr;
    }
}

SmtpSession::SmtpSession() : m_session(mailsmtp_new(0, nullptr)) {
}

SmtpSession::~SmtpSession() {
    if (m_session != nullptr) {
        // mailsmtp_free emits the trailing QUIT when the socket is open.
        mailsmtp_free(m_session);
        m_session = nullptr;
    }
}

struct mailsmtp *SmtpSession::get() const {
    return m_session;
}

void SmtpSession::release() {
    if (m_session != nullptr) {
        mailsmtp_free(m_session);
        m_session = nullptr;
    }
}

ImapFetchList::ImapFetchList(clist *list) : m_list(list) {
}

ImapFetchList::~ImapFetchList() {
    if (m_list != nullptr) {
        mailimap_fetch_list_free(m_list);
        m_list = nullptr;
    }
}

const clist *ImapFetchList::get() const {
    return m_list;
}

ImapMailboxList::ImapMailboxList(clist *list) : m_list(list) {
}

ImapMailboxList::~ImapMailboxList() {
    if (m_list != nullptr) {
        mailimap_list_result_free(m_list);
        m_list = nullptr;
    }
}

const clist *ImapMailboxList::get() const {
    return m_list;
}

} // namespace ningmail

#endif // NINGMAIL_WITH_LIBETPAN
