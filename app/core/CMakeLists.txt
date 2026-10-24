add_library(ningmail_core STATIC
    MetaTypes.cpp
    Store/AccountStore.cpp
    Store/Database.cpp
    Store/FolderStore.cpp
    Store/MessageStore.cpp
    Store/Migrations.cpp
    MemoryCredentialStore.cpp
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
        Qt6::Core
        Qt6::Sql
)

ningmail_apply_warnings(ningmail_core)
