// Unit tests for the libetpan pure helpers (task 13, R86).
//
// Everything testable without a live IMAP server lives in LibetpanHelpers
// and is exercised here through hand-built libetpan C structures. The
// protocol-speaking Backend is covered by tests/integration ([integration]
// tag, skipped without a real account on this host).

#include "LibetpanHelpers.h"

#include "MimeParser.h"
#include "Types.h"

#include <catch2/catch_test_macros.hpp>

#include <QByteArray>
#include <QDate>
#include <QDateTime>
#include <QList>
#include <QString>
#include <QTimeZone>

#ifdef NINGMAIL_WITH_LIBETPAN

namespace {

using ningmail::MailAddress;

char *cstr(const char *value) {
    return value == nullptr ? nullptr : strdup(value);
}

MailAddress address(const QString &name, const QString &mail) {
    MailAddress result;
    result.name = name;
    result.address = mail;
    return result;
}

struct mailimap_address *imapAddress(const char *name, const char *mailbox, const char *host) {
    return mailimap_address_new(cstr(name), nullptr, cstr(mailbox), cstr(host));
}

clist *addressList(std::initializer_list<struct mailimap_address *> addr) {
    clist *list = clist_new();
    for (struct mailimap_address *item : addr) {
        clist_append(list, item);
    }
    return list;
}

struct mailimap_envelope *makeEnvelope(const char *subject, clist *from, clist *to, clist *cc,
                                       clist *bcc, const char *messageId) {
    return mailimap_envelope_new(nullptr, cstr(subject), mailimap_env_from_new(from), nullptr,
                                 nullptr, to == nullptr ? nullptr : mailimap_env_to_new(to),
                                 cc == nullptr ? nullptr : mailimap_env_cc_new(cc),
                                 bcc == nullptr ? nullptr : mailimap_env_bcc_new(bcc), nullptr,
                                 cstr(messageId));
}

struct mailimap_msg_att_item *envelopeItem(struct mailimap_envelope *envelope) {
    return mailimap_msg_att_item_new(
        MAILIMAP_MSG_ATT_ITEM_STATIC, nullptr,
        mailimap_msg_att_static_new(MAILIMAP_MSG_ATT_ENVELOPE, envelope, nullptr, nullptr, nullptr,
                                    nullptr, 0, 0, nullptr, nullptr, nullptr, 0),
        nullptr);
}

struct mailimap_msg_att_item *internalDateItem(int day, int month, int year, int hour, int minute,
                                               int second, int zone) {
    return mailimap_msg_att_item_new(
        MAILIMAP_MSG_ATT_ITEM_STATIC, nullptr,
        mailimap_msg_att_static_new(
            MAILIMAP_MSG_ATT_INTERNALDATE, nullptr,
            mailimap_date_time_new(day, month, year, hour, minute, second, zone), nullptr, nullptr,
            nullptr, 0, 0, nullptr, nullptr, nullptr, 0),
        nullptr);
}

struct mailimap_msg_att_item *uidItem(uint32_t uid) {
    return mailimap_msg_att_item_new(
        MAILIMAP_MSG_ATT_ITEM_STATIC, nullptr,
        mailimap_msg_att_static_new(MAILIMAP_MSG_ATT_UID, nullptr, nullptr, nullptr, nullptr,
                                    nullptr, 0, 0, nullptr, nullptr, nullptr, uid),
        nullptr);
}

struct mailimap_msg_att_item *sizeItem(uint32_t size) {
    return mailimap_msg_att_item_new(
        MAILIMAP_MSG_ATT_ITEM_STATIC, nullptr,
        mailimap_msg_att_static_new(MAILIMAP_MSG_ATT_RFC822_SIZE, nullptr, nullptr, nullptr,
                                    nullptr, nullptr, 0, size, nullptr, nullptr, nullptr, 0),
        nullptr);
}

struct mailimap_msg_att_item *flagsItem(std::initializer_list<uint32_t> flagTypes) {
    clist *list = clist_new();
    for (uint32_t type : flagTypes) {
        clist_append(list, mailimap_flag_fetch_new(
                               MAILIMAP_FLAG_FETCH_OTHER,
                               mailimap_flag_new(static_cast<int>(type), nullptr, nullptr)));
    }
    return mailimap_msg_att_item_new(MAILIMAP_MSG_ATT_ITEM_DYNAMIC,
                                     mailimap_msg_att_dynamic_new(list), nullptr, nullptr);
}

struct mailimap_msg_att_item *bodySectionItem(const char *content) {
    auto *section = mailimap_msg_att_body_section_new(mailimap_section_new(nullptr), 0,
                                                      cstr(content), strlen(content));
    return mailimap_msg_att_item_new(
        MAILIMAP_MSG_ATT_ITEM_STATIC, nullptr,
        mailimap_msg_att_static_new(MAILIMAP_MSG_ATT_BODY_SECTION, nullptr, nullptr, nullptr,
                                    nullptr, nullptr, 0, 0, nullptr, nullptr, section, 0),
        nullptr);
}

struct mailimap_msg_att *messageAtt(std::initializer_list<struct mailimap_msg_att_item *> items,
                                    uint32_t attNumber = 0) {
    clist *list = clist_new();
    for (struct mailimap_msg_att_item *item : items) {
        clist_append(list, item);
    }
    auto *att = mailimap_msg_att_new(list);
    att->att_number = attNumber;
    return att;
}

clist *fetchList(std::initializer_list<struct mailimap_msg_att *> atts) {
    clist *list = clist_new();
    for (struct mailimap_msg_att *att : atts) {
        clist_append(list, att);
    }
    return list;
}

clist *mailboxList(std::initializer_list<struct mailimap_mailbox_list *> entries) {
    clist *list = clist_new();
    for (struct mailimap_mailbox_list *entry : entries) {
        clist_append(list, entry);
    }
    return list;
}

struct mailimap_mailbox_list *mailboxEntry(const char *name, char delimiter, int sflag,
                                           std::initializer_list<const char *> flagExtensions) {
    struct mailimap_mbx_list_flags *flags = nullptr;
    if (sflag != MAILIMAP_MBX_LIST_SFLAG_ERROR || flagExtensions.size() > 0) {
        clist *oflags = nullptr;
        if (flagExtensions.size() > 0) {
            oflags = clist_new();
            for (const char *ext : flagExtensions) {
                clist_append(oflags, mailimap_mbx_list_oflag_new(MAILIMAP_MBX_LIST_OFLAG_FLAG_EXT,
                                                                 cstr(ext)));
            }
        }
        flags =
            mailimap_mbx_list_flags_new(flagExtensions.size() > 0 ? MAILIMAP_MBX_LIST_FLAGS_NO_SFLAG
                                                                  : MAILIMAP_MBX_LIST_FLAGS_SFLAG,
                                        oflags, sflag);
    }
    return mailimap_mailbox_list_new(flags, delimiter, cstr(name));
}

void freeFetchList(clist *list) {
    if (list == nullptr)
        return;
    for (clistiter *it = clist_begin(list); it != nullptr; it = clist_next(it)) {
        mailimap_msg_att_free(static_cast<struct mailimap_msg_att *>(clist_content(it)));
    }
    clist_free(list);
}

void freeMailboxList(clist *list) {
    if (list == nullptr)
        return;
    for (clistiter *it = clist_begin(list); it != nullptr; it = clist_next(it)) {
        mailimap_mailbox_list_free(static_cast<struct mailimap_mailbox_list *>(clist_content(it)));
    }
    clist_free(list);
}

} // namespace

