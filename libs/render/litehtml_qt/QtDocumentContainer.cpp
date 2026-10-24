#include "QtDocumentContainer.h"

#include <QByteArray>
#include <QColor>
#include <QPen>
#include <QPointF>
#include <QStringList>

namespace ningmail {
namespace {

// Same 64 KB gate as the sanitizer (R69): data: URIs are the only inline
// payload allowed into QImage::fromData.
constexpr qint64 kMaxInlineDataBytes = 64 * 1024;

QString utf8(const char *text) {
    return QString::fromUtf8(text != nullptr ? text : "");
}

QPainter *painterOf(litehtml::uint_ptr hdc) {
    return reinterpret_cast<QPainter *>(hdc);
}

/// Saves/restores the painter around a draw call and applies the current
/// litehtml clip when one is active (set_clip/del_clip stack, radii ignored in
/// M1).
class ScopedClip {
  public:
    ScopedClip(QPainter &painter, const QRectF &clipRect) : m_painter(painter) {
        m_painter.save();
        if (clipRect.isValid()) {
            m_painter.setClipRect(clipRect, Qt::ReplaceClip);
        }
    }
    ~ScopedClip() {
        m_painter.restore();
    }
    ScopedClip(const ScopedClip &) = delete;
    ScopedClip &operator=(const ScopedClip &) = delete;

