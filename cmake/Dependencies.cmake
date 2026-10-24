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
