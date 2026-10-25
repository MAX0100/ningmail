file(GLOB UNIT_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp)
# Round 2 item 2: the libetpan unit tests build raw libetpan C structures,
# so they only join the suite when the libetpan gate is on (semantic
# no-op on hosts that always have libetpan).
if(NOT NINGMAIL_WITH_LIBETPAN)
    list(FILTER UNIT_TEST_SOURCES EXCLUDE REGEX "test_libetpan")
endif()

add_executable(ningmail_tests
    main.cpp
    ${UNIT_TEST_SOURCES}
)

target_link_libraries(ningmail_tests
    PRIVATE
        ningmail_core
        ningmail_mail
        ningmail_render
        ningmail_ui
        Catch2::Catch2
        Qt6::Test
)

target_compile_definitions(ningmail_tests
    PRIVATE
        NINGMAIL_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/mime")

ningmail_apply_warnings(ningmail_tests)

include(Catch)
# The renderer needs fonts, so every test binary now runs a QGuiApplication;
# offscreen keeps them working on headless hosts (direct runs also set the
# platform from main.cpp when the variable is unset).
catch_discover_tests(ningmail_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