  private:
    QPainter &m_painter;
};

} // namespace

QtDocumentContainer::QtDocumentContainer(int viewportWidth, qreal devicePixelRatio)
    : m_viewportWidth(viewportWidth > 0 ? viewportWidth : 600),
      m_devicePixelRatio(devicePixelRatio > 0 ? devicePixelRatio : 1.0) {
}

// ---------------------------------------------------------------------------
// Fonts
// ---------------------------------------------------------------------------

litehtml::uint_ptr QtDocumentContainer::create_font(const litehtml::font_description &descr,
                                                    const litehtml::document * /*doc*/,
                                                    litehtml::font_metrics *fm) {
    auto *handle = new FontHandle;
    handle->font = QFont(QString::fromStdString(descr.family));
    // CJK fallback and platform default resolution stay with Qt (R70): a
    // request for "sans-serif" or an unknown family resolves to the platform
    // default font, which carries the user's CJK fonts.
    if (descr.weight >= 100 && descr.weight <= 1000) {
        handle->font.setWeight(static_cast<QFont::Weight>(descr.weight));
    }
    if (descr.style == litehtml::font_style_italic) {
        handle->font.setStyle(QFont::StyleItalic);
    } else if (descr.style == litehtml::font_style_normal) {
        handle->font.setStyle(QFont::StyleNormal);
    }
    handle->font.setUnderline((descr.decoration_line & litehtml::text_decoration_line_underline) !=
                              0);
    handle->font.setStrikeOut(
        (descr.decoration_line & litehtml::text_decoration_line_line_through) != 0);
    handle->font.setPixelSize(descr.size > 0 ? qRound(descr.size) : 16);
    // One QFontMetricsF per handle so measure (text_width) and draw (draw_text)
    // agree exactly; litehtml's font_metrics is derived from it.
    handle->metrics = std::make_unique<QFontMetricsF>(handle->font);

    if (fm != nullptr) {
        const QFontMetricsF &metrics = *handle->metrics;
        fm->font_size = descr.size;
        fm->height = metrics.height();
        fm->ascent = metrics.ascent();
        fm->descent = metrics.descent();
        fm->x_height = metrics.xHeight();
        fm->ch_width = metrics.horizontalAdvance(QStringLiteral("0"));
        fm->draw_spaces = true;
        fm->sub_shift = 0;
        fm->super_shift = 0;
    }
    return reinterpret_cast<litehtml::uint_ptr>(handle);
}

void QtDocumentContainer::delete_font(litehtml::uint_ptr hFont) {
    delete reinterpret_cast<FontHandle *>(hFont);
}

litehtml::pixel_t QtDocumentContainer::text_width(const char *text, litehtml::uint_ptr hFont) {
    const auto *handle = reinterpret_cast<const FontHandle *>(hFont);
    return handle->metrics->horizontalAdvance(utf8(text));
}

void QtDocumentContainer::draw_text(litehtml::uint_ptr hdc, const char *text,
                                    litehtml::uint_ptr hFont, litehtml::web_color color,
                                    const litehtml::position &pos) {
    QPainter *painter = painterOf(hdc);
    ScopedClip clip(*painter, clipRect());
    const auto *handle = reinterpret_cast<const FontHandle *>(hFont);
    painter->setFont(handle->font);
    painter->setPen(QPen(toQColor(color)));
    // litehtml positions give the top of the text box (its reference test
    // container draws with text_baseline = top), so the Qt baseline point sits
    // one ascent down.
    painter->drawText(QPointF(pos.x, pos.y + handle->metrics->ascent()), utf8(text));
}

litehtml::pixel_t QtDocumentContainer::pt_to_px(float pt) const {
    return pt * 96.0f / 72.0f;
}

litehtml::pixel_t QtDocumentContainer::get_default_font_size() const {
    return 16;
}

const char *QtDocumentContainer::get_default_font_name() const {
    return "sans-serif";
}

// ---------------------------------------------------------------------------
// Backgrounds, borders, lists
// ---------------------------------------------------------------------------

void QtDocumentContainer::draw_solid_fill(litehtml::uint_ptr hdc,
                                          const litehtml::background_layer &layer,
                                          const litehtml::web_color &color) {
    QPainter *painter = painterOf(hdc);
    ScopedClip clip(*painter, clipRect());
    painter->fillRect(QRectF(layer.border_box.x, layer.border_box.y, layer.border_box.width,
                             layer.border_box.height),
                      toQColor(color));
}

void QtDocumentContainer::draw_borders(litehtml::uint_ptr hdc, const litehtml::borders &borders,
                                       const litehtml::position &pos, bool /*root*/) {
    QPainter *painter = painterOf(hdc);
    ScopedClip clip(*painter, clipRect());
    const QRectF box{static_cast<double>(pos.x), static_cast<double>(pos.y),
                     static_cast<double>(pos.width), static_cast<double>(pos.height)};

    // M1 approximation: every border style paints as a solid line; only a
    // zero-width side (including border_style_none) is skipped. Dotted/dashed
    // borders stay solid in M1.
    painter->setRenderHint(QPainter::Antialiasing, false);
    if (borders.top.width > 0) {
        painter->setPen(QPen(toQColor(borders.top.color), borders.top.width));
        painter->drawLine(QPointF(box.left(), box.top() + borders.top.width / 2.0),
                          QPointF(box.right(), box.top() + borders.top.width / 2.0));
    }
    if (borders.bottom.width > 0) {
        painter->setPen(QPen(toQColor(borders.bottom.color), borders.bottom.width));
        painter->drawLine(QPointF(box.left(), box.bottom() - borders.bottom.width / 2.0),
                          QPointF(box.right(), box.bottom() - borders.bottom.width / 2.0));
    }
    if (borders.left.width > 0) {
        painter->setPen(QPen(toQColor(borders.left.color), borders.left.width));
        painter->drawLine(QPointF(box.left() + borders.left.width / 2.0, box.top()),
                          QPointF(box.left() + borders.left.width / 2.0, box.bottom()));
    }
    if (borders.right.width > 0) {
        painter->setPen(QPen(toQColor(borders.right.color), borders.right.width));
        painter->drawLine(QPointF(box.right() - borders.right.width / 2.0, box.top()),
                          QPointF(box.right() - borders.right.width / 2.0, box.bottom()));
    }
}

void QtDocumentContainer::draw_list_marker(litehtml::uint_ptr hdc,
                                           const litehtml::list_marker &marker) {
    QPainter *painter = painterOf(hdc);
    ScopedClip clip(*painter, clipRect());

    const auto *handle = reinterpret_cast<const FontHandle *>(marker.font);
    if (!marker.image.empty()) {
        const QImage img = imageFor(marker.image);
        if (!img.isNull()) {
            painter->drawImage(QPointF(marker.pos.x, marker.pos.y + handle->metrics->ascent()),
                               img);
        }
        return;
    }

    const QPointF center(marker.pos.x + marker.pos.width / 2.0,
                         marker.pos.y + marker.pos.height / 2.0);
    switch (marker.marker_type) {
    case litehtml::list_style_type_disc:
        painter->setPen(Qt::NoPen);
        painter->setBrush(toQColor(marker.color));
        painter->drawEllipse(center, marker.pos.width / 2.0, marker.pos.height / 2.0);
        break;
    case litehtml::list_style_type_circle:
        painter->setPen(QPen(toQColor(marker.color), 1.0));
        painter->setBrush(Qt::NoBrush);
        painter->drawEllipse(center, marker.pos.width / 2.0, marker.pos.height / 2.0);
        break;
    case litehtml::list_style_type_square:
        painter->fillRect(QRectF(marker.pos.x, marker.pos.y, marker.pos.width, marker.pos.height),
                          toQColor(marker.color));
        break;
    default:
        // Ordered markers: draw the decimal index at the marker box (the same
        // top-of-box baseline rule as draw_text).
        if (handle != nullptr && marker.index > 0) {
            painter->setFont(handle->font);
            painter->setPen(QPen(toQColor(marker.color)));
            painter->drawText(QPointF(marker.pos.x, marker.pos.y + handle->metrics->ascent()),
                              QString::number(marker.index));
        }
        break;
    }
}

void QtDocumentContainer::draw_linear_gradient(
    litehtml::uint_ptr /*hdc*/, const litehtml::background_layer & /*layer*/,
    const litehtml::background_layer::linear_gradient & /*gradient*/) {
    // M1: gradients are not rendered (see header note).
}

void QtDocumentContainer::draw_radial_gradient(
    litehtml::uint_ptr /*hdc*/, const litehtml::background_layer & /*layer*/,
    const litehtml::background_layer::radial_gradient & /*gradient*/) {
    // M1: gradients are not rendered (see header note).
}

void QtDocumentContainer::draw_conic_gradient(
    litehtml::uint_ptr /*hdc*/, const litehtml::background_layer & /*layer*/,
    const litehtml::background_layer::conic_gradient & /*gradient*/) {
    // M1: gradients are not rendered (see header note).
}

// ---------------------------------------------------------------------------
// Images
// ---------------------------------------------------------------------------

QImage QtDocumentContainer::imageFor(const std::string &url) {
    return m_images.value(QByteArray::fromStdString(url));
}

void QtDocumentContainer::load_image(const char *src, const char * /*baseurl*/,
                                     bool /*redraw_on_ready*/) {
    // M1: no network loads (controller ruling). Only cid: and data: URIs are
    // honoured; everything else stays an absent image.
    const QString source = utf8(src);
    const QByteArray key = source.toUtf8();
    if (m_images.contains(key)) {
        return;
    }
    if (source.startsWith(QLatin1String("cid:"))) {
        // Inline attachments are not fetched in M1; an empty image collapses
        // the element size instead of showing a broken image box.
        m_images.insert(key, QImage());
        return;
    }
    if (source.startsWith(QLatin1String("data:"))) {
        const int comma = source.indexOf(QLatin1Char(','));
        if (comma < 0) {
            m_images.insert(key, QImage());
            return;
        }
        const QByteArray payload = source.mid(comma + 1).toLatin1();
        if (payload.size() * 3 / 4 > kMaxInlineDataBytes) {
            // Base64 expands by 4/3; this mirrors the 64 KB decoded gate.
            m_images.insert(key, QImage());
            return;
        }
        m_images.insert(key, QImage::fromData(QByteArray::fromBase64(payload)));
        return;
    }
    m_images.insert(key, QImage()); // http(s):/relative → intentionally absent
}

void QtDocumentContainer::get_image_size(const char *src, const char * /*baseurl*/,
                                         litehtml::size &sz) {
    const QByteArray key = utf8(src).toUtf8();
    if (!m_images.contains(key)) {
        load_image(src, "", false);
    }
    const QImage img = m_images.value(key);
    sz.width = img.width();
    sz.height = img.height();
}

void QtDocumentContainer::draw_image(litehtml::uint_ptr hdc,
                                     const litehtml::background_layer &layer,
                                     const std::string &url, const std::string & /*base_url*/) {
    const QImage img = imageFor(url);
    if (img.isNull()) {
        return;
    }
    QPainter *painter = painterOf(hdc);
    ScopedClip clip(*painter, clipRect());
    // Honour the standard CSS repeat modes across the clip box; the M1 simple
    // path draws once at the origin box.
    const QRectF clipBox(layer.clip_box.x, layer.clip_box.y, layer.clip_box.width,
                         layer.clip_box.height);
    const qreal tileWidth = img.width();
    const qreal tileHeight = img.height();
    qreal x = layer.origin_box.x;
    qreal y = layer.origin_box.y;
    switch (layer.repeat) {
    case litehtml::background_repeat_repeat_x:
        while (x > clipBox.left()) {
            x -= tileWidth;
        }
        for (; x < clipBox.right(); x += tileWidth) {
            painter->drawImage(QPointF(x, y), img);
        }
        break;
    case litehtml::background_repeat_repeat_y:
        while (y > clipBox.top()) {
            y -= tileHeight;
        }
        for (; y < clipBox.bottom(); y += tileHeight) {
            painter->drawImage(QPointF(x, y), img);
        }
        break;
    case litehtml::background_repeat_repeat:
        while (x > clipBox.left()) {
            x -= tileWidth;
        }
        while (y > clipBox.top()) {
            y -= tileHeight;
        }
        for (; x < clipBox.right(); x += tileWidth) {
            for (qreal tileY = y; tileY < clipBox.bottom(); tileY += tileHeight) {
                painter->drawImage(QPointF(x, tileY), img);
            }
        }
        break;
    default: // background_repeat_no_repeat
        painter->drawImage(QPointF(x, y), img);
        break;
    }
}

// ---------------------------------------------------------------------------
// Document-facing callbacks
// ---------------------------------------------------------------------------

void QtDocumentContainer::set_caption(const char * /*caption*/) {
    // M1: no window title reflection.
}

void QtDocumentContainer::set_base_url(const char * /*base_url*/) {
    // M1: relative URLs are never resolved (network image loads are off).
}

void QtDocumentContainer::link(const std::shared_ptr<litehtml::document> & /*doc*/,
                               const litehtml::element::ptr & /*el*/) {
    // M1: no link bookkeeping (no click handling).
}

void QtDocumentContainer::on_anchor_click(const char * /*url*/,
                                          const litehtml::element::ptr & /*el*/) {
    // M1: anchor clicks are inert.
}

void QtDocumentContainer::on_mouse_event(const litehtml::element::ptr & /*el*/,
                                         litehtml::mouse_event /*event*/) {
    // M1: no hover tracking; rendering is offscreen and stateless.
}

void QtDocumentContainer::set_cursor(const char * /*cursor*/) {
    // M1: nothing interactive to place a cursor on.
}

void QtDocumentContainer::transform_text(std::string &text, litehtml::text_transform tt) {
    // M1 simple implementation: uppercase/lowercase via QString
    // (locale-independent default conversion); capitalize uppercases the first
    // letter of the run. CJK text has no visible casing change, which matches
    // the expected behaviour.
    QString qtText = QString::fromUtf8(text.c_str());
    switch (tt) {
    case litehtml::text_transform_uppercase:
        qtText = qtText.toUpper();
        break;
    case litehtml::text_transform_lowercase:
        qtText = qtText.toLower();
        break;
    case litehtml::text_transform_capitalize:
        qtText = qtText.toLower();
        if (!qtText.isEmpty()) {
            qtText[0] = qtText.at(0).toUpper();
        }
        break;
    case litehtml::text_transform_none:
        break;
    }
    text = qtText.toUtf8().toStdString();
}

void QtDocumentContainer::import_css(std::string &text, const std::string & /*url*/,
                                     std::string & /*baseurl*/) {
    // M1: external stylesheets are not fetched; only <style> blocks already in
    // the (sanitized) document are used.
    text.clear();
}

void QtDocumentContainer::set_clip(const litehtml::position &pos,
                                   const litehtml::border_radiuses & /*bdr_radius*/) {
    // litehtml clip protocol; border radii are ignored in M1.
    m_clipStack.append(QRectF(pos.x, pos.y, pos.width, pos.height));
}

void QtDocumentContainer::del_clip() {
    if (!m_clipStack.isEmpty()) {
        m_clipStack.removeLast();
    }
}

void QtDocumentContainer::get_viewport(litehtml::position &viewport) const {
    // The plan's get_client_rect == the v0.10 get_viewport virtual (utf8 API).
    // Height is the agreed render cap so nothing depends on unknown content
    // height at container-creation time.
    viewport = litehtml::position(0, 0, static_cast<litehtml::pixel_t>(m_viewportWidth),
                                  static_cast<litehtml::pixel_t>(kMaxRenderHeight));
}

litehtml::element::ptr
QtDocumentContainer::create_element(const char * /*tag_name*/,
                                    const litehtml::string_map & /*attributes*/,
                                    const std::shared_ptr<litehtml::document> & /*doc*/) {
    // All M1 tags are handled by litehtml's builtin element set; the sanitizer
    // already unwrapped unknown tags. nullptr Falls back to the documented
    // default element creation.
    return nullptr;
}

void QtDocumentContainer::get_media_features(litehtml::media_features &media) const {
    litehtml::position viewport;
    get_viewport(viewport);
    media.type = litehtml::media_type_screen;
    media.width = viewport.width;
    media.height = viewport.height;
    media.device_width = viewport.width * m_devicePixelRatio;
    media.device_height = viewport.height * m_devicePixelRatio;
    media.color = 8;
    media.color_index = 24;
    media.monochrome = 0;
    media.resolution = 96.0f * static_cast<float>(m_devicePixelRatio);
}

void QtDocumentContainer::get_language(std::string &language, std::string &culture) const {
    // Deterministic defaults; M1 email bodies do not depend on the UI locale.
    language = "en";
    culture = "en-US";
}

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

QColor QtDocumentContainer::toQColor(litehtml::web_color color) {
    return QColor(color.red, color.green, color.blue, color.alpha);
}

QRectF QtDocumentContainer::clipRect() const {
    if (m_clipStack.isEmpty()) {
        return QRectF();
    }
    // The innermost clip wins: litehtml pushes nested boxes and the reference
    // containers clip to the current (top of stack) box.
    return m_clipStack.last();
}

} // namespace ningmail
