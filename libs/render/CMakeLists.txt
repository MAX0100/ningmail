add_library(ningmail_render STATIC
    HtmlSanitizer.cpp
)

target_include_directories(ningmail_render
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}
)

target_link_libraries(ningmail_render
    PUBLIC
        Qt6::Core
        PkgConfig::GUMBO
)

ningmail_apply_warnings(ningmail_render)
