cmake_minimum_required(VERSION 3.16)

project(NingMail VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(cmake/CompilerWarnings.cmake)
include(cmake/Dependencies.cmake)

qt_standard_project_setup()

# ':/qt/qml/' is the default resource prefix for QML modules (Qt 6.5+).
qt_policy(SET QTP0001 NEW)

enable_testing()

add_subdirectory(platform/kylin)
add_subdirectory(libs/mail)
add_subdirectory(app)
add_subdirectory(tests)
