#include "MailtoParser.h"

#include "Types.h"

#include <QUrl>
#include <catch2/catch_test_macros.hpp>

// Task 15 [mailto]: the parser contract per R93③ — `mailto:` scheme is
// mandatory, recipients come from the URL path plus the `to` query key,
// cc/bcc/subject/body live in the query, unknown keys are ignored, duplicate
// keys fall back to the first one, empty `to` stays allowed, and `+` keeps its
// literal meaning (RFC 6068 / QUrlQuery FullyDecoded, not form encoding).
TEST_CASE("MailtoParser parses the canonical mailto round trip", "[mailto]") {
    const auto parsed = ningmail::MailtoParser::parse(
        QUrl(QStringLiteral("mailto:a@b.com?subject=Hello%20World&cc=c@d.com&body=Line1%0ALine2")));
    REQUIRE(parsed.ok());

    const auto &request = *parsed.value;
    CHECK(request.to == QStringList{QStringLiteral("a@b.com")});
    CHECK(request.cc == QStringList{QStringLiteral("c@d.com")});
    CHECK(request.subject == QStringLiteral("Hello World"));
    CHECK(request.body == QStringLiteral("Line1\nLine2"));
}

TEST_CASE("MailtoParser splits comma-separated recipients", "[mailto]") {
    const auto parsed =
        ningmail::MailtoParser::parse(QUrl(QStringLiteral("mailto:a@b.com,  c@d.com")));
    REQUIRE(parsed.ok());
    CHECK(parsed.value->to == QStringList{QStringLiteral("a@b.com"), QStringLiteral("c@d.com")});
}

TEST_CASE("MailtoParser appends query to-recipients after the URL path", "[mailto]") {
    const auto parsed = ningmail::MailtoParser::parse(
        QUrl(QStringLiteral("mailto:a%2Db@c.com?to=d%40e.com&to=f%40g.com")));
    REQUIRE(parsed.ok());
    // Path recipient first, then the first query `to` value; the second `to`
    // is a duplicate key and must be dropped.
    CHECK(parsed.value->to == QStringList{QStringLiteral("a-b@c.com"), QStringLiteral("d@e.com")});
}

TEST_CASE("MailtoParser keeps plus signs literal and decodes percent escapes", "[mailto]") {
    SECTION("percent-encoded space decodes") {
        const auto parsed = ningmail::MailtoParser::parse(
            QUrl(QStringLiteral("mailto:x@y.com?subject=Hello%20World")));
        REQUIRE(parsed.ok());
        CHECK(parsed.value->subject == QStringLiteral("Hello World"));
    }
    SECTION("a raw plus stays a plus") {
        const auto parsed =
            ningmail::MailtoParser::parse(QUrl(QStringLiteral("mailto:x@y.com?subject=a+b")));
        REQUIRE(parsed.ok());
        CHECK(parsed.value->subject == QStringLiteral("a+b"));
    }
}

TEST_CASE("MailtoParser parses bcc recipients", "[mailto]") {
    const auto parsed =
        ningmail::MailtoParser::parse(QUrl(QStringLiteral("mailto:a@b.com?bcc=hidden@e.com")));
    REQUIRE(parsed.ok());
    CHECK(parsed.value->bcc == QStringList{QStringLiteral("hidden@e.com")});
}

TEST_CASE("MailtoParser allows an empty recipient list", "[mailto]") {
    const auto parsed =
        ningmail::MailtoParser::parse(QUrl(QStringLiteral("mailto:?subject=Hi%20there")));
    REQUIRE(parsed.ok());
    CHECK(parsed.value->to.isEmpty());
    CHECK(parsed.value->subject == QStringLiteral("Hi there"));
}

TEST_CASE("MailtoParser ignores unknown query keys", "[mailto]") {
    const auto parsed = ningmail::MailtoParser::parse(
        QUrl(QStringLiteral("mailto:a@b.com?x=1&subject=First&subject=Second")));
    REQUIRE(parsed.ok());
    CHECK(parsed.value->cc.isEmpty());
    CHECK(parsed.value->bcc.isEmpty());
    CHECK(parsed.value->subject == QStringLiteral("First"));
}

TEST_CASE("MailtoParser rejects non-mailto URLs", "[mailto]") {
    CHECK_FALSE(
        ningmail::MailtoParser::parse(QUrl(QStringLiteral("https://example.com/?x=1"))).ok());
    CHECK_FALSE(ningmail::MailtoParser::parse(QUrl()).ok());
}