TEST_CASE("mUTF-7 decoder follows RFC 3501 5.1.3", "[libetpan]") {
    // RFC 3501 5.1.3 anchor example (controller context quoted the lowercase
    // variant "&U,bTFw-"; the RFC original uses uppercase "&U,BTFw-").
    REQUIRE(ningmail::decodeMailbox(QByteArrayLiteral("~peter/mail/&U,BTFw-/&ZeVnLIqe-")) ==
            QString::fromUtf8("~peter/mail/台北/日本語"));
    REQUIRE(ningmail::decodeMailbox(QByteArrayLiteral("&U,BTFw-")) == QString::fromUtf8("台北"));
    REQUIRE(ningmail::decodeMailbox(QByteArrayLiteral("&ZeVnLIqe-")) ==
            QString::fromUtf8("日本語"));
    // "&-" is the escape for a literal "&".
    REQUIRE(ningmail::decodeMailbox(QByteArrayLiteral("&-")) == QString::fromUtf8("&"));
    // Pure US-ASCII passes through untouched.
    REQUIRE(ningmail::decodeMailbox(QByteArrayLiteral("INBOX")) == QString::fromUtf8("INBOX"));
    REQUIRE(ningmail::decodeMailbox(QByteArrayLiteral("Sent Items")) ==
            QString::fromUtf8("Sent Items"));
    // Malformed modified UTF-7 ("&-less" tail, RFC: "&Jjo!" is invalid) falls
    // back to the raw bytes instead of inventing a wrong mailbox name.
    REQUIRE(ningmail::decodeMailbox(QByteArrayLiteral("&Jjo!")) == QString::fromUtf8("&Jjo!"));
}

