#include "HtmlSanitizer.h"

#include <QString>
#include <QtGlobal>

#include <catch2/catch_test_macros.hpp>

// NOTE: the plan snippet used QString("A").repeated(90000), which does not exist on QString;
// QString(90000, 'A') is the equivalent form (controller-approved substitution).

TEST_CASE("removes script iframe and event handlers", "[sanitizer]") {
    auto out = ningmail::HtmlSanitizer::sanitize(
        "<p onclick=\"x()\">hi</p><script>alert(1)</script><iframe src=\"x\"></iframe>");
    REQUIRE(out.html.contains("<p>hi</p>"));
    REQUIRE_FALSE(out.html.contains("script"));
    REQUIRE_FALSE(out.html.contains("iframe"));
    REQUIRE_FALSE(out.html.contains("onclick"));
}

TEST_CASE("removes javascript and remote images", "[sanitizer]") {
    auto out = ningmail::HtmlSanitizer::sanitize(
        "<a href=\"javascript:alert(1)\">x</a><img src=\"http://tracker/x.gif\">");
    REQUIRE_FALSE(out.html.contains("javascript:"));
    REQUIRE_FALSE(out.html.contains("http://tracker"));
    REQUIRE(out.warnings.contains("remote-image-blocked"));
}

TEST_CASE("keeps tables and cid images", "[sanitizer]") {
    auto out = ningmail::HtmlSanitizer::sanitize(
        "<table><tr><td>a</td></tr></table><img src=\"cid:img1\">");
    REQUIRE(out.html.contains("<table>"));
    REQUIRE(out.html.contains("cid:img1"));
}

TEST_CASE("drops oversized data images", "[sanitizer]") {
    // QString(90000, 'A') replaces the plan's QString("A").repeated(90000) (no such API).
    QString big = "data:image/png;base64," + QString(90000, 'A');
    auto out = ningmail::HtmlSanitizer::sanitize("<img src=\"" + big + "\">");
    REQUIRE_FALSE(out.html.contains("data:image/png"));
    REQUIRE(out.warnings.contains("data-image-too-large"));
}

TEST_CASE("escapes text content", "[sanitizer]") {
    auto out = ningmail::HtmlSanitizer::sanitize("<p>a &lt; b &amp; c &gt; d</p>");
    // Gumbo decodes the entities, rebuild escapes exactly once: semantically identical to the
    // source.
    REQUIRE(out.html.contains("a &lt; b &amp; c &gt; d"));
    auto out2 = ningmail::HtmlSanitizer::sanitize("<p>&lt;script&gt;</p>");
    // Round trip: the original "&lt;" literal must not be double-escaped into "&amp;lt;".
    REQUIRE_FALSE(out2.html.contains("&amp;lt;"));
    REQUIRE(out2.html.contains("&lt;script&gt;"));
}

TEST_CASE("unwraps unknown tags", "[sanitizer]") {
    auto out =
        ningmail::HtmlSanitizer::sanitize("<p>before<web-component>inner</web-component>after</p>");
    REQUIRE(out.html.contains("<p>"));
    REQUIRE(out.html.contains("inner"));
    REQUIRE(out.html.contains("after"));
    REQUIRE_FALSE(out.html.contains("web-component"));
}

TEST_CASE("removes relative path img src with unsafe-url warning", "[sanitizer]") {
    auto out = ningmail::HtmlSanitizer::sanitize("<img src=\"images/logo.png\">");
    REQUIRE_FALSE(out.html.contains("images/logo.png"));
    REQUIRE(out.warnings.contains("unsafe-url-blocked"));
}

// Fix round 1 (R68): cleanCss must survive comment-obfuscated and whitespace-split variants.

TEST_CASE("removes comment-obfuscated css import", "[sanitizer]") {
    auto out = ningmail::HtmlSanitizer::sanitize("<style>@/*x*/import \"evil.css\";</style>");
    // Stripping the comment first exposes the bare @import so the pattern pass removes it; neither
    // the comment residue nor the import keyword may survive the rebuild.
    REQUIRE_FALSE(out.html.contains("/*x*/"));
    REQUIRE_FALSE(out.html.contains("@import"));
}

TEST_CASE("removes whitespace-split position fixed inline style", "[sanitizer]") {
    auto out = ningmail::HtmlSanitizer::sanitize("<div style=\"position: fixed; top:0\">t</div>");
    // "position: fixed" (with whitespace) must be caught like "position:fixed".
    REQUIRE_FALSE(out.html.contains("position"));
}

// Fix round 1 (R69): the data: gate only accepts image/* + base64 + decodable + <=64KB payloads;
// everything except oversize decodes yields data-image-invalid.

TEST_CASE("rejects non-image data mediatype and empty payload", "[sanitizer]") {
    auto out = ningmail::HtmlSanitizer::sanitize("<img src=\"data:text/html;base64,PGI+\">");
    REQUIRE_FALSE(out.html.contains("data:text/html"));
    REQUIRE(out.warnings.contains("data-image-invalid"));

    auto out2 = ningmail::HtmlSanitizer::sanitize("<img src=\"data:image/png;base64,\">");
    REQUIRE_FALSE(out2.html.contains("data:image/png"));
    REQUIRE(out2.warnings.contains("data-image-invalid"));
}

// Fix round 2: an optional whitespace around the ';' separator must not break the base64
// parameter lookup ("image/png; base64,..." is the same media type list per R69 intent).

TEST_CASE("accepts data uri base64 parameter with surrounding whitespace", "[sanitizer]") {
    auto out = ningmail::HtmlSanitizer::sanitize("<img src=\"data:image/png; base64,PGI+\">");
    REQUIRE(out.html.contains("base64,PGI+"));
    REQUIRE(out.warnings.isEmpty());
}
