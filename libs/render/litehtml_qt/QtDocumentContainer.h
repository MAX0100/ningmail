// QtDocumentContainer — litehtml document_container backend on QPainter.
//
// Adapted for NingMail (task 10b). The original container interfaces come from
// the litehtml project, BSD-3-Clause:
//   Copyright (c) 2013, Yuri Kobets (tordex), https://github.com/litehtml/litehtml
// litehtml v0.10 itself ships reference containers for cairo/windows/haiku but
// none for Qt, so this file was written against the v0.10
// include/litehtml/document_container.h interface (utf8 std::string API), using
// the project's test container as the behavioural reference.
//
// M1 restrictions (renderer contract): only cid:/data: images are loaded, no
// network fetching, no link interaction, gradients are not drawn.
#pragma once

#include <memory>

#include <QFont>
#include <QFontMetricsF>
#include <QHash>
#include <QImage>
#include <QPainter>
#include <QRectF>
#include <QVector>

#include <litehtml.h>

namespace ningmail {

/// Upper bound for one rendered message image (logical pixels, R73 ruling 7).
inline constexpr int kMaxRenderHeight = 20000;

/// Renders litehtml layout into a QPainter and supplies fonts, colors and
/// images on top of Qt. One instance is created per render pass on a worker
/// thread; all state is local to that pass, so repeated renders are
/// deterministic and free of shared mutable state.
class QtDocumentContainer : public litehtml::document_container {
  public:
    /// `viewportWidth`/`viewportHeight` are logical pixels; `devicePixelRatio`
    /// feeds get_media_features so CSS media queries see the real surface.
    QtDocumentContainer(int viewportWidth, qreal devicePixelRatio);

    // Fonts ------------------------------------------------------------------
    litehtml::uint_ptr create_font(const litehtml::font_description &descr,
                                   const litehtml::document *doc,
                                   litehtml::font_metrics *fm) override;
    void delete_font(litehtml::uint_ptr hFont) override;
    litehtml::pixel_t text_width(const char *text, litehtml::uint_ptr hFont) override;
    void draw_text(litehtml::uint_ptr hdc, const char *text, litehtml::uint_ptr hFont,
                   litehtml::web_color color, const litehtml::position &pos) override;
    litehtml::pixel_t pt_to_px(float pt) const override;
    litehtml::pixel_t get_default_font_size() const override;
    const char *get_default_font_name() const override;

    // Backgrounds, borders, lists ------------------------------------------------
    void draw_solid_fill(litehtml::uint_ptr hdc, const litehtml::background_layer &layer,
                         const litehtml::web_color &color) override;
    void draw_borders(litehtml::uint_ptr hdc, const litehtml::borders &borders,
                      const litehtml::position &draw_pos, bool root) override;
    void draw_list_marker(litehtml::uint_ptr hdc, const litehtml::list_marker &marker) override;
    // Gradients are not drawn in M1 (M1uma is expected to be handled by M2's stylesheet
    // pass); email bodies in M1 only use solid fills.
    void draw_linear_gradient(litehtml::uint_ptr hdc, const litehtml::background_layer &layer,
                              const litehtml::background_layer::linear_gradient &gradient) override;
    void draw_radial_gradient(litehtml::uint_ptr hdc, const litehtml::background_layer &layer,
                              const litehtml::background_layer::radial_gradient &gradient) override;
    void draw_conic_gradient(litehtml::uint_ptr hdc, const litehtml::background_layer &layer,
                             const litehtml::background_layer::conic_gradient &gradient) override;

    // Images -----------------------------------------------------------------
    void load_image(const char *src, const char *baseurl, bool redraw_on_ready) override;
    void get_image_size(const char *src, const char *baseurl, litehtml::size &sz) override;
    void draw_image(litehtml::uint_ptr hdc, const litehtml::background_layer &layer,
                    const std::string &url, const std::string &base_url) override;

    // Document-facing callbacks ---------------------------------------------
    void set_caption(const char *caption) override;
    void set_base_url(const char *base_url) override;
    void link(const std::shared_ptr<litehtml::document> &doc,
              const litehtml::element::ptr &el) override;
    void on_anchor_click(const char *url, const litehtml::element::ptr &el) override;
    void on_mouse_event(const litehtml::element::ptr &el, litehtml::mouse_event event) override;
    void set_cursor(const char *cursor) override;
    void transform_text(std::string &text, litehtml::text_transform tt) override;
    void import_css(std::string &text, const std::string &url, std::string &baseurl) override;
    void set_clip(const litehtml::position &pos,
                  const litehtml::border_radiuses &bdr_radius) override;
    void del_clip() override;
    void get_viewport(litehtml::position &viewport) const override;
    litehtml::element::ptr create_element(const char *tag_name,
                                          const litehtml::string_map &attributes,
                                          const std::shared_ptr<litehtml::document> &doc) override;
    void get_media_features(litehtml::media_features &media) const override;
    void get_language(std::string &language, std::string &culture) const override;

  private:
    struct FontHandle {
        QFont font;
        // QFontMetricsF has no default constructor; hold it behind a pointer so
        // `new FontHandle` works and the metrics match exactly one font.
        std::unique_ptr<QFontMetricsF> metrics;
    };

    QImage imageFor(const std::string &url);
    static QColor toQColor(litehtml::web_color color);
    QRectF clipRect() const;

    QVector<QRectF> m_clipStack;
    QHash<QByteArray, QImage> m_images;
    int m_viewportWidth;
    qreal m_devicePixelRatio;
};

} // namespace ningmail