TEST_CASE("mUTF-7 malformed sequences fall back to raw bytes", "[libetpan]") {
    // Unterminated BASE64 run (no closing '-').
    REQUIRE(ningmail::decodeMailbox(QByteArrayLiteral("&Abc")) == QString::fromUtf8("&Abc"));
    // Two sextets build 12 bits: a single odd byte is not a 16-bit unit.
    REQUIRE(ningmail::decodeMailbox(QByteArrayLiteral("&AB-")) == QString::fromUtf8("&AB-"));
    // A one-character run can never build a 16-bit unit.
    REQUIRE(ningmail::decodeMailbox(QByteArrayLiteral("&A-")) == QString::fromUtf8("&A-"));
}

TEST_CASE("mUTF-7 encoder mirrors RFC 3501 5.1.3 and roundtrips", "[libetpan]") {
    REQUIRE(ningmail::encodeMailbox(QString::fromUtf8("台北")) == QByteArrayLiteral("&U,BTFw-"));
    REQUIRE(ningmail::encodeMailbox(QString::fromUtf8("日本語")) ==
            QByteArrayLiteral("&ZeVnLIqe-"));
    REQUIRE(ningmail::encodeMailbox(QString::fromUtf8("~peter/mail/台北/日本語")) ==
            QByteArrayLiteral("~peter/mail/&U,BTFw-/&ZeVnLIqe-"));
    // "&" always becomes "&-" on the wire.
    REQUIRE(ningmail::encodeMailbox(QString::fromUtf8("Rock & Roll")) ==
            QByteArrayLiteral("Rock &- Roll"));
    // Printable US-ASCII (except "&") must never be BASE64-encoded.
    REQUIRE(ningmail::encodeMailbox(QString::fromUtf8("INBOX")) == QByteArrayLiteral("INBOX"));
    // Roundtrips both ways for the RFC vector and a non-BMP code point.
    const QString mailbox = QString::fromUtf8("~peter/mail/台北/日本語/🦌");
    REQUIRE(ningmail::decodeMailbox(ningmail::encodeMailbox(mailbox)) == mailbox);
    const QByteArray encoded =
        ningmail::encodeMailbox(QString::fromUtf8("~peter/mail/台北/日本語"));
    REQUIRE(ningmail::decodeMailbox(encoded) == QString::fromUtf8("~peter/mail/台北/日本語"));
}

TEST_CASE("folderRoleFromName maps flags, English and Coremail aliases", "[libetpan]") {
    using ningmail::FolderRole;

    // Mailbox (LIST) flags win first.
    REQUIRE(ningmail::folderRoleFromName(QString::fromUtf8("Other"),
                                         {QString::fromUtf8("\\Sent")}) == FolderRole::Sent);
    REQUIRE(ningmail::folderRoleFromName(QString::fromUtf8("Other"),
                                         {QString::fromUtf8("\\Trash")}) == FolderRole::Trash);
    // English names (any case), matched on the last hierarchy segment.
    REQUIRE(ningmail::folderRoleFromName(QString::fromUtf8("INBOX")) == FolderRole::Inbox);
    REQUIRE(ningmail::folderRoleFromName(QString::fromUtf8("Sent")) == FolderRole::Sent);
    REQUIRE(ningmail::folderRoleFromName(QString::fromUtf8("sent items")) == FolderRole::Sent);
    REQUIRE(ningmail::folderRoleFromName(QString::fromUtf8("INBOX.Drafts")) == FolderRole::Drafts);
    REQUIRE(ningmail::folderRoleFromName(QString::fromUtf8("Trash")) == FolderRole::Trash);
    REQUIRE(ningmail::folderRoleFromName(QString::fromUtf8("Junk")) == FolderRole::Junk);
    REQUIRE(ningmail::folderRoleFromName(QString::fromUtf8("Archive")) == FolderRole::Archive);
    // Coremail/QQ/163 Chinese aliases.
    REQUIRE(ningmail::folderRoleFromName(QString::fromUtf8("收件箱")) == FolderRole::Inbox);
    REQUIRE(ningmail::folderRoleFromName(QString::fromUtf8("已发送")) == FolderRole::Sent);
    REQUIRE(ningmail::folderRoleFromName(QString::fromUtf8("草稿箱")) == FolderRole::Drafts);
    REQUIRE(ningmail::folderRoleFromName(QString::fromUtf8("已删除")) == FolderRole::Trash);
    REQUIRE(ningmail::folderRoleFromName(QString::fromUtf8("垃圾邮件")) == FolderRole::Junk);
    REQUIRE(ningmail::folderRoleFromName(QString::fromUtf8("存档")) == FolderRole::Archive);
    // Unknown folders stay Custom.
    REQUIRE(ningmail::folderRoleFromName(QString::fromUtf8("其他文件夹")) == FolderRole::Custom);
    // fullName keeps its original form in the caller; helper only decides role.
    REQUIRE(ningmail::folderRoleFromName(QString::fromUtf8("Gmail/已发送")) == FolderRole::Sent);
}

