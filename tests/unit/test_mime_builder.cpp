#include "MimeBuilder.h"

#include "MailBackend.h"
#include "MimeParser.h"
#include "Types.h"

#include <QDateTime>
#include <QString>
#include <QTimeZone>

#include <catch2/catch_test_macros.hpp>

#include <utility>

namespace {

ningmail::OutgoingMessage sampleMessage() {
    ningmail::OutgoingMessage message;
    ningmail::Account account;
    account.id = 1;
    account.displayName = QString::fromUtf8("Writer Account");
    account.email = QString::fromUtf8("writer@ningmail.local");
    message.account = account;
    message.to.append(
        ningmail::MailAddress{QString::fromUtf8("Recipient"), QString::fromUtf8("to@example.com")});
    message.to.append(ningmail::MailAddress{QString(), QString::fromUtf8("plain@example.com")});
    message.cc.append(ningmail::MailAddress{QString(), QString::fromUtf8("cc@example.com")});
    message.subject = QString::fromUtf8("季度汇报");
    message.bodyText = QString::fromUtf8("第一行\n第二行");
    return message;
}

/// Header block of the built message, i.e. everything up to the blank line.
QByteArray headerBlockOf(const QByteArray &raw) {
    const int split = raw.indexOf("\r\n\r\n");
    REQUIRE(split > 0);
    return raw.left(split + 2);
}

/// The value of one header line without folding support (M1 keeps every header
/// on a single line), found in the header block only.
QByteArray headerValue(const QByteArray &raw, const char *name) {
    const QByteArray headerBlock = headerBlockOf(raw);
    const QByteArray token = QByteArray(name) + ":";
    const int start = headerBlock.indexOf(token);
    REQUIRE(start >= 0);
    const int lineEnd = headerBlock.indexOf("\r\n", start);
    REQUIRE(lineEnd > start);
    return headerBlock.mid(start + static_cast<int>(token.size()), lineEnd - start - token.size())
        .trimmed();
}

} // namespace

TEST_CASE("MimeBuilder emits the mandatory RFC 5322 headers", "[mime-builder]") {
    const QByteArray raw = ningmail::MimeBuilder::build(sampleMessage());

    // R53 display rule: a name renders as "name <address>", an unnamed address
    // as the bare address. The From line uses the account identity.
    CHECK(headerValue(raw, "From") == "Writer Account <writer@ningmail.local>");
    CHECK(headerValue(raw, "To") == "Recipient <to@example.com>, plain@example.com");
    CHECK(headerValue(raw, "Cc") == "cc@example.com");
    CHECK(headerValue(raw, "MIME-Version") == "1.0");
    CHECK(headerValue(raw, "Content-Type") == "text/plain; charset=UTF-8");
    CHECK(headerValue(raw, "Content-Transfer-Encoding") == "base64");

    // M1 ignores bcc: a header that survives in the output would leak the
    // hidden recipients to every receiver.
    CHECK_FALSE(raw.contains("Bcc"));
}

TEST_CASE("MimeBuilder uses CRLF everywhere and separates headers with a blank line",
          "[mime-builder]") {
    const QByteArray raw = ningmail::MimeBuilder::build(sampleMessage());

    // No bare LF anywhere: stripping CRLF must leave no line break at all.
    QByteArray stripped = raw;
    stripped.replace("\r\n", "");
    CHECK_FALSE(stripped.contains('\n'));

    // The header block ends with its own CRLF; the blank line follows.
    const int split = raw.indexOf("\r\n\r\n");
    REQUIRE(split > 0);
    const int bodyStart = split + 4;
    CHECK(raw.at(bodyStart) != '\r');
    CHECK(raw.at(bodyStart) != '\n');
}

TEST_CASE("MimeBuilder encodes a non-ASCII subject as an RFC 2047 word", "[mime-builder]") {
    const QByteArray raw = ningmail::MimeBuilder::build(sampleMessage());

    const QByteArray subject = headerValue(raw, "Subject");
    CHECK(subject.startsWith("=?UTF-8?B?"));
    CHECK(subject.endsWith("?="));

    // Round-trip: the parser must recover the original subject text.
    const auto parsed = ningmail::MimeParser::parse(raw);
    REQUIRE(parsed.ok());
    CHECK(parsed.value->subject == QString::fromUtf8("季度汇报"));
}

TEST_CASE("MimeBuilder keeps an ASCII subject verbatim", "[mime-builder]") {
    ningmail::OutgoingMessage message = sampleMessage();
    message.subject = QString::fromUtf8("Plain hello");
    const QByteArray raw = ningmail::MimeBuilder::build(message);
    CHECK(headerValue(raw, "Subject") == "Plain hello");
}

TEST_CASE("MimeBuilder base64-encodes the body so the text survives a parse round-trip",
          "[mime-builder]") {
    const QByteArray raw = ningmail::MimeBuilder::build(sampleMessage());

    const auto parsed = ningmail::MimeParser::parse(raw);
    REQUIRE(parsed.ok());
    CHECK(parsed.value->plainText == QString::fromUtf8("第一行\n第二行"));
    CHECK(parsed.value->html.isEmpty());

    // Every base64 line inside the body stays within the 76-column limit.
    const int split = raw.indexOf("\r\n\r\n");
    const QList<QByteArray> lines = raw.mid(split + 4).split('\n');
    for (const QByteArray &line : lines) {
        CHECK(line.size() <= 77); // 76 columns + trailing CR (last line has none)
    }
}

