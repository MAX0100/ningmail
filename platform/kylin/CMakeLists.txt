# Kylin platform layer: OS integrations (Secret Service keyring via libsecret).
add_library(ningmail_platform_kylin STATIC)

target_include_directories(ningmail_platform_kylin
    PUBLIC
        ${PROJECT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/app/core
)

target_link_libraries(ningmail_platform_kylin
    PUBLIC
        Qt6::Core
)

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBSECRET IMPORTED_TARGET libsecret-1)
endif()

if(LIBSECRET_FOUND)
    target_sources(ningmail_platform_kylin PRIVATE LibSecretCredentialStore.cpp)
    # GLib headers collide with the Qt `signals`/`slots` macros in this file.
    target_compile_definitions(ningmail_platform_kylin PRIVATE QT_NO_KEYWORDS)
    target_compile_definitions(ningmail_platform_kylin PUBLIC NINGMAIL_HAVE_LIBSECRET)
    target_link_libraries(ningmail_platform_kylin PRIVATE PkgConfig::LIBSECRET)
endif()

ningmail_apply_warnings(ningmail_platform_kylin)
