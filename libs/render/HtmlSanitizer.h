#pragma once

#include <QString>
#include <QStringList>

namespace ningmail {

struct SanitizedHtml {
    QString html;
    QStringList warnings;
};

// Filters untrusted email HTML before rendering (task 10a). Parses the source HTML with Gumbo and
// rebuilds the tree through an allowlist: remove-list subtrees are dropped entirely, unknown tags
// are unwrapped, and text/attribute values are escaped on output.
class HtmlSanitizer {
  public:
    static SanitizedHtml sanitize(const QString &html);
};

} // namespace ningmail
