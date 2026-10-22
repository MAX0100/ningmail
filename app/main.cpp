#include "Types.h"
#include "Version.h"

#include <QGuiApplication>
#include <QQmlApplicationEngine>

#include <iostream>
#include <string_view>

namespace {

int runApplication(int argc, char *argv[], bool smokeTest) {
    QGuiApplication app(argc, argv);
    QQmlApplicationEngine engine;
    engine.loadFromModule(u"NingMail", u"Main");
    if (engine.rootObjects().isEmpty()) {
        qWarning("Failed to load the NingMail UI");
        return 1;
    }
    if (smokeTest) {
        return 0;
    }
    return app.exec();
}

} // namespace

int main(int argc, char *argv[]) {
    ningmail::registerMetaTypes();

    // Parsed without any QGuiApplication so --version also works on headless systems.
    bool smokeTest = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];
        if (argument == "--version") {
            std::cout << "NingMail " << ningmail::version().toStdString() << std::endl;
            return 0;
        }
        if (argument == "--smoke-test") {
            smokeTest = true;
        }
    }
    return runApplication(argc, argv, smokeTest);
}
