#include "SingleInstance.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>

#include <memory>

namespace ningmail {

namespace {

/// One-shot probe/forward budget; short on purpose so a dead-but-stale socket
/// file (crashed run) falls through to the reclaim path fast.
constexpr int kConnectTimeoutMs = 500;

} // namespace

SingleInstance::SingleInstance(QString key, QObject *parent)
    : QObject(parent),
      m_key(key.isEmpty() ? QStringLiteral("ningmail-single-instance") : std::move(key)) {
    // R94①: the lock file decides ownership atomically, so two instances
    // starting in the same instant cannot both conclude "no primary" from a
    // socket probe and both take the transport.
    // R95①: the lock path must mirror QLocalServer's relative-key resolution
    // (qlocalserver_unix resolves it into QDir::tempPath()); a CWD-relative
    // lock would silently fragment per working directory into multiple
    // primaries. Absolute keys (tests) pass through unchanged, matching the
    // socket's as-is handling of absolute paths.
    m_lock = std::make_unique<QLockFile>(QDir::temp().filePath(m_key + QStringLiteral(".lock")));
    // R95②: never age out a held lock by mtime — under the default 30 s rule,
    // NFS/FUSE filesystems can let a live primary's lock be stolen. Crash
    // remnants are reclaimed explicitly on a failed forward instead (the
    // send-failure path's removeStaleLockFile()).
    m_lock->setStaleLockTime(0);
    if (m_lock->tryLock(0)) {
        // Holding the lock already guarantees there is no live primary.
        takeOwnership();
        return;
    }

    // Secondary. The lock owner is alive by definition of tryLock failing;
    // the probe only verifies the transport exists and disconnects silently.
    // A probe connection reaching the primary carries no payload and is
    // ignored there (see acceptConnection).
    QLocalSocket probe;
    probe.connectToServer(m_key);
    if (probe.waitForConnected(kConnectTimeoutMs)) {
        probe.disconnectFromServer();
    }
}

SingleInstance::~SingleInstance() = default;

void SingleInstance::takeOwnership() {
    // R94①: identity comes from the lock, which is already held. If the
    // transport cannot come up (internal error — permissions, path length, …)
    // this instance stays primary but without IPC: logged honestly instead of
    // silently handing the identity back.
    m_primary = true;
    // A stale socket file from a crashed previous primary must be dropped
    // before listen succeeds (a second listen reports "address in use").
    QLocalServer::removeServer(m_key);
    m_server = new QLocalServer(this);
    if (!m_server->listen(m_key)) {
        qWarning() << "SingleInstance: primary lock acquired but listening on" << m_key
                   << "failed:" << m_server->errorString();
        delete m_server;
        m_server = nullptr;
        return;
    }
    connect(m_server, &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket *pending = m_server->nextPendingConnection()) {
            acceptConnection(pending);
        }
    });
}

void SingleInstance::acceptConnection(QLocalSocket *socket) {
    // Per-connection buffer (R94②): readyRead appends, and disconnected()
    // performs the final unified readAll. After the peer closes, the kernel
    // keeps data already in the unix-domain socket buffer readable, so the
    // last chunk sent before EOF is never lost.
    const auto buffer = std::make_shared<QByteArray>();
    connect(socket, &QLocalSocket::readyRead, this,
            [socket, buffer] { buffer->append(socket->readAll()); });
    connect(socket, &QLocalSocket::disconnected, this, [this, socket, buffer] {
        buffer->append(socket->readAll()); // final unified read (see above)
        socket->deleteLater();
        // A probe connection (another instance's ownership check) closes
        // without any payload: not an error, just silence.
        if (buffer->isEmpty()) {
            return;
        }
        const QJsonDocument document = QJsonDocument::fromJson(*buffer);
        if (!document.isArray()) {
            // Never crash the primary over a malformed forward.
            qWarning("SingleInstance: ignoring malformed arguments from a second instance");
            return;
        }
        const QJsonArray array = document.array();
        // An empty array (a forward of "no arguments") is equally inert.
        if (array.isEmpty()) {
            return;
        }
        QStringList arguments;
        for (const QJsonValue &value : array) {
            if (value.isString()) {
                arguments.append(value.toString());
            }
        }
        emit argumentsReceived(arguments);
    });
}

bool SingleInstance::sendArguments(const QStringList &arguments) {
    QLocalSocket socket;
    socket.connectToServer(m_key);
    if (socket.waitForConnected(kConnectTimeoutMs)) {
        // One-shot JSON array write; disconnectFromServer() half-closes so the
        // primary sees the message boundary without a length prefix.
        socket.write(QJsonDocument::fromVariant(arguments).toJson(QJsonDocument::Compact));
        socket.flush();
        socket.disconnectFromServer();
        if (socket.state() != QLocalSocket::UnconnectedState) {
            socket.waitForDisconnected(kConnectTimeoutMs);
        }
        return true;
    }
    qWarning() << "SingleInstance: no primary reachable for argument forwarding";

    // The primary may have died between this secondary's start and this
    // forward, leaving the lock file behind with a dead PID. Linux QLockFile
    // staleness covers exactly this; drop the remnant and retry ownership
    // once (R94①). A second retry is deliberately not attempted.
    m_lock->removeStaleLockFile();
    if (m_lock->tryLock(0)) {
        qWarning() << "SingleInstance: reclaiming the dead primary's lock" << m_key;
        takeOwnership();
    } else {
        qWarning() << "SingleInstance: single-instance IPC unavailable for" << m_key;
    }
    return false;
}

} // namespace ningmail
