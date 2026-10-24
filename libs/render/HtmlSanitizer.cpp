#include "HtmlSanitizer.h"

#include <gumbo.h>

#include <QByteArray>
#include <QLatin1Char>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

namespace ningmail {
namespace {

constexpr int kMaxDataImageBytes = 64 * 1024;

constexpr const char *kWarningRemoteImage = "remote-image-blocked";
constexpr const char *kWarningDataImageTooLarge = "data-image-too-large";
constexpr const char *kWarningDataImageInvalid =
    "data-image-invalid"; // Added in fix round 1 (R69).
constexpr const char *kWarningUnsafeUrl = "unsafe-url-blocked";

bool isAllowedTag(const QString &name) {
    static const QStringList tags = {
        "html",   "head",  "style", "body",  "div",        "span", "p",    "br",     "hr",
        "table",  "thead", "tbody", "tfoot", "tr",         "td",   "th",   "ul",     "ol",
        "li",     "dl",    "dt",    "dd",    "blockquote", "pre",  "code", "a",      "img",
        "strong", "b",     "em",    "i",     "u",          "s",    "font", "center", "h1",
        "h2",     "h3",    "h4",    "h5",    "h6",         "sup",  "sub"};
    return tags.contains(name);
}

bool isRemovedTag(const QString &name) {
    static const QStringList tags = {"script", "iframe", "frame",    "object", "embed", "form",
                                     "input",  "button", "textarea", "select", "meta",  "link",
                                     "base",   "video",  "audio",    "canvas", "svg"};
    return tags.contains(name);
}

bool isVoidTag(const QString &name) {
    return name == "br" || name == "hr" || name == "img";
}

bool isAllowedAttribute(const QString &name) {
    static const QStringList names = {"href",    "src",     "alt",   "title",  "width", "height",
                                      "colspan", "rowspan", "align", "valign", "style", "class",
                                      "id",      "bgcolor", "color", "face",   "size"};
    return names.contains(name);
}

// Pattern cleaning for <style> element content and inline style attribute values; no warning is
// recorded for these removals (R65). Fix round 1 (R68): strip /*...*/ comments first (exposing
// comment-obfuscated keywords such as @/*x*/import), collapse whitespace runs so whitespace-split
// variants such as "position: fixed" match, then remove the forbidden constructs. A pure
// whitespace split like "@im port" is not valid CSS and never executes, so it is not required
// (and not attempted) here.
QString cleanCss(const QString &css) {
    QString result = css;
    static const QRegularExpression comments(QStringLiteral("/\\*.*?\\*/"),
                                             QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression whitespaceRuns(QStringLiteral("\\s+"));
    static const QRegularExpression forbidden(
        QStringLiteral("@import|url\\s*\\(|expression\\s*\\(|behavior\\s*:|position\\s*:\\s*fixed"),
        QRegularExpression::CaseInsensitiveOption);
    result.replace(comments, QString());
    result.replace(whitespaceRuns, QStringLiteral(" "));
    result.replace(forbidden, QString());
    return result;
}

QString escapeText(const QString &text) {
    QString result = text;
    result.replace('&', "&amp;").replace('<', "&lt;").replace('>', "&gt;");
    return result;
}

QString escapeAttributeValue(const QString &value) {
    QString result = value;
    result.replace('&', "&amp;").replace('<', "&lt;").replace('>', "&gt;").replace('"', "&quot;");
    return result;
}

// Lower-case URI scheme of the value; empty for relative or malformed URLs. Only the ASCII scheme
// forms count, so e.g. relative paths never produce a scheme.
QString urlScheme(const QString &url) {
    const int count = url.size();
    for (int i = 0; i < count; ++i) {
        const QChar c = url.at(i);
        if (c == ':')
            return url.left(i).toLower();
        const bool schemeChar = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
        if (!schemeChar)
            return QString();
    }
    return QString();
}

// Verdict of the data: URI gate on img src (fix round 1, R69): a payload only passes when the
// mediatype is image/* (case-insensitive), the encoding is base64, the payload is non-empty and
// decodable, and the decoded size is at most 64KB. Oversize decodes report too-large; everything
// else (non-image/*, empty payload, syntax or decode failure) reports invalid. 64KB means 65536
// decoded bytes.
enum class DataImageCheck { Valid, TooLarge, Invalid };

DataImageCheck dataImageCheck(const QString &value) {
    const int comma = value.indexOf(',');
    if (comma < 0)
        return DataImageCheck::Invalid;
    const QString mediatype =
        value.mid(5, comma - 5).toLower(); // Only part past the "data:" prefix.
    if (!mediatype.startsWith("image/"))
        return DataImageCheck::Invalid;
    // Fix round 2: parameter tokens are trimmed before the lookup, so forms like
    // "image/png; base64,..." are accepted exactly like "image/png;base64,...".
    const int parameterStart = mediatype.indexOf(';');
    const QStringList parameters = mediatype.mid(parameterStart + 1).split(';');
    bool hasBase64Parameter = false;
    for (const QString &parameter : parameters) {
        if (parameter.trimmed() == QLatin1String("base64")) {
            hasBase64Parameter = true;
            break;
        }
    }
    if (parameterStart < 0 || !hasBase64Parameter)
        return DataImageCheck::Invalid;
    QString payload = value.mid(comma + 1);
    const int padding = payload.indexOf('=');
    if (padding >= 0)
        payload = payload.left(padding);
    int dataChars = 0;
    for (const QChar c : payload) {
        if (c.isSpace())
            continue;
        const bool inAlphabet = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') || c == '+' || c == '/';
        if (!inAlphabet)
            return DataImageCheck::Invalid;
        ++dataChars;
    }
    if (dataChars == 0)
        return DataImageCheck::Invalid; // Empty payload (removing whitespace leaves nothing).
    const int remainder = dataChars % 4;
    if (remainder == 1)
        return DataImageCheck::Invalid;
    const qint64 decodedBytes = (static_cast<qint64>(dataChars) / 4) * 3 +
                                (remainder == 2 ? 1 : 0) + (remainder == 3 ? 2 : 0);
    return decodedBytes <= kMaxDataImageBytes ? DataImageCheck::Valid : DataImageCheck::TooLarge;
}

// Lower-case element name. Gumbo_tag_unknown names come from the original source slice between '<'
// and the first attribute/tag terminator.
QString elementTagName(const GumboElement &element) {
    if (element.tag != GUMBO_TAG_UNKNOWN) {
        return QString::fromLatin1(gumbo_normalized_tagname(element.tag)).toLower();
    }
    const char *data = element.original_tag.data;
    int length = static_cast<int>(element.original_tag.length);
    if (length > 0 && data[0] == '<') {
        ++data;
        --length;
    }
    int end = 0;
    while (end < length) {
        const char c = data[end];
        if (c == '>' || c == '/' || c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' ||
            c == '\f')
            break;
        ++end;
    }
    return QString::fromUtf8(data, end).toLower();
}

void rebuildChildren(const GumboVector &children, bool insideStyleText, QString &out,
                     QStringList &warnings);

// Serializes the allowed attributes. Returns whether the caller must append the blocked-image flag.
void serializeAttributes(const GumboElement &element, const QString &tagName, QString &out,
                         QStringList &warnings) {
    bool blockRemoteImage = false;
    const GumboVector &attributes = element.attributes;
    for (unsigned int i = 0; i < attributes.length; ++i) {
        const auto *attribute = static_cast<const GumboAttribute *>(attributes.data[i]);
        const QString name = QString::fromUtf8(attribute->name).toLower();
        if (!isAllowedAttribute(name))
            continue; // Drops on* handlers and every other unsupported attribute.
        if (attribute->value == nullptr)
            continue;
        QString value = QString::fromUtf8(attribute->value);
        if (name == "href") {
            const QString scheme = urlScheme(value);
            if (scheme != "http" && scheme != "https" && scheme != "mailto") {
                warnings.append(kWarningUnsafeUrl);
                continue;
            }
        } else if (name == "src") {
            const QString scheme = urlScheme(value);
            if (scheme == "http" || scheme == "https") {
                if (tagName == "img") {
                    blockRemoteImage = true; // "load remote images" lands here in M2.
                    warnings.append(kWarningRemoteImage);
                    continue;
                }
                warnings.append(kWarningUnsafeUrl);
                continue;
            }
            if (scheme == "data") {
                if (tagName != "img") {
                    warnings.append(kWarningDataImageInvalid);
                    continue;
                }
                switch (dataImageCheck(value)) {
                case DataImageCheck::Valid:
                    break;
                case DataImageCheck::TooLarge:
                    warnings.append(kWarningDataImageTooLarge);
                    continue;
                case DataImageCheck::Invalid:
                    warnings.append(kWarningDataImageInvalid);
                    continue;
                }
            } else if (scheme != "mailto" && scheme != "cid") {
                warnings.append(kWarningUnsafeUrl);
                continue;
            }
        } else if (name == "style") {
            value = cleanCss(value);
        }
        out += QLatin1Char(' ') + name + "=\"" + escapeAttributeValue(value) + QLatin1Char('"');
    }
    if (blockRemoteImage)
        out += " data-ningmail-blocked=\"1\"";
}

void rebuildElement(const GumboNode *node, QString &out, QStringList &warnings) {
    const GumboElement &element = node->v.element;
    const QString name = elementTagName(element);
    if (isRemovedTag(name))
        return; // Remove-list: drop the whole subtree, including its text.
    if (!isAllowedTag(name)) {
        // Unknown tag (R64): unwrap, rebuilding the children inline.
        rebuildChildren(element.children, false, out, warnings);
        return;
    }
    out += QLatin1Char('<') + name;
    serializeAttributes(element, name, out, warnings);
    out += QLatin1Char('>');
    if (isVoidTag(name))
        return;
    rebuildChildren(element.children, name == "style", out, warnings);
    out += "</" + name + QLatin1Char('>');
}

void rebuildNode(const GumboNode *node, bool insideStyleText, QString &out, QStringList &warnings) {
    switch (node->type) {
    case GUMBO_NODE_DOCUMENT:
        rebuildChildren(node->v.document.children, false, out, warnings);
        return;
    case GUMBO_NODE_ELEMENT:
    case GUMBO_NODE_TEMPLATE:
        rebuildElement(node, out, warnings);
        return;
    case GUMBO_NODE_TEXT:
    case GUMBO_NODE_CDATA:
    case GUMBO_NODE_WHITESPACE: {
        const QString text = QString::fromUtf8(node->v.text.text);
        // CSS content is raw text in HTML output, so it must not be entity-escaped (entities are
        // not decoded inside <style>); escaping would corrupt legitimate CSS elsewhere.
        out += insideStyleText ? cleanCss(text) : escapeText(text);
        return;
    }
    default:
        return; // Comments and doctype nodes are dropped.
    }
}

void rebuildChildren(const GumboVector &children, bool insideStyleText, QString &out,
                     QStringList &warnings) {
    for (unsigned int i = 0; i < children.length; ++i) {
        const auto *node = static_cast<const GumboNode *>(children.data[i]);
        rebuildNode(node, insideStyleText, out, warnings);
    }
}

QStringList dedupeWarnings(const QStringList &warnings) {
    QStringList unique;
    for (const QString &warning : warnings) {
        if (!unique.contains(warning))
            unique.append(warning);
    }
    return unique;
}

} // namespace

SanitizedHtml HtmlSanitizer::sanitize(const QString &html) {
    SanitizedHtml result;
    const QByteArray utf8 = html.toUtf8();
    GumboOutput *parsed = gumbo_parse_with_options(&kGumboDefaultOptions, utf8.constData(),
                                                   static_cast<unsigned int>(utf8.size()));
    QString out;
    rebuildChildren(parsed->document->v.document.children, false, out, result.warnings);
    gumbo_destroy_output(&kGumboDefaultOptions, parsed);
    result.warnings = dedupeWarnings(result.warnings);
    result.html = out;
    return result;
}

} // namespace ningmail
