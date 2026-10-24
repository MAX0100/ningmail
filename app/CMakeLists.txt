add_subdirectory(core)
add_subdirectory(ui)

qt_add_executable(ningmail
    main.cpp
    qml_plugin_import.cpp
)

target_link_libraries(ningmail
    PRIVATE
        ningmail_core
        ningmail_mail
        ningmail_ui
        ningmail_uiplugin
)

ningmail_apply_warnings(ningmail)
