# Integration tests talk to real mail servers; they only build when the
# libetpan adapter (the only real backend) is compiled in.
if(NOT NINGMAIL_WITH_LIBETPAN)
    message(STATUS "libetpan missing: integration tests disabled")
    return()
endif()

add_executable(ningmail_integration
    test_libetpan_imap.cpp
)

target_link_libraries(ningmail_integration
    PRIVATE
        ningmail_core
        ningmail_mail
        Catch2::Catch2WithMain
        Qt6::Core
)

ningmail_apply_warnings(ningmail_integration)

include(Catch)
# The adapter exercises Qt types throughout, so the binary runs headless.
# SKIP (no env creds on this host) exits 4; ctest reports it as skipped.
catch_discover_tests(ningmail_integration PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    SKIP_RETURN_CODE "4")
