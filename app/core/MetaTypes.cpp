#include "Types.h"

#include <QMetaType>

namespace ningmail {

void registerMetaTypes() {
    qRegisterMetaType<QList<Folder>>();
    qRegisterMetaType<QList<MessageHeader>>();
    qRegisterMetaType<QList<AttachmentMeta>>();
    qRegisterMetaType<MessageBody>();
    qRegisterMetaType<MailError>();
}

} // namespace ningmail
