#include <QGuiApplication>

#include <qtconfigmacros.h>

#include <catch2/catch_session.hpp>

int main(int argc, char *argv[]) {
    // The renderer creates fonts (task 10b), which needs QGuiApplication and a
    // working platform. Headless hosts get the offscreen platform plugin unless
    // the caller picked one explicitly, so plain direct runs also work.
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QGuiApplication app(argc, argv);
    return Catch::Session().run(argc, argv);
}
