#pragma once

#include "Store/FolderStore.h"
#include "Types.h"

#include <QAbstractListModel>
#include <QHash>
#include <QList>

namespace ningmail {

/// Read-only QML model of one account's folders. Every call must happen on
/// the GUI thread; the model holds no database connection, only the
/// FolderStore reference it was constructed with.
class FolderListModel : public QAbstractListModel {
    Q_OBJECT
  public:
    enum Roles { IdRole = Qt::UserRole + 1, AccountIdRole, NameRole, RoleRole, UnreadRole };

    explicit FolderListModel(FolderStore &store, QObject *parent = nullptr);

    /// Resets the model to the folders of `accountId`.
    Q_INVOKABLE void reload(AccountId accountId);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

  private:
    FolderStore &m_store;
    QList<Folder> m_folders;
};

} // namespace ningmail
