file(GLOB UNIT_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp)

add_executable(ningmail_tests
    main.cpp
    ${UNIT_TEST_SOURCES}
)

target_link_libraries(ningmail_tests
    PRIVATE
        ningmail_core
        ningmail_mail
        Catch2::Catch2
        Qt6::Test
)

target_compile_definitions(ningmail_tests
    PRIVATE
        NINGMAIL_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/mime")

ningmail_apply_warnings(ningmail_tests)

include(Catch)
catch_discover_tests(ningmail_tests)
