#include "LiteHtmlRenderer.h"

#include <QElapsedTimer>
#include <QImage>
#include <QString>

#include <catch2/catch_test_macros.hpp>

// The plan snippet for the long table uses QString("...").repeated(2000), which is the
// exact form kept below (QString::repeated(qsizetype) exists in Qt 6.8).

TEST_CASE("renders simple html to image", "[render]") {
    ningmail::LiteHtmlRenderer r;
    auto image = r.render("<p>你好</p>", 600);
    REQUIRE_FALSE(image.isNull());
    REQUIRE(image.width() == 600);
    REQUIRE(image.height() > 0);
    REQUIRE(r.lastHeight() == image.height());
}

TEST_CASE("renders table and long html under budget", "[render][perf]") {
    ningmail::LiteHtmlRenderer r;
    QString html = "<table>" + QString("<tr><td>row</td></tr>").repeated(2000) + "</table>";
    QElapsedTimer t;
    t.start();
    auto image = r.render(html, 800);
    REQUIRE_FALSE(image.isNull());
    REQUIRE(t.elapsed() < 5000);
    REQUIRE(image.height() <= 20000);
}

TEST_CASE("does not crash on script or malformed html", "[render]") {
    ningmail::LiteHtmlRenderer r;
    auto image = r.render("<script>alert(1)</script><p>ok", 600);
    REQUIRE_FALSE(image.isNull());
}

TEST_CASE("degrades oversized html", "[render]") {
    ningmail::LiteHtmlRenderer r;
    QString html = "<p>" + QString("x").repeated(1024 * 1024 + 1) + "</p>";
    auto image = r.render(html, 600);
    REQUIRE_FALSE(image.isNull());
    REQUIRE(r.lastError() == "html-too-large");
    REQUIRE(image.height() <= 200);
}