TEST_CASE("mailFlagsFromLibetpan forwards the MailFlags bit pattern", "[libetpan]") {
    using ningmail::MailFlag;

    // Round 1 item 3: the input is already a MailFlags bit pattern
    // (msgAttFlagsToMask maps libetpan flags onto MailFlags itself), so
    // known bits pass through and unknown bits are pruned.
    const auto combined = ningmail::MailFlags(MailFlag::Flagged) | MailFlag::Answered |
                          MailFlag::Deleted | MailFlag::Draft;
    REQUIRE(ningmail::mailFlagsFromLibetpan(uint32_t(MailFlag::Seen)) ==
            ningmail::MailFlags(MailFlag::Seen));
    REQUIRE(ningmail::mailFlagsFromLibetpan(
                uint32_t(MailFlag::Flagged) | uint32_t(MailFlag::Answered) |
                uint32_t(MailFlag::Deleted) | uint32_t(MailFlag::Draft)) == combined);
    // Keywords / extensions / foreign bits never leak into the core set.
    REQUIRE(ningmail::mailFlagsFromLibetpan(1u << 8) == ningmail::MailFlags());
    REQUIRE(ningmail::mailFlagsFromLibetpan(uint32_t(MailFlag::Seen) | (1u << 8)) ==
            ningmail::MailFlags(MailFlag::Seen));
}

TEST_CASE("msgAttFlagsToMask maps flag items onto MailFlags directly", "[libetpan]") {
    using ningmail::MailFlag;

    auto *flags = mailimap_msg_att_dynamic_new(nullptr);
    clist *list = clist_new();
    clist_append(list,
                 mailimap_flag_fetch_new(MAILIMAP_FLAG_FETCH_OTHER,
                                         mailimap_flag_new(MAILIMAP_FLAG_SEEN, nullptr, nullptr)));
    clist_append(
        list, mailimap_flag_fetch_new(MAILIMAP_FLAG_FETCH_OTHER,
                                      mailimap_flag_new(MAILIMAP_FLAG_FLAGGED, nullptr, nullptr)));
    // Keywords have no MailFlag counterpart.
    clist_append(list, mailimap_flag_fetch_new(
                           MAILIMAP_FLAG_FETCH_OTHER,
                           mailimap_flag_new(MAILIMAP_FLAG_KEYWORD, cstr("custom"), nullptr)));
    flags->att_list = list;

    const auto mapped = ningmail::msgAttFlagsToMask(flags);
    REQUIRE(mapped == (ningmail::MailFlags(MailFlag::Seen) | MailFlag::Flagged));
    REQUIRE_FALSE(mapped.testFlag(MailFlag::Draft));

    mailimap_msg_att_dynamic_free(flags);

    // Missing / flagless responses stay empty.
    REQUIRE(ningmail::msgAttFlagsToMask(nullptr) == ningmail::MailFlags());
}

