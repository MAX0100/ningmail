#pragma once

#include <QByteArray>
#include <QString>

namespace ningmail {

// Decodes raw bytes to text using an explicit charset name.
// Unknown or failing charsets fall back to UTF-8, then Latin-1.
class Charset {
  public:
    static QString decode(const QByteArray &data, const QString &charset);
};

} // namespace ningmail
