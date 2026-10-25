add_library(ningmail_core STATIC
    MetaTypes.cpp
    SendService.cpp
    Store/AccountStore.cpp
    Store/Database.cpp
    Store/DraftStore.cpp
    Store/FolderStore.cpp
    Store/MessageStore.cpp
    Store/Migrations.cpp
    Store/OutboxStore.cpp
    MemoryCredentialStore.cpp
    SyncService.cpp
    Version.cpp
    Version.h
)

target_include_directories(ningmail_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(ningmail_core
    PUBLIC
        ningmail_platform_kylin
        ningmail_mail
        Qt6::Core
        Qt6::Concurrent
        Qt6::Sql
)

ningmail_apply_warnings(ningmail_core)