TEST_CASE("extractRfcMessageId reads the header Message-ID", "[libetpan]") {
    using ningmail::extractRfcMessageId;

    // Plain header with the id in angle brackets.
    REQUIRE(extractRfcMessageId(QByteArrayLiteral("From: a@b\r\n"
                                                  "Message-ID: <id@host>\r\n"
                                                  "\r\n"
                                                  "body")) == QString::fromUtf8("<id@host>"));
    // Field-name case variants and whitespace around the value.
    REQUIRE(extractRfcMessageId(QByteArrayLiteral("from: a@b\r\n"
                                                  "message-id:\t <x@y> \r\n"
                                                  "\r\n")) == QString::fromUtf8("<x@y>"));
    REQUIRE(extractRfcMessageId(QByteArrayLiteral("MESSAGE-ID: <up@low>\r\n\r\n")) ==
            QString::fromUtf8("<up@low>"));
    // Folded header: the value starts on a continuation line.
    REQUIRE(extractRfcMessageId(QByteArrayLiteral("Message-Id:\r\n"
                                                  " <folded@host>\r\n"
                                                  "\r\n")) == QString::fromUtf8("<folded@host>"));

    // Round 2 item 1: legal CFWS comments around the id must not leak into
    // the extracted value — the first <...> token wins.
    REQUIRE(extractRfcMessageId(QByteArrayLiteral("Message-ID: <id@x> (comment)\r\n\r\n")) ==
            QString::fromUtf8("<id@x>"));
    REQUIRE(extractRfcMessageId(QByteArrayLiteral("Message-Id: (note) <id@x>\r\n\r\n")) ==
            QString::fromUtf8("<id@x>"));
    // An empty angle pair is not an id token: fall back to the trimmed
    // (whole) line value so both sides still normalize onto it (round 2).
    REQUIRE(extractRfcMessageId(QByteArrayLiteral("Message-Id: (note) <>\r\n\r\n")) ==
            QString::fromUtf8("(note) <>"));

    // No Message-ID / empty input yield an empty string.
    REQUIRE(extractRfcMessageId(QByteArrayLiteral("From: a\r\n\r\nbody")).isEmpty());
    REQUIRE(extractRfcMessageId(QByteArrayLiteral("")).isEmpty());
    // Only the header region is searched: a body line must not match.
    REQUIRE(extractRfcMessageId(QByteArrayLiteral("From: a\r\n\r\nmessage-id: <later@host>"))
                .isEmpty());
}

TEST_CASE("messageIdMismatchError reports both ids", "[libetpan]") {
    const ningmail::MailError error = ningmail::messageIdMismatchError(
        QString::fromUtf8("<msg-1@example.test>"), QString::fromUtf8("<other@host>"));
    REQUIRE(error.code == ningmail::ErrorCode::Protocol);
    REQUIRE(error.message.contains(QString::fromUtf8("<msg-1@example.test>")));
    REQUIRE(error.message.contains(QString::fromUtf8("<other@host>")));

    // An empty extracted id shows up as "(none)" instead of vanishing.
    const ningmail::MailError none =
        ningmail::messageIdMismatchError(QString::fromUtf8("<a@x>"), QString());
    REQUIRE(none.message.contains(QString::fromUtf8("(none)")));
}

TEST_CASE("normalizedMessageId keeps only the comparison token", "[libetpan]") {
    using ningmail::normalizedMessageId;

    // Round 2 item 1: the envelope side may carry CFWS the body side lacks
    // (and vice versa) — the inner <...> token decides, raw values never do.
    REQUIRE(normalizedMessageId(QString::fromUtf8("<id@x> (comment)")) ==
            QString::fromUtf8("id@x"));
    REQUIRE(normalizedMessageId(QString::fromUtf8("(note) <>")) == QString());
    REQUIRE(normalizedMessageId(QString::fromUtf8("<id@x>")) == QString::fromUtf8("id@x"));
    // No angle pair at all: the trimmed value itself is comparable.
    REQUIRE(normalizedMessageId(QString::fromUtf8("  id@x  ")) == QString::fromUtf8("id@x"));
    REQUIRE(normalizedMessageId(QString()) == QString());
}

