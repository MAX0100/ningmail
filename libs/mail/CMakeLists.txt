add_library(ningmail_mail STATIC
    Charset.cpp
    FakeMailBackend.cpp
    MimeBuilder.cpp
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

# libetpan provides the real IMAP/SMTP adapter; its sources join the
# target only when found, the rest of ningmail_mail stays libetpan-free.
if(NINGMAIL_WITH_LIBETPAN)
    target_sources(ningmail_mail PRIVATE
        LibetpanBackend.cpp
        LibetpanBackendFactory.cpp
        LibetpanHelpers.cpp
    )
    target_compile_definitions(ningmail_mail PUBLIC NINGMAIL_WITH_LIBETPAN=1)
    target_link_libraries(ningmail_mail PUBLIC PkgConfig::LIBETPAN)
endif()

ningmail_apply_warnings(ningmail_mail)
