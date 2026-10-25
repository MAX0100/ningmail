#include "SingleInstance.h"

#include "Types.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocalServer>
#include <QProcess>
#include <QSignalSpy>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QTest>
#include <catch2/catch_test_macros.hpp>

#include <memory>

// Task 15 [single-instance]: one per-user key; the first process becomes the
// primary and listens, later ones detect it and only forward arguments through
// sendArguments() (R93⑤, all main-thread with QTRY waits).
TEST_CASE("SingleInstance makes the first check primary", "[single-instance]") {
    QTemporaryDir dir;
    const QString key = dir.filePath("ningmail-test-instance");

    auto primary = std::make_unique<ningmail::SingleInstance>(key);
    CHECK(primary->isPrimary());

    // A stale socket file without a live owner must not block a new primary.
    SECTION("a later check is secondary while the primary lives") {
        ningmail::SingleInstance secondary(key);
        CHECK_FALSE(secondary.isPrimary());
    }
    SECTION("a new primary appears after the previous one exits") {
        primary.reset();
        ningmail::SingleInstance next(key);
        CHECK(next.isPrimary());
    }
}

TEST_CASE("SingleInstance forwards arguments to the primary", "[single-instance]") {
    QTemporaryDir dir;
    const QString key = dir.filePath("ningmail-test-instance");

    ningmail::SingleInstance primary(key);
    REQUIRE(primary.isPrimary());
    QSignalSpy received(&primary, &ningmail::SingleInstance::argumentsReceived);

    ningmail::SingleInstance secondary(key);
    REQUIRE_FALSE(secondary.isPrimary());
    CHECK(secondary.sendArguments(
        QStringList{QStringLiteral("mailto:x@y.com"), QStringLiteral("plain argument")}));

    QTRY_COMPARE(received.count(), 1);
    const QStringList arguments = received.at(0).at(0).value<QStringList>();
    CHECK(arguments ==
          QStringList{QStringLiteral("mailto:x@y.com"), QStringLiteral("plain argument")});
}

TEST_CASE("SingleInstance sendArguments fails when the primary has gone", "[single-instance]") {
    QTemporaryDir dir;
    const QString key = dir.filePath("ningmail-test-instance");

    auto primary = std::make_unique<ningmail::SingleInstance>(key);
    REQUIRE(primary->isPrimary());
    // A secondary holding the key survives its primary; a forward then finds
    // no live owner anymore.
    ningmail::SingleInstance secondary(key);
    REQUIRE_FALSE(secondary.isPrimary());
    primary.reset();

    CHECK_FALSE(secondary.sendArguments(QStringList{QStringLiteral("mailto:x@y.com")}));
    // R94①: with the primary dead, its lock file is stale; reclaiming it
    // transfers the instance identity to the former secondary.
    CHECK(secondary.isPrimary());
}

TEST_CASE("SingleInstance recovers from a stale lock file", "[single-instance]") {
    QTemporaryDir dir;
    const QString key = dir.filePath("ningmail-test-instance");

    // Remnant lock construction: QLockFile's on-disk format is
    // "PID\nappname\nhostname\nhostid\nbootid\n..." — the first line carries
    // the owner PID. The owner below is genuinely reaped
    // (started, terminated, waited) so its PID is dead, not a zombie.
    QProcess remnant;
    remnant.start(QStringLiteral("sleep"), QStringList{QStringLiteral("30")});
    REQUIRE(remnant.waitForStarted());
    const qint64 deadPid = remnant.processId();
    remnant.terminate();
    REQUIRE(remnant.waitForFinished());
    {
        QFile stale(key + QStringLiteral(".lock"));
        REQUIRE(stale.open(QIODevice::WriteOnly | QIODevice::Truncate));
        const QByteArray contents =
            QByteArray::number(deadPid) + '\n' + QSysInfo::machineHostName().toUtf8() + '\n' + '\n';
        CHECK(stale.write(contents) == contents.size());
    }

    // Identity lives in the lock (R94①): a new instance must take over the
    // dead owner's lock and become the primary.
    ningmail::SingleInstance next(key);
    CHECK(next.isPrimary());
}

TEST_CASE("SingleInstance keeps the lock beside the socket for relative keys",
          "[single-instance]") {
    // R95①: a relative key makes QLocalServer resolve the socket into
    // QDir::tempPath() (qlocalserver_unix). The identity lock must mirror that
    // resolution — a CWD-relative lock would fragment per working directory
    // into multiple primaries. This is the production-shape escape point: the
    // other cases use absolute keys (QTemporaryDir) and cannot catch it.
    const QString key =
        QStringLiteral("ningmail-test-single-%1").arg(QCoreApplication::applicationPid());
    // Unique per test process; explicit cleanup keeps parallel ctest runs safe.
    const QString lockPath = QDir::tempPath() + u'/' + key + QStringLiteral(".lock");
    QFile::remove(lockPath);
    QLocalServer::removeServer(key);

    {
        ningmail::SingleInstance primary(key);
        CHECK(primary.isPrimary());
        // The lock must exist in temp — and never have been created relative
        // to the current working directory.
        CHECK(QFileInfo::exists(lockPath));
    }

    QLocalServer::removeServer(key);
    QFile::remove(lockPath);
}
