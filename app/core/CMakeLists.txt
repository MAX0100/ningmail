add_library(ningmail_core STATIC
    MetaTypes.cpp
    Version.cpp
    Version.h
)

target_include_directories(ningmail_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(ningmail_core
    PUBLIC
        Qt6::Core
)

ningmail_apply_warnings(ningmail_core)
