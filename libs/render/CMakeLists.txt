qt_add_library(ningmail_render STATIC)

qt_add_qml_module(ningmail_render
    URI NingMail.Components
    VERSION 1.0
    # QML module (R74): MessageViewItem carries QML_ELEMENT and is registered
    # as NingMail.Components 1.0; the sanitizer/renderer/container sources ride
    # in the same module target.
    SOURCES
        HtmlSanitizer.cpp
        HtmlSanitizer.h
        LiteHtmlRenderer.cpp
        LiteHtmlRenderer.h
        MessageViewItem.cpp
        MessageViewItem.h
        litehtml_qt/QtDocumentContainer.cpp
        litehtml_qt/QtDocumentContainer.h
)

# models/ headers are included as "..." by tests; project-root includes too.
target_include_directories(ningmail_render
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}
)

if (NINGMAIL_USE_SYSTEM_LITEHTML)
    target_link_libraries(ningmail_render PRIVATE PkgConfig::LITEHTML)
else()
    target_link_libraries(ningmail_render PRIVATE litehtml)
endif()

target_link_libraries(ningmail_render
    PUBLIC
        Qt6::Core
        Qt6::Gui
        Qt6::Quick
        Qt6::Concurrent
        PkgConfig::GUMBO
)

ningmail_apply_warnings(ningmail_render)
