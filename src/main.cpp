#include "ui/MainWindow.h"
#include "Version.h"
#include <QApplication>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("aiaixyz"));
    QApplication::setApplicationName(QStringLiteral("MineMic-PC"));
    QApplication::setApplicationVersion(QStringLiteral(MINEMIC_VERSION));
    QApplication::setWindowIcon(minemic::applicationIcon());
    const bool smokeTest = app.arguments().contains(QStringLiteral("--smoke-test"));
    QTemporaryDir temporarySettings;
    if (smokeTest) {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporarySettings.path());
    }
    minemic::MainWindow window;
    window.show();
    if (smokeTest) {
        // Leave user's saved settings untouched during unattended validation.
        QTimer::singleShot(250, &app, [&] {
            const auto preview = qEnvironmentVariable("MINEMIC_PREVIEW_PATH");
            if (!preview.isEmpty()) window.grab().save(preview);
            app.quit();
        });
    }
    return app.exec();
}