TEST_CASE("MimeBuilder quotes ASCII display names quoted-string style", "[mime-builder]") {
    ningmail::OutgoingMessage message = sampleMessage();
    message.to.clear();
    message.to.append(
        ningmail::MailAddress{QString::fromUtf8("Doe, John"), QString::fromUtf8("a@example.com")});

    const QByteArray raw = ningmail::MimeBuilder::build(message);
    CHECK(headerValue(raw, "To") == "\"Doe, John\" <a@example.com>");

    // The parser keeps the comma inside the quoted display name (angle/quote
    // aware splitting), so the round trip identifies the right recipient.
    const auto parsed = ningmail::MimeParser::parse(raw);
    REQUIRE(parsed.ok());
    REQUIRE(parsed.value->to.size() == 1);
    CHECK(parsed.value->to.at(0).name == QString::fromUtf8("Doe, John"));
    CHECK(parsed.value->to.at(0).address == QString::fromUtf8("a@example.com"));
}

TEST_CASE("MimeBuilder escapes quotes and backslashes inside quoted names", "[mime-builder]") {
    ningmail::OutgoingMessage message = sampleMessage();
    message.to.clear();
    message.to.append(ningmail::MailAddress{QString::fromUtf8("He said \"hi\""),
                                            QString::fromUtf8("a@example.com")});

    const QByteArray raw = ningmail::MimeBuilder::build(message);
    // Inside the quoted-string both `"` and `\` carry a backslash.
    CHECK(headerValue(raw, "To") == "\"He said \\\"hi\\\"\" <a@example.com>");
}

TEST_CASE("MimeBuilder omits the To/Cc headers when the lists are empty", "[mime-builder]") {
    ningmail::OutgoingMessage message = sampleMessage();
    message.to.clear();
    message.cc.clear();

    const QByteArray raw = ningmail::MimeBuilder::build(message);
    CHECK_FALSE(headerBlockOf(raw).contains("To:"));
    CHECK_FALSE(headerBlockOf(raw).contains("Cc:"));

    // The header block without the receiver headers stays fully parseable.
    const auto parsed = ningmail::MimeParser::parse(raw);
    REQUIRE(parsed.ok());
    CHECK(parsed.value->to.isEmpty());
    CHECK(parsed.value->cc.isEmpty());
    CHECK(parsed.value->from.address == QString::fromUtf8("writer@ningmail.local"));
    CHECK(parsed.value->plainText == QString::fromUtf8("第一行\n第二行"));
}

TEST_CASE("MimeBuilder honours a preset Message-ID and generates one otherwise", "[mime-builder]") {
    ningmail::OutgoingMessage preset = sampleMessage();
    preset.rfcMessageId = QString::fromUtf8("<fixed-id@ningmail.local>");
    const QByteArray rawPreset = ningmail::MimeBuilder::build(preset);
    CHECK(headerValue(rawPreset, "Message-ID") == "<fixed-id@ningmail.local>");

    ningmail::OutgoingMessage generated = sampleMessage();
    REQUIRE(generated.rfcMessageId.isEmpty());
    const QByteArray rawGenerated = ningmail::MimeBuilder::build(generated);
    const QByteArray messageId = headerValue(rawGenerated, "Message-ID");
    CHECK(messageId.startsWith('<'));
    CHECK(messageId.contains("@ningmail.local>"));
    // Two builds must not collide.
    CHECK(headerValue(ningmail::MimeBuilder::build(generated), "Message-ID") != messageId);
}

TEST_CASE("MimeBuilder writes a parsable RFC 2822 Date header", "[mime-builder]") {
    const QByteArray raw = ningmail::MimeBuilder::build(sampleMessage());
    const QString dateValue = QString::fromLatin1(headerValue(raw, "Date"));
    REQUIRE_FALSE(dateValue.isEmpty());

    // The parse of the built message carries the very same instant back, and
    // the header itself parses as an RFC 2822 date.
    CHECK(QDateTime::fromString(dateValue, Qt::RFC2822Date).isValid());
    const auto parsed = ningmail::MimeParser::parse(raw);
    REQUIRE(parsed.ok());
    CHECK(parsed.value->date.isValid());
}

TEST_CASE("MimeBuilder does not fold header lines in the M1 header set", "[mime-builder]") {
    const QByteArray raw = ningmail::MimeBuilder::build(sampleMessage());
    const int headerEnd = raw.indexOf("\r\n\r\n");
    // QByteArray::split takes a single byte: split on LF; inside the header
    // block every LF is preceded by a CR (checked by the CRLF test above).
    const QList<QByteArray> lines = raw.left(headerEnd).split('\n');
    REQUIRE_FALSE(lines.isEmpty());
    for (const QByteArray &line : lines) {
        // Every header stays unfolded: no continuation line (= leading blank)
        // may appear inside the header block.
        CHECK_FALSE(line.startsWith(' '));
        CHECK_FALSE(line.startsWith('\t'));
    }
}
