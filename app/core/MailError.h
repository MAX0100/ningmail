#pragma once

#include <QString>
#include <QtGlobal>

namespace ningmail {

enum class ErrorCode { None, Auth, Network, Tls, Protocol, Storage, Quota, Cancelled, Unknown };

struct MailError {
    ErrorCode code = ErrorCode::None;
    QString message;
    QString detail;

    explicit operator bool() const {
        return code != ErrorCode::None;
    }
};

} // namespace ningmail
