add_subdirectory(unit)

add_test(NAME app_smoke
    COMMAND $<TARGET_FILE:ningmail> --smoke-test
)

set_tests_properties(app_smoke PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)