TEST_CASE("header and body message ids compare via normalization", "[libetpan]") {
    // This mirrors fetchBody's round 2 check: normalize both sides, and the
    // error builder still receives the raw values.
    const auto extractedOf = [](const QByteArray &raw) {
        return ningmail::extractRfcMessageId(raw);
    };

    // ① comment only in the body header → pass.
    REQUIRE(ningmail::normalizedMessageId(
                extractedOf(QByteArrayLiteral("Message-ID: <id@x> (comment)\r\n\r\n"))) ==
            ningmail::normalizedMessageId(QString::fromUtf8("<id@x>")));
    // ③ comment only on the envelope side → pass.
    REQUIRE(ningmail::normalizedMessageId(
                extractedOf(QByteArrayLiteral("Message-ID: <id@x>\r\n\r\n"))) ==
            ningmail::normalizedMessageId(QString::fromUtf8("<id@x> (comment)")));
    // ② empty angle pair both sides (weird server) → pass.
    REQUIRE(ningmail::normalizedMessageId(
                extractedOf(QByteArrayLiteral("Message-Id: (note) <>\r\n\r\n"))) ==
            ningmail::normalizedMessageId(QString::fromUtf8("<>")));
    // ④ a real mismatch still triggers.
    REQUIRE_FALSE(ningmail::normalizedMessageId(
                      extractedOf(QByteArrayLiteral("Message-ID: <a@x>\r\n\r\n"))) ==
                  ningmail::normalizedMessageId(QString::fromUtf8("<b@y>")));
    // The error builder keeps reporting the raw ids untouched.
    REQUIRE(ningmail::messageIdMismatchError(QString::fromUtf8("<b@y> (c)"),
                                             QString::fromUtf8("<a@x> (c)"))
                .message.contains(QString::fromUtf8("<b@y> (c)")));
}

TEST_CASE("formatAddress renders RFC 2822 style addresses", "[libetpan]") {
    REQUIRE(ningmail::formatAddress(address(QString::fromUtf8("Name"), QString::fromUtf8("a@b"))) ==
            QString::fromUtf8("Name <a@b>"));
    REQUIRE(ningmail::formatAddress(address(QString(), QString::fromUtf8("a@b"))) ==
            QString::fromUtf8("a@b"));
}

TEST_CASE("dateFromInternaldate converts to UTC", "[libetpan]") {
    // 17 July 1996, 02:44:25 -0700 → 09:44:25Z.
    auto *date = mailimap_date_time_new(17, 7, 1996, 2, 44, 25, -700);
    const QDateTime converted = ningmail::dateFromInternaldate(date);
    mailimap_date_time_free(date);
    const QDateTime expected(QDate(1996, 7, 17), QTime(9, 44, 25), QTimeZone::UTC);
    REQUIRE(converted == expected);

    REQUIRE(ningmail::dateFromInternaldate(nullptr).isNull());
}

TEST_CASE("headerFromEnvelope maps subject and multi-address fields", "[libetpan]") {
    // Hand-built ENVELOPE with an RFC 2047 encoded Chinese subject and a
    // two-recipient To list.
    clist *from = addressList({imapAddress("Zhang San", "zhangsan", "corp.example")});
    clist *to = addressList({imapAddress("Alice", "alice", "example.test"),
                             // Address supplied whole in the mailbox part
                             // (relay style): no personal name survives.
                             imapAddress(nullptr, "bob@example.test", "example.org")});
    clist *cc = addressList({imapAddress("Carol", "carol", "example.test")});
    struct mailimap_envelope *envelope =
        makeEnvelope("=?UTF-8?B?5rWL6K+V?=", from, to, cc, nullptr, "<msg-1@example.test>");

    const ningmail::MessageHeader header = ningmail::headerFromEnvelope(envelope, 7, 11);
    mailimap_envelope_free(envelope);

    REQUIRE(header.accountId == 7);
    REQUIRE(header.folderId == 11);
    REQUIRE(header.subject == QString::fromUtf8("测试"));
    REQUIRE(header.rfcMessageId == QString::fromUtf8("<msg-1@example.test>"));
    REQUIRE(header.threadId == QString::fromUtf8("<msg-1@example.test>"));
    REQUIRE(header.from ==
            address(QString::fromUtf8("Zhang San"), QString::fromUtf8("zhangsan@corp.example")));
    REQUIRE(header.to.size() == 2);
    REQUIRE(header.to.at(0) ==
            address(QString::fromUtf8("Alice"), QString::fromUtf8("alice@example.test")));
    REQUIRE(header.to.at(1) == address(QString(), QString::fromUtf8("bob@example.test")));
    REQUIRE(header.cc.size() == 1);
    REQUIRE(header.cc.at(0) ==
            address(QString::fromUtf8("Carol"), QString::fromUtf8("carol@example.test")));
    REQUIRE(header.bcc.isEmpty());
}

