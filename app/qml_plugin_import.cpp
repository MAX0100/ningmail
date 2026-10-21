// Statically imports the NingMail QML plugin so that
// QQmlApplicationEngine::loadFromModule("NingMail", "Main") resolves at runtime.
// qmlimportscanner does not detect loadFromModule in C++ sources, so this explicit
// import is required (same mechanism qt_import_qml_plugins uses for static Qt builds).
#include <QtQml/qqmlextensionplugin.h>

Q_IMPORT_QML_PLUGIN(NingMailPlugin)
