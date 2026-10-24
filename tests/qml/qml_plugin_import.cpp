// Statically imports the NingMail QML plugin so that `import NingMail` inside
// the tst_*.qml files resolves at runtime. Same mechanism as app/qml_plugin_import.cpp
// (see the comment there): qmlimportscanner does not detect the imports used by
// the QuickTest runner from C++ sources, so this explicit import is required.
#include <QtQml/qqmlextensionplugin.h>

Q_IMPORT_QML_PLUGIN(NingMailPlugin)
