#pragma once

#include "Store/MessageStore.h"
#include "Types.h"

#include <QAbstractListModel>
#include <QHash>
#include <QList>

namespace ningmail {

/// Read-only QML model of one folder's message headers, paged with keyset
/// pagination (newest first, PAGE_SIZE rows per page). Every call must happen
/// on the GUI thread; the model holds no database connection, only the
/// MessageStore reference it was constructed with.
class MessageListModel : public QAbstractListModel {
    Q_OBJECT
  public:
    enum Roles {
        IdRole = Qt::UserRole + 1,
        SubjectRole,
        FromRole,
        FromAddressRole,
        DateRole,
        UnreadRole,
        FlaggedRole,
        HasAttachmentsRole,
        SnippetRole
    };

    static constexpr int PAGE_SIZE = 100;

    explicit MessageListModel(MessageStore &store, QObject *parent = nullptr);

    /// Resets the model to `folderId` and loads the first page only.
    Q_INVOKABLE void setFolder(FolderId folderId);
    /// fetchMore({}): appends the next page incrementally.
    Q_INVOKABLE void loadMore();
    /// Resets the model and reloads the first page of the current folder.
    void reload();

    /// Q_INVOKABLE so the QML layer can guard loadMore() with the same
    /// check the view uses before requesting the next page.
    Q_INVOKABLE bool canFetchMore(const QModelIndex &parent = QModelIndex()) const override;
    void fetchMore(const QModelIndex &parent) override;
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

  private:
    MessageStore &m_store;
    FolderId m_folderId = 0;
    QList<MessageHeader> m_headers;
};

} // namespace ningmail
