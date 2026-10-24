#include "Charset.h"

#include <QString>

#include <unicode/ucnv.h>

#include <optional>

namespace ningmail {
namespace {

// Per task spec: gb2312/gbk/gb18030 -> GB18030, utf8 -> UTF-8, latin1 ->
// ISO-8859-1, empty -> UTF-8; everything else is passed to ICU in upper case.
QString normalizeName(const QString &charset) {
    const QString name = charset.trimmed().toLower();
    if (name.isEmpty() || name == QLatin1String("utf8") || name == QLatin1String("utf-8"))
        return QLatin1String("UTF-8");
    if (name == QLatin1String("gb2312") || name == QLatin1String("gbk") ||
        name == QLatin1String("gb_2312") || name == QLatin1String("gb18030"))
        return QLatin1String("GB18030");
    if (name == QLatin1String("latin1") || name == QLatin1String("latin-1"))
        return QLatin1String("ISO-8859-1");
    return name.toUpper();
}

std::optional<QString> decodeWithIcu(const QByteArray &data, const QString &name) {
    const QByteArray converterName = name.toUtf8();
    UErrorCode status = U_ZERO_ERROR;
    UConverter *converter = ucnv_open(converterName.constData(), &status);
    if (U_FAILURE(status) || converter == nullptr)
        return std::nullopt;

    // Preflight to get the required UChar count.
    status = U_ZERO_ERROR;
    const int32_t needed =
        ucnv_toUChars(converter, nullptr, 0, data.constData(), data.size(), &status);
    if (status != U_BUFFER_OVERFLOW_ERROR || needed <= 0) {
        ucnv_close(converter);
        return needed == 0 ? std::optional<QString>(QString()) : std::nullopt;
    }

    QString result(static_cast<int>(needed), QChar(u'\0'));
    status = U_ZERO_ERROR;
    ucnv_toUChars(converter, reinterpret_cast<UChar *>(result.data()), needed, data.constData(),
                  data.size(), &status);
    ucnv_close(converter);
    if (U_FAILURE(status))
        return std::nullopt;
    return result;
}

// Drops NUL characters and a leading BOM, keeps line breaks intact.
QString cleanText(QString text) {
    text.remove(QChar(u'\0'));
    if (text.startsWith(QChar(u'\uFEFF')))
        text.remove(0, 1);
    return text;
}

} // namespace

QString Charset::decode(const QByteArray &data, const QString &charset) {
    if (data.isEmpty())
        return QString();

    if (auto decoded = decodeWithIcu(data, normalizeName(charset)))
        return cleanText(*decoded);

    // Fallback chain: UTF-8, then Latin-1.
    const QString asUtf8 = QString::fromUtf8(data);
    // U+FFFD only counts as a failure marker when the input does not literally
    // contain its own encoding (EF BF BD); legitimate U+FFFD content must not
    // be demoted to Latin-1.
    if (!asUtf8.contains(QChar(u'\uFFFD')) || data.contains(QByteArray("\xEF\xBF\xBD")))
        return cleanText(asUtf8);
    return cleanText(QString::fromLatin1(data));
}

} // namespace ningmail
