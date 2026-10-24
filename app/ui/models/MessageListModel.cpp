#include "models/MessageListModel.h"

#include <optional>

namespace ningmail {

MessageListModel::MessageListModel(MessageStore &store, QObject *parent)
    : QAbstractListModel{parent}, m_store{store} {
}

void MessageListModel::setFolder(FolderId folderId) {
    m_folderId = folderId;
    reload();
}

void MessageListModel::reload() {
    beginResetModel();
    m_headers.clear();
    if (m_folderId > 0) {
        const auto page = m_store.list(m_folderId, PAGE_SIZE);
        if (page.ok()) {
            m_headers = *page.value;
        }
    }
    endResetModel();
}

void MessageListModel::loadMore() {
    fetchMore(QModelIndex());
}

bool MessageListModel::canFetchMore(const QModelIndex &parent) const {
    if (parent.isValid() || m_folderId <= 0) {
        return false;
    }
    return m_headers.size() < m_store.count(m_folderId);
}

void MessageListModel::fetchMore(const QModelIndex &parent) {
    if (parent.isValid() || m_folderId <= 0) {
        return;
    }
    std::optional<MessageCursor> cursor;
    if (!m_headers.isEmpty()) {
        // Keyset cursor: strictly older than the last loaded (sent_at, id).
        const MessageHeader &last = m_headers.last();
        cursor = MessageCursor{last.sentAt.toMSecsSinceEpoch(), last.id};
    }
    const auto page = m_store.list(m_folderId, PAGE_SIZE, cursor);
    if (!page.ok() || page.value->isEmpty()) {
        return;
    }
    const int count = page.value->size();
    beginInsertRows(QModelIndex(), m_headers.size(), m_headers.size() + count - 1);
    m_headers.append(*page.value);
    endInsertRows();
}

int MessageListModel::rowCount(const QModelIndex &parent) const {
    if (parent.isValid()) {
        return 0;
    }
    return m_headers.size();
}

QVariant MessageListModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_headers.size()) {
        return {};
    }
    const MessageHeader &header = m_headers.at(index.row());
    switch (role) {
    case IdRole:
        return header.id;
    case SubjectRole:
        return header.subject;
    case FromRole:
        return header.from.name.isEmpty() ? header.from.address : header.from.name;
    case FromAddressRole:
        return header.from.address;
    case DateRole:
        return QVariant(header.sentAt);
    case UnreadRole:
        return !header.flags.testFlag(MailFlag::Seen);
    case FlaggedRole:
        return header.flags.testFlag(MailFlag::Flagged);
    case HasAttachmentsRole:
        return header.hasAttachments;
    case SnippetRole:
        return header.snippet;
    default:
        return {};
    }
}

QHash<int, QByteArray> MessageListModel::roleNames() const {
    return {
        {IdRole, "id"},           {SubjectRole, "subject"},
        {FromRole, "from"},       {FromAddressRole, "fromAddress"},
        {DateRole, "date"},       {UnreadRole, "unread"},
        {FlaggedRole, "flagged"}, {HasAttachmentsRole, "hasAttachments"},
        {SnippetRole, "snippet"},
    };
}

} // namespace ningmail
