#include "Charset.h"

#include "MimeParser.h"

#include "MailError.h"
#include "Types.h"

#include <QByteArray>
#include <QDateTime>
#include <QFile>
#include <QString>
#include <QtGlobal>

#include <catch2/catch_test_macros.hpp>

namespace {

QByteArray readFixture(const char *name) {
    QFile file(QString::fromLatin1(NINGMAIL_FIXTURES_DIR) + QLatin1Char('/') + QLatin1String(name));
    REQUIRE(file.open(QIODevice::ReadOnly));
    return file.readAll();
}

} // namespace

TEST_CASE("parses plain UTF-8 message", "[mime]") {
    auto msg = ningmail::MimeParser::parse(readFixture("plain_utf8.eml"));
    REQUIRE(msg.ok());
    REQUIRE(msg.value->subject == QString::fromUtf8("Hello"));
    REQUIRE(msg.value->plainText.contains(QString::fromUtf8("你好，NingMail")));
    REQUIRE(msg.value->html.isEmpty());
    REQUIRE(msg.value->from.name == QString::fromUtf8("Sender"));
    REQUIRE(msg.value->from.address == QString::fromUtf8("sender@example.com"));
    REQUIRE(msg.value->date.isValid());
}

TEST_CASE("decodes GB2312 RFC2047 subject", "[mime]") {
    auto raw = readFixture("gb2312_subject.eml");
    auto msg = ningmail::MimeParser::parse(raw);
    REQUIRE(msg.ok());
    REQUIRE(msg.value->subject == QString::fromUtf8("你好"));
}

TEST_CASE("decodes RFC2047 words separated by whitespace", "[mime]") {
    const auto decoded = ningmail::MimeParser::decodeHeader("=?UTF-8?B?5L2g?= =?UTF-8?B?5aW9?=");
    REQUIRE(decoded == QString::fromUtf8("你好"));
}

TEST_CASE("decodes GB18030 HTML body via charset parameter", "[mime]") {
    auto msg = ningmail::MimeParser::parse(readFixture("gb18030_html.eml"));
    REQUIRE(msg.ok());
    REQUIRE(msg.value->plainText.isEmpty());
    REQUIRE(msg.value->html.contains(QString::fromUtf8("中文")));
}

TEST_CASE("parses multipart/alternative with both plain and html", "[mime]") {
    auto msg = ningmail::MimeParser::parse(readFixture("multipart_alternative.eml"));
    REQUIRE(msg.ok());
    REQUIRE(msg.value->plainText.contains(QString::fromUtf8("plain body")));
    REQUIRE(msg.value->html.contains(QString::fromUtf8("<p>html</p>")));
    REQUIRE(msg.value->attachments.isEmpty());
}

TEST_CASE("collects inline attachment from multipart related Content-ID", "[mime]") {
    auto msg = ningmail::MimeParser::parse(readFixture("cid_inline.eml"));
    REQUIRE(msg.ok());
    REQUIRE(msg.value->html.contains(QString::fromUtf8("cid:img1")));
    REQUIRE(msg.value->attachments.size() == 1);
    const auto &attachment = msg.value->attachments.first();
    REQUIRE(attachment.contentId == QString::fromUtf8("img1"));
    REQUIRE(attachment.isInline);
    REQUIRE(attachment.filename == QString::fromUtf8("logo.png"));
    REQUIRE(attachment.mimeType == QString::fromUtf8("image/png"));
    REQUIRE(attachment.size == 8);
}

TEST_CASE("decodes quoted-printable body", "[mime]") {
    auto msg = ningmail::MimeParser::parse(readFixture("quoted_printable.eml"));
    REQUIRE(msg.ok());
    REQUIRE(msg.value->plainText.contains(QString::fromUtf8("中文")));
}

TEST_CASE("decodes RFC2231 extended parameter filename", "[mime]") {
    auto msg = ningmail::MimeParser::parse(readFixture("rfc2231_filename.eml"));
    REQUIRE(msg.ok());
    REQUIRE(msg.value->attachments.size() == 1);
    REQUIRE(msg.value->attachments.first().filename == QString::fromUtf8("中文.txt"));
    REQUIRE(msg.value->plainText.isEmpty());
}

TEST_CASE("truncated multipart returns Protocol error", "[mime]") {
    auto raw = readFixture("truncated_multipart.eml");
    auto msg = ningmail::MimeParser::parse(raw);
    REQUIRE_FALSE(msg.ok());
    REQUIRE(msg.error.code == ningmail::ErrorCode::Protocol);
}

TEST_CASE("deeply nested multipart stops at depth limit", "[mime]") {
    auto raw = readFixture("deep_nesting.eml");
    auto msg = ningmail::MimeParser::parse(raw);
    REQUIRE_FALSE(msg.ok());
    REQUIRE(msg.error.code == ningmail::ErrorCode::Protocol);
    // The depth guard fires (not a missing boundary): fixture closes all 25 nested
    // multiparts, so the only possible failure is the depth limit.
    REQUIRE(msg.error.message.contains(QString::fromUtf8("depth")));
}

TEST_CASE("missing From and Date fall back to defaults", "[mime]") {
    auto msg = ningmail::MimeParser::parse(readFixture("bad_header.eml"));
    REQUIRE(msg.ok());
    REQUIRE(msg.value->from == ningmail::MailAddress{});
    REQUIRE(msg.value->to.isEmpty());
    const qint64 delta = msg.value->date.secsTo(QDateTime::currentDateTimeUtc());
    REQUIRE(qAbs(delta) < 5);
}

