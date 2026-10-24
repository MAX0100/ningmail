# Third-party dependencies shared by all targets.

find_package(Qt6 6.8 REQUIRED
    COMPONENTS
        Core
        Gui
        Quick
        QuickControls2
        QuickTest
        Sql
        Network
        Concurrent
        Test
        Svg
)

find_package(Catch2 3 REQUIRED)

find_package(ICU REQUIRED COMPONENTS uc)

find_package(PkgConfig REQUIRED)

pkg_check_modules(GUMBO REQUIRED IMPORTED_TARGET gumbo)

# litehtml renders sanitized email HTML into a layout tree (task 10b, R70).
# By default it is fetched at configure time (pinned v0.10 commit; the clone
# under build/_deps is never committed); opt-in NINGMAIL_USE_SYSTEM_LITEHTML=ON
# switches to the system pkg-config copy. litehtml ships its own HTML parser
# (bundled gumbo), so no external parser dependency is added here.
option(NINGMAIL_USE_SYSTEM_LITEHTML "Use a system-installed litehtml (pkg-config)" OFF)

if (NINGMAIL_USE_SYSTEM_LITEHTML)
    pkg_check_modules(LITEHTML REQUIRED IMPORTED_TARGET litehtml)
else()
    include(FetchContent)
    FetchContent_Declare(litehtml
        GIT_REPOSITORY https://github.com/litehtml/litehtml.git
        GIT_TAG 9bc84b8b8d15a4e50f18b327aa30955048b441c2) # v0.10
    FetchContent_MakeAvailable(litehtml)
endif()
