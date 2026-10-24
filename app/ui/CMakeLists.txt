qt_add_library(ningmail_ui STATIC)

set_source_files_properties(Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(ningmail_ui
    URI NingMail
    VERSION 0.1
    OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/NingMail
    QML_FILES
        Main.qml
        Theme.qml
        EmptyState.qml
        FolderRail.qml
        MessageList.qml
        MessageListDelegate.qml
        ReaderPane.qml
        StatusBar.qml
        TopBar.qml
    SOURCES
        AppController.h
        AppController.cpp
        models/FolderListModel.h
        models/FolderListModel.cpp
        models/MessageListModel.h
        models/MessageListModel.cpp
)

# models/ headers are included as "models/..." by tests and AppController.
target_include_directories(ningmail_ui
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(ningmail_ui
    PUBLIC
        Qt6::Quick
        Qt6::QuickControls2
        ningmail_core
)

ningmail_apply_warnings(ningmail_ui)