TEST_CASE("Charset::decode normalizes gb2312 to GB18030", "[mime]") {
    const QByteArray bytes = QByteArray::fromBase64("xOO6ww==");
    REQUIRE(ningmail::Charset::decode(bytes, "gb2312") == QString::fromUtf8("你好"));
}

TEST_CASE("Charset::decode treats empty charset as UTF-8 and falls back to Latin1", "[mime]") {
    REQUIRE(ningmail::Charset::decode(QByteArray("你好"), "") == QString::fromUtf8("你好"));
    REQUIRE(ningmail::Charset::decode(QByteArray("\xE9"), "latin1") == QString::fromUtf8("é"));
}

TEST_CASE("MimeParser::decodeText delegates to Charset", "[mime]") {
    REQUIRE(ningmail::MimeParser::decodeText(QByteArray::fromBase64("xOO6ww=="), "gb18030") ==
            QString::fromUtf8("你好"));
}

// --- Robustness: hostile input policies (lenient reads, no crash, no hang) ---

TEST_CASE("address splitting tolerates unclosed quotes and angle brackets", "[mime]") {
    // "a@b, "x <y, c@d": the unclosed quote must swallow the rest as one token
    // (a@d garbage never blocks a@b) instead of throwing or looping forever.
    const QByteArray quoted =
        QByteArray("From: sender@example.com\r\n"
                   "To: a@b, \"x <y, c@d\r\n"
                   "Subject: t\r\n"
                   "Content-Type: text/plain; charset=\"utf-8\"\r\n\r\nhi\r\n");
    auto msg = ningmail::MimeParser::parse(quoted);
    REQUIRE(msg.ok());
    REQUIRE(msg.value->to.size() == 2);
    REQUIRE(msg.value->to.first().name.isEmpty());
    REQUIRE(msg.value->to.first().address == QString::fromUtf8("a@b"));
    // The unclosed-quote remainder collapses deterministically into one address.
    REQUIRE(msg.value->to.last().name == QString::fromUtf8("\"x"));
    REQUIRE(msg.value->to.last().address == QString::fromUtf8("y, c@d"));

    // Unclosed angle bracket: commas inside stay part of the same token.
    const QByteArray angled =
        QByteArray("From: sender@example.com\r\n"
                   "To: a@b, <p@q, r@s\r\n"
                   "Subject: t\r\n"
                   "Content-Type: text/plain; charset=\"utf-8\"\r\n\r\nhi\r\n");
    auto msg2 = ningmail::MimeParser::parse(angled);
    REQUIRE(msg2.ok());
    REQUIRE(msg2.value->to.size() == 2);
    REQUIRE(msg2.value->to.last().address == QString::fromUtf8("p@q, r@s"));

    // Unclosed quote in a parameter value: lenient read, body still decodes.
    const QByteArray param = QByteArray("From: sender@example.com\r\n"
                                        "Subject: t\r\n"
                                        "Content-Type: text/plain; charset=\"utf-8\r\n\r\n"
                                        "你好\r\n");
    auto msg3 = ningmail::MimeParser::parse(param);
    REQUIRE(msg3.ok());
    REQUIRE(msg3.value->plainText.contains(QString::fromUtf8("你好")));
}

TEST_CASE("malformed encoded words are treated as plain text", "[mime]") {
    // Valid word, then a truncated one (no closing ?=), then another valid word:
    // decoded parts decode, the malformed part stays verbatim, whitespace around
    // malformed words is kept (only whitespace between valid words is dropped).
    REQUIRE(ningmail::MimeParser::decodeHeader("=?UTF-8?B?5L2g?= =?UTF-8?B?abc =?UTF-8?B?5aW9?=") ==
            QString::fromUtf8("你 =?UTF-8?B?abc 好"));

    // A word that only ends mid-payload keeps its text untouched.
    REQUIRE(ningmail::MimeParser::decodeHeader("Subject: =?UTF-8?B?abc") ==
            QString::fromUtf8("Subject: =?UTF-8?B?abc"));

    // A bare "=?" prefix never hangs or throws.
    REQUIRE(ningmail::MimeParser::decodeHeader("a=?") == QString::fromUtf8("a=?"));
}

TEST_CASE("Q payload keeps malformed = sequences verbatim", "[mime]") {
    // Non-hex after '=': keep the '=' and continue.
    REQUIRE(ningmail::MimeParser::decodeHeader("=?ISO-8859-1?Q?a=XYb?=") ==
            QString::fromUtf8("a=XYb"));
    // '=' with fewer than two bytes left: keep the trailing '='.
    REQUIRE(ningmail::MimeParser::decodeHeader("=?ISO-8859-1?Q?a=X?=") == QString::fromUtf8("a=X"));
    // Complete payload with padding '=' stays as-is.
    REQUIRE(ningmail::MimeParser::decodeHeader("=?ISO-8859-1?Q?ab=?=") == QString::fromUtf8("ab="));
    // A word missing its final terminator stays verbatim (malformed-word policy).
    REQUIRE(ningmail::MimeParser::decodeHeader("=?ISO-8859-1?Q?ab=") ==
            QString::fromUtf8("=?ISO-8859-1?Q?ab="));
}

TEST_CASE("Charset::decode accepts literal U+FFFD bytes", "[mime]") {
    // EF BF BD is the UTF-8 encoding of U+FFFD itself; an input that legitimately
    // contains it must not be demoted to Latin1 by the replacement-character check.
    const QByteArray bytes = QByteArray("\xEF\xBF\xBD"
                                        "\xE4\xBD\xA0\xE5\xA5\xBD");
    // "x-unknown" is not openable in ICU, forcing the UTF-8/Latin1 fallback chain.
    REQUIRE(ningmail::Charset::decode(bytes, "x-unknown") == QString::fromUtf8(bytes));
}
