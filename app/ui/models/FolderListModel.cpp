#include "models/FolderListModel.h"

namespace ningmail {

FolderListModel::FolderListModel(FolderStore &store, QObject *parent)
    : QAbstractListModel{parent}, m_store{store} {
}

void FolderListModel::reload(AccountId accountId) {
    beginResetModel();
    m_folders.clear();
    const auto folders = m_store.list(accountId);
    if (folders.ok()) {
        m_folders = *folders.value;
    }
    endResetModel();
}

int FolderListModel::rowCount(const QModelIndex &parent) const {
    if (parent.isValid()) {
        return 0;
    }
    return m_folders.size();
}

QVariant FolderListModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_folders.size()) {
        return {};
    }
    const Folder &folder = m_folders.at(index.row());
    switch (role) {
    case IdRole:
        return folder.id;
    case AccountIdRole:
        return folder.accountId;
    case NameRole:
        return folder.fullName;
    case RoleRole:
        return static_cast<int>(folder.role);
    case UnreadRole:
        // M1 placeholder: this model is constructed with a FolderStore& only
        // and cannot read the messages table, so the unread count reports 0.
        return 0;
    default:
        return {};
    }
}

QHash<int, QByteArray> FolderListModel::roleNames() const {
    return {
        {IdRole, "id"},     {AccountIdRole, "accountId"}, {NameRole, "name"},
        {RoleRole, "role"}, {UnreadRole, "unread"},
    };
}

} // namespace ningmail
