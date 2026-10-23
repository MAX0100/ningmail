add_library(ningmail_mail STATIC
    FakeMailBackend.cpp
)

target_include_directories(ningmail_mail
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}
)

target_link_libraries(ningmail_mail
    PUBLIC
        Qt6::Core
)

ningmail_apply_warnings(ningmail_mail)
