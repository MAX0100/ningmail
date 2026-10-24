#include "SyncService.h"

#include "CredentialStore.h"
#include "MailError.h"

#include <QMetaObject>
#include <QThread>
#include <QtConcurrent/QtConcurrent>

#include <memory>
#include <optional>
#include <utility>

namespace ningmail {

namespace {

/// Single fetch for the inbox headers; per brief step 69 the pipeline stays
/// within one `fetchHeaders(inbox, 1, 500)` call (M1 scope, no paging).
constexpr int kHeaderFetchLimit = 500;
constexpr quint32 kFirstUid = 1;

const Folder *findInbox(const QList<Folder> &folders) {
    for (const Folder &folder : folders) {
        if (folder.role == FolderRole::Inbox) {
            return &folder;
        }
    }
    for (const Folder &folder : folders) {
        if (folder.fullName.compare(QString::fromUtf8("INBOX"), Qt::CaseInsensitive) == 0) {
            return &folder;
        }
    }
    return nullptr;
}

MailError missingCredentialError() {
    return MailError{ErrorCode::Auth, QString::fromUtf8("Credential not found"),
                     QString::fromUtf8("missing credential")};
}

} // namespace

SyncService::SyncService(MailBackendFactory *factory, CredentialStore *credentials, QObject *parent)
    : QObject(parent), m_factory(factory), m_credentials(credentials), m_epoch(0),
      m_activeWorkers(0) {
    // Brief step 74: register the queued-signal payload types (idempotent) so
    // the service stays self-contained even without registerMetaTypes().
    // Account additionally enables queued cross-thread slot invocation.
    qRegisterMetaType<Account>();
    qRegisterMetaType<QList<Folder>>();
    qRegisterMetaType<QList<MessageHeader>>();
    qRegisterMetaType<MessageBody>();
    qRegisterMetaType<MailError>();
}

SyncService::~SyncService() {
    // Invalidate any dispatch that has not run yet, drain the currently
    // tracked future, then wait until every worker has made its last member
    // access, so no thread can race the destruction of the m_* members.
    m_epoch.fetch_add(1, std::memory_order_release);
    if (m_activeFuture.isValid()) {
        m_activeFuture.waitForFinished();
    }
    while (m_activeWorkers.load(std::memory_order_acquire) != 0) {
        QThread::msleep(1);
    }
}

void SyncService::dispatch(std::function<void(quint64)> worker) {
    // R48: a new dispatch supersedes the active run; the epoch bump makes the
    // old worker abandon its remaining phases and drop pending results.
    const quint64 epoch = m_epoch.fetch_add(1, std::memory_order_acq_rel) + 1;
    m_activeWorkers.fetch_add(1, std::memory_order_relaxed);
    m_activeFuture = QtConcurrent::run([this, epoch, worker = std::move(worker)]() {
        // A stale worker exits quickly under the lock without backend work.
        worker(epoch);
        // Decrementing the counter is the worker's last member access; the
        // destructor waits for the counter to reach zero before destroying
        // the m_* members.
        m_activeWorkers.fetch_sub(1, std::memory_order_release);
    });
}

void SyncService::postStep(quint64 epoch, std::function<void()> step) {
    // Result delivery happens on the service's (GUI) thread. The epoch check
    // is executed there, single-threaded between stop()/dispatch bumps and
    // deliveries, so a superseded result can no longer slip through a
    // check/emit race on the worker side. Queued invocations targeted at a
    // destroyed service are removed by the event machinery, which also makes
    // destruction safe while results are still queued.
    QMetaObject::invokeMethod(
        this,
        [this, epoch, step = std::move(step)]() {
            if (m_epoch.load(std::memory_order_acquire) != epoch) {
                return;
            }
            step();
        },
        Qt::QueuedConnection);
}

void SyncService::syncAccount(const Account &account) {
    // Brief step 68: the secret is read on the service's (GUI) thread. A sync
    // whose credential is missing never starts the worker and never emits
    // syncStarted/syncProgress; it directly finishes with Auth.
    const std::optional<QString> secret = m_credentials->read(account.credentialRef);
    if (!secret.has_value()) {
        m_epoch.fetch_add(1, std::memory_order_acq_rel);
        emit syncFinished(account.id, missingCredentialError());
        return;
    }

    dispatch([this, account, secret = *secret](quint64 epoch) { runSync(account, secret, epoch); });
}

void SyncService::fetchBody(const Account &account, const MessageHeader &header) {
    dispatch([this, account, header](quint64 epoch) { runBodyFetch(account, header, epoch); });
}

void SyncService::stop() {
    m_epoch.fetch_add(1, std::memory_order_release);
}

void SyncService::runSync(const Account &account, const QString &secret, quint64 epoch) {
    const std::scoped_lock lock(m_operationMutex);
    const auto isStale = [this, epoch]() {
        return m_epoch.load(std::memory_order_acquire) != epoch;
    };

    if (isStale()) {
        return;
    }

    const auto accountId = account.id;
    postStep(epoch, [this, accountId]() { emit syncStarted(accountId); });

    // Folders stage in progress (0 of 1 remaining stages done).
    postStep(epoch, [this, accountId]() { emit syncProgress(accountId, 0, 1); });
    if (isStale()) {
        return;
    }

    std::unique_ptr<MailBackend> backend = m_factory->create(account);
    const MailError connectError = backend->connect(account, secret);
    // R51 修订版：后端错误原样透传（不改码），仅缺凭据映射为 Auth。
    if (connectError) {
        postStep(epoch, [this, accountId, error = connectError]() {
            emit syncFinished(accountId, error);
        });
        return;
    }
    if (isStale()) {
        return;
    }

    const Result<QList<Folder>> folders = backend->listFolders();
    // R51 修订版：后端错误原样透传（不改码），仅缺凭据映射为 Auth。
    if (!folders.ok()) {
        postStep(epoch, [this, accountId, error = folders.error]() {
            emit syncFinished(accountId, error);
        });
        return;
    }
    if (isStale()) {
        return;
    }
    postStep(epoch, [this, accountId, folders = *folders.value]() {
        emit foldersFetched(accountId, folders);
    });

    const Folder *inbox = findInbox(*folders.value);
    if (inbox == nullptr) {
        postStep(epoch, [this, accountId]() {
            emit syncFinished(accountId,
                              MailError{ErrorCode::Protocol, QString::fromUtf8("No inbox folder"),
                                        QString::fromUtf8("no inbox")});
        });
        return;
    }

    const Result<QList<MessageHeader>> headers =
        backend->fetchHeaders(*inbox, kFirstUid, kHeaderFetchLimit);
    // R51 修订版：headers 阶段的失败只发 syncFinished；错误原样透传。
    if (!headers.ok()) {
        postStep(epoch, [this, accountId, error = headers.error]() {
            emit syncFinished(accountId, error);
        });
        return;
    }
    if (isStale()) {
        return;
    }
    postStep(epoch, [this, accountId, fullName = inbox->fullName, headers = *headers.value]() {
        emit headersFetched(accountId, fullName, headers);
    });

    // Headers stage complete (1 of 1 stages done).
    postStep(epoch, [this, accountId]() { emit syncProgress(accountId, 1, 1); });
    postStep(epoch, [this, accountId]() { emit syncFinished(accountId, MailError{}); });
}

void SyncService::runBodyFetch(const Account &account, const MessageHeader &header, quint64 epoch) {
    const std::scoped_lock lock(m_operationMutex);
    const auto isStale = [this, epoch]() {
        return m_epoch.load(std::memory_order_acquire) != epoch;
    };

    if (isStale()) {
        return;
    }

    // Brief step 70: fetchBody keeps the same background pattern and reads the
    // secret inside the worker; m_operationMutex serializes this store access
    // with other service operations.
    const std::optional<QString> secret = m_credentials->read(account.credentialRef);
    if (!secret.has_value()) {
        postStep(epoch, [this, messageId = header.id]() {
            emit bodyFetched(messageId, MessageBody{}, missingCredentialError());
        });
        return;
    }
    if (isStale()) {
        return;
    }

    std::unique_ptr<MailBackend> backend = m_factory->create(account);
    const MailError connectError = backend->connect(account, *secret);
    // R51 修订版：后端错误原样透传（不改码），仅缺凭据映射为 Auth。
    if (connectError) {
        postStep(epoch, [this, messageId = header.id, error = connectError]() {
            emit bodyFetched(messageId, MessageBody{}, error);
        });
        return;
    }
    if (isStale()) {
        return;
    }

    const Result<MessageBody> body = backend->fetchBody(header);
    // R51 修订版：后端错误原样透传（不改码），仅缺凭据映射为 Auth。
    if (!body.ok()) {
        postStep(epoch, [this, messageId = header.id, error = body.error]() {
            emit bodyFetched(messageId, MessageBody{}, error);
        });
        return;
    }

    // R51: the delivered body always references the requested header id.
    MessageBody resolved = *body.value;
    resolved.messageId = header.id;
    postStep(epoch, [this, messageId = header.id, body = resolved]() {
        emit bodyFetched(messageId, body, MailError{});
    });
}

} // namespace ningmail