TEST_CASE("headersFromFetchList merges envelope, flags, date, size and uid", "[libetpan]") {
    // One FETCH response row assembled by hand:
    // FLAGS (Seen Flagged), INTERNALDATE, RFC822.SIZE, UID 42, ENVELOPE.
    clist *from = addressList({imapAddress("Zhang San", "zhangsan", "corp.example")});
    struct mailimap_envelope *envelope = makeEnvelope("=?UTF-8?B?5rWL6K+V?=", from, nullptr,
                                                      nullptr, nullptr, "<msg-42@example.test>");
    auto *att = messageAtt({flagsItem({MAILIMAP_FLAG_SEEN, MAILIMAP_FLAG_FLAGGED}),
                            internalDateItem(17, 7, 1996, 2, 44, 25, -700), sizeItem(1024),
                            uidItem(42), envelopeItem(envelope)});

    clist *list = fetchList({att});
    const QList<ningmail::MessageHeader> headers = ningmail::headersFromFetchList(list, 3, 4, 0);
    freeFetchList(list);

    REQUIRE(headers.size() == 1);
    const ningmail::MessageHeader &header = headers.at(0);
    // id ≡ uid aligns with the FakeMailBackend semantics.
    REQUIRE(header.uid == 42);
    REQUIRE(header.id == 42);
    REQUIRE(header.accountId == 3);
    REQUIRE(header.folderId == 4);
    REQUIRE(header.subject == QString::fromUtf8("测试"));
    REQUIRE(header.size == 1024);
    REQUIRE(header.flags.testFlag(ningmail::MailFlag::Seen));
    REQUIRE(header.flags.testFlag(ningmail::MailFlag::Flagged));
    REQUIRE(header.receivedAt == QDateTime(QDate(1996, 7, 17), QTime(9, 44, 25), QTimeZone::UTC));
    REQUIRE(header.from ==
            address(QString::fromUtf8("Zhang San"), QString::fromUtf8("zhangsan@corp.example")));
    REQUIRE(header.threadId == QString::fromUtf8("<msg-42@example.test>"));
}

TEST_CASE("headersFromFetchList sorts ascending and filters by fromUid", "[libetpan]") {
    auto *later = messageAtt({uidItem(7)});
    auto *earlier = messageAtt({uidItem(3)});
    // Deliberately out of order; helper must sort by uid.
    clist *list = fetchList({later, earlier});
    const QList<ningmail::MessageHeader> headers = ningmail::headersFromFetchList(list, 1, 1, 5);

    REQUIRE(headers.size() == 1);
    REQUIRE(headers.at(0).uid == 7);
    freeFetchList(list);
}

TEST_CASE("foldersFromList decodes mUTF-7 names and assigns roles", "[libetpan]") {
    const clist *list =
        mailboxList({mailboxEntry("INBOX", '/', MAILIMAP_MBX_LIST_SFLAG_ERROR, {}),
                     mailboxEntry("~peter/mail/&U,BTFw-", '/', MAILIMAP_MBX_LIST_SFLAG_ERROR, {}),
                     mailboxEntry("Sent", '/', MAILIMAP_MBX_LIST_SFLAG_ERROR, {}),
                     mailboxEntry("&U,BTFw-2Xzd", '.', MAILIMAP_MBX_LIST_SFLAG_ERROR, {"\\Sent"})});

    const QList<ningmail::Folder> folders = ningmail::foldersFromList(list, 9);
    freeMailboxList(const_cast<clist *>(list));

    REQUIRE(folders.size() == 4);
    REQUIRE(folders.at(0).accountId == 9);
    REQUIRE(folders.at(0).fullName == QString::fromUtf8("INBOX"));
    REQUIRE(folders.at(0).role == ningmail::FolderRole::Inbox);
    REQUIRE(folders.at(0).delimiter == u'/');
    REQUIRE(folders.at(1).fullName == QString::fromUtf8("~peter/mail/台北"));
    REQUIRE(folders.at(2).role == ningmail::FolderRole::Sent);
    REQUIRE(folders.at(3).delimiter == u'.');
    REQUIRE(folders.at(3).role == ningmail::FolderRole::Sent);
}

