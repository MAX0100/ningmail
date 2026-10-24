qt_add_executable(tst_qml
    tst_qml.cpp
    qml_plugin_import.cpp
)

# The NingMail plugin import (see qml_plugin_import.cpp) must be linked in so
# the module resolves statically at runtime.
target_link_libraries(tst_qml
    PRIVATE
        Qt6::QuickTest
        Qt6::Quick
        ningmail_ui
        ningmail_uiplugin
)

target_compile_definitions(tst_qml
    PRIVATE
        QUICK_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

add_test(NAME tst_qml COMMAND tst_qml)

set_tests_properties(tst_qml PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

ningmail_apply_warnings(tst_qml)
