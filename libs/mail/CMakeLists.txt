add_library(ningmail_mail STATIC
    Charset.cpp
    FakeMailBackend.cpp
    MimeParser.cpp
)

target_include_directories(ningmail_mail
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}
)

target_link_libraries(ningmail_mail
    PUBLIC
        Qt6::Core
        ICU::uc
)

ningmail_apply_warnings(ningmail_mail)
