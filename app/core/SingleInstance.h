#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include <memory>

class QLockFile;
class QLocalServer;
class QLocalSocket;

namespace ningmail {

/// Task 15 single-instance gate: two cooperating primitives.
///
/// Identity lives in a QLockFile (`lock = key + ".lock"` next to the resolved
/// socket): the actual mutex is flock(LOCK_EX) held for the primary's
/// lifetime, so two instances starting near-simultaneously cannot both probe
/// "no primary" and both claim the socket (the R93 TOCTOU, caught in review
/// round 1 / R94①). The QLocalServer key is only the transport: keep-alives
/// between a secondary and the primary, plus the one-shot JSON-array forwards
/// from a secondary through sendArguments().
///
/// Staleness is explicit, not heuristic (R94/R95②): crash remnants are
/// reclaimed through the failed-forward path's removeStaleLockFile() — an
/// open+flock+unlink that only ever deletes a lock nobody currently holds
/// (a live primary's flock blocks the removal). m_lock->setStaleLockTime(0)
/// disables Qt's default mtime rule entirely: on NFS/FUSE an age-based rule
/// could "steal" a live primary's lock once 30 s of mtime elapsed, which
/// would break the single-instance identity itself.
///
/// Linux semantics are the reference (target platform; the Kylin deployment
/// relies on them): unix-domain socket keys may be absolute temp paths or
/// relative names resolved into QDir::tempPath() — the lock path mirrors that
/// resolution. Windows named-pipe key semantics remain deferred (R93-family
/// disclosure).
///
/// Everything happens on the calling (GUI) thread; the primary parses one
/// JSON array per connection and emits argumentsReceived once the peer
/// half-closes.
class SingleInstance : public QObject {
    Q_OBJECT
  public:
    explicit SingleInstance(QString key = QString(), QObject *parent = nullptr);
    ~SingleInstance() override;

    SingleInstance(const SingleInstance &) = delete;
    SingleInstance &operator=(const SingleInstance &) = delete;

    bool isPrimary() const {
        return m_primary;
    }

    /// Secondary side: hand `arguments` (one JSON array write) to the primary.
    /// False when no primary is reachable; a failed forward additionally
    /// retries ownership once against a stale lock (dead primary) and may
    /// promote this instance to primary (R94①).
    bool sendArguments(const QStringList &arguments);

  signals:
    void argumentsReceived(const QStringList &arguments);

  private:
    /// Assumes the already-acquired lock: clears a stale socket file and
    /// starts the transport. m_primary is true even when listen fails —
    /// the lock (identity) is authoritative, the socket is not.
    void takeOwnership();
    void acceptConnection(QLocalSocket *socket);

    QString m_key;
    bool m_primary = false;
    /// Non-null only while this instance is the primary.
    QLocalServer *m_server = nullptr;
    /// Identity holder; held for the object's lifetime in the primary.
    std::unique_ptr<QLockFile> m_lock;
};

} // namespace ningmail