TEST_CASE("errorFromMailimap and errorFromMailsmtp map classes", "[libetpan]") {
    using ningmail::ErrorCode;

    REQUIRE(ningmail::errorFromMailimap(MAILIMAP_NO_ERROR).code == ErrorCode::None);
    REQUIRE(ningmail::errorFromMailimap(MAILIMAP_NO_ERROR_AUTHENTICATED).code == ErrorCode::None);
    REQUIRE(ningmail::errorFromMailimap(MAILIMAP_NO_ERROR_NON_AUTHENTICATED).code ==
            ErrorCode::None);
    REQUIRE(ningmail::errorFromMailimap(MAILIMAP_ERROR_LOGIN).code == ErrorCode::Auth);
    REQUIRE(ningmail::errorFromMailimap(MAILIMAP_ERROR_CONNECTION_REFUSED).code ==
            ErrorCode::Network);
    REQUIRE(ningmail::errorFromMailimap(MAILIMAP_ERROR_STREAM).code == ErrorCode::Network);
    REQUIRE(ningmail::errorFromMailimap(MAILIMAP_ERROR_DONT_ACCEPT_CONNECTION).code ==
            ErrorCode::Network);
    REQUIRE(ningmail::errorFromMailimap(MAILIMAP_ERROR_FETCH).code == ErrorCode::Protocol);
    REQUIRE(ningmail::errorFromMailimap(MAILIMAP_ERROR_APPEND).code == ErrorCode::Protocol);
    REQUIRE(ningmail::errorFromMailimap(MAILIMAP_ERROR_SELECT).code == ErrorCode::Protocol);

    REQUIRE(ningmail::errorFromMailsmtp(MAILSMTP_NO_ERROR).code == ErrorCode::None);
    REQUIRE(ningmail::errorFromMailsmtp(MAILSMTP_ERROR_AUTH_LOGIN).code == ErrorCode::Auth);
    REQUIRE(ningmail::errorFromMailsmtp(MAILSMTP_ERROR_AUTH_AUTHENTICATION_FAILED).code ==
            ErrorCode::Auth);
    REQUIRE(ningmail::errorFromMailsmtp(MAILSMTP_ERROR_CONNECTION_REFUSED).code ==
            ErrorCode::Network);
    REQUIRE(ningmail::errorFromMailsmtp(MAILSMTP_ERROR_STREAM).code == ErrorCode::Network);
    REQUIRE(ningmail::errorFromMailsmtp(MAILSMTP_ERROR_TRANSACTION_FAILED).code ==
            ErrorCode::Protocol);
    REQUIRE(ningmail::errorFromMailsmtp(MAILSMTP_ERROR_STARTTLS_NOT_SUPPORTED).code ==
            ErrorCode::Tls);
}

TEST_CASE("bodyFromMsgAtt extracts the BODY[] payload", "[libetpan]") {
    auto *att = messageAtt({bodySectionItem("hello"), uidItem(5)});
    const QByteArray body = ningmail::bodyFromMsgAtt(att);
    REQUIRE(body == QByteArrayLiteral("hello"));
}

TEST_CASE("fetchBodyTransport reuses the fetched body through MimeParser (R70 note)",
          "[libetpan]") {
    // A raw MIME message as it would arrive in BODY[] (with an explicit
    // Content-Type; MimeParser parks type-less parts as attachments).
    const char rawMessage[] = "From: a@example.test\r\n"
                              "Subject: =?UTF-8?B?5rWL6K+V?=\r\n"
                              "Content-Type: text/plain; charset=utf-8\r\n"
                              "\r\n"
                              "Body line\r\n";
    auto *att = messageAtt({bodySectionItem(rawMessage)});
    const QByteArray body = ningmail::bodyFromMsgAtt(att);
    const auto parsed = ningmail::MimeParser::parse(body);
    REQUIRE(parsed.ok());
    // The transport layer feeds the full raw message (headers + body) to
    // MimeParser::parse; messageId is stamped from header.id by the backend,
    // keeping header.id ≡ uid intact end to end.
    REQUIRE(parsed.value->subject == QString::fromUtf8("测试"));
    REQUIRE(parsed.value->plainText.contains(QString::fromUtf8("Body line")));
}

#else // NINGMAIL_WITH_LIBETPAN

// libetpan is absent: no adapter, nothing to assert.
TEST_CASE("libetpan is not available on this host", "[libetpan]") {
    INFO("libetpan-dev missing: helpers/backend compiled out (NINGMAIL_WITH_LIBETPAN=0)");
    SUCCEED();
}

#endif // NINGMAIL_WITH_LIBETPAN
