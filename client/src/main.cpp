#include "browsercontroller.hpp"

#include <bb/cascades/Application>
#include <bb/cascades/QmlDocument>
#include <bb/cascades/AbstractPane>

#include <QLocale>
#include <QTranslator>

#include "tls/bbportlog.hpp"

using namespace bb::cascades;

Q_DECL_EXPORT int main(int argc, char **argv)
{
    Application app(argc, argv);

    // English by default; Feather_<language>.qm (translations/*.ts, compiled by
    // package.ps1) when the phone uses that language, e.g. Feather_it.qm.
    QTranslator translator;
    if (translator.load("Feather_" + QLocale().name(), "app/native/assets/translations")) {
        app.installTranslator(&translator);
    }

    BrowserController controller;

    QmlDocument *qml = QmlDocument::create("asset:///main.qml").parent(&controller);
    qml->setContextProperty("controller", &controller);

    AbstractPane *root = qml->createRootObject<AbstractPane>();
    if (!root) {
        // QML failed to load (syntax error, unknown type/property): the app
        // would otherwise just sit on a black screen with no trace.
        bbportLog("[Feather] FATAL: main.qml failed to create the root object");
    }
    Application::instance()->setScene(root);

    return Application::exec();
}
