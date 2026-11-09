// SPDX-License-Identifier: GPL-3.0-only

#include "../ui/MainWindow.h"
#include "dingobench/infrastructure/AppPaths.h"
#include "dingobench/dicom/Verification.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QIcon>
#include <QPixmap>
#include <QTextStream>
#include <QTimer>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string_view>

namespace {

bool requestsInformation(int argc, char *argv[])
{
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--") {
            break;
        }
        if (argument == "--help" || argument == "-h" || argument == "-?"
            || argument == "--version" || argument == "-v") {
            return true;
        }
    }
    return false;
}

int runWorkspace(QApplication &application, dingobench::logging::Logger &logger, bool smokeTest, quint16 echoPort)
{
    using dingobench::logging::Severity;
    QIcon icon;
    for (const int size : {16, 24, 32, 48, 64, 128, 256})
        icon.addFile(QStringLiteral(":/branding/app-icon-%1.png").arg(size));
    application.setWindowIcon(icon);
    if (icon.pixmap(32, 32).isNull())
        logger.log(Severity::Error, "Workspace", "The bundled application icon could not be loaded.");
    if (!dingobench::ui::installTheme(application))
        logger.log(Severity::Error, "Workspace", "The bundled workspace stylesheet could not be loaded.");

    const auto config = dingobench::paths::configDirectory();
    const auto profilePath = smokeTest || config.isEmpty() ? QString() : QDir(config).filePath("profiles.json");
    if (!smokeTest && config.isEmpty())
        logger.log(Severity::Warning, "Profiles", "No local configuration directory is available.");
    dingobench::MainWindow window(logger, !smokeTest, profilePath);
    dingobench::dicom::OperationRunner echoRunner(logger);
    window.show();
    if (smokeTest) {
        QTimer::singleShot(0, &window, [&window, &logger, &echoRunner, echoPort] {
            const QPixmap expected(QStringLiteral(":/branding/app-icon-32.png"));
            const QPixmap actual = window.windowIcon().pixmap(QSize(32, 32), 1.0);
            if (!window.isVisible() || expected.isNull() || actual.toImage() != expected.toImage()) {
                const auto message = QStringLiteral("The main window or its bundled icon failed verification.");
                logger.log(Severity::Error, "Workspace", message);
                QTextStream(stderr) << message << '\n';
                QCoreApplication::exit(EXIT_FAILURE);
                return;
            }
            if (!echoPort) {
                if (!window.close())
                    QCoreApplication::exit(EXIT_FAILURE);
                return;
            }
            using namespace dingobench::dicom;
            QObject::connect(&echoRunner, &OperationRunner::finished, &window,
                [&window](QUuid, const OperationResult &result) {
                    if (result.status != OperationResult::Status::Succeeded || result.dicomStatus != 0) {
                        QTextStream(stderr) << "Packaged loopback C-ECHO failed: "
                            << (result.error ? result.error->message : result.details) << Qt::endl;
                        QCoreApplication::exit(EXIT_FAILURE);
                    } else if (!window.close()) {
                        QCoreApplication::exit(EXIT_FAILURE);
                    }
                });
            const dingobench::profiles::Profile source{1, "Smoke source", "DINGO_SMOKE", {}, 11112, {}};
            const dingobench::profiles::Profile target{2, "Smoke peer", "DINGO_SMOKE", "127.0.0.1", echoPort, {}};
            if (echoRunner.start("C-ECHO", verificationOperation(source, target, {2, 2, 2}), true).isNull())
                QCoreApplication::exit(EXIT_FAILURE);
        });
    }
    return application.exec();
}

}  // namespace

int main(int argc, char *argv[])
{
    std::unique_ptr<QCoreApplication> application;
    if (requestsInformation(argc, argv)) {
        application = std::make_unique<QCoreApplication>(argc, argv);
    } else {
        application = std::make_unique<QApplication>(argc, argv);
    }
    application->setApplicationName(QStringLiteral("DingoBench"));
    application->setOrganizationName(QStringLiteral("DingoBench"));
    application->setOrganizationDomain(QStringLiteral("jpdevoto.github.io"));
    application->setApplicationVersion(QStringLiteral(DINGOBENCH_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("DICOM troubleshooting tool"));
    const QCommandLineOption help({QStringLiteral("?"), QStringLiteral("h"), QStringLiteral("help")},
                                  QStringLiteral("Display command-line help."));
    const auto version = parser.addVersionOption();
    const QCommandLineOption smokeTest(QStringLiteral("smoke-test"),
                                       QStringLiteral("Verify the window and its icon, then exit."));
    const QCommandLineOption smokeEcho(QStringLiteral("smoke-echo-port"),
        QStringLiteral("With --smoke-test, verify a loopback DINGO_SMOKE DICOM peer."), QStringLiteral("port"));
    parser.addOptions({help, smokeTest, smokeEcho});
    if (!parser.parse(application->arguments())) {
        QTextStream(stderr) << parser.errorText() << '\n';
        return EXIT_FAILURE;
    }
    if (parser.isSet(help)) {
        QTextStream(stdout) << parser.helpText();
        return EXIT_SUCCESS;
    }
    if (parser.isSet(version)) {
        QTextStream(stdout) << application->applicationName() << ' '
                            << application->applicationVersion() << '\n';
        return EXIT_SUCCESS;
    }
    if (!parser.positionalArguments().isEmpty()) {
        QTextStream(stderr) << "Unexpected positional arguments. See --help.\n";
        return EXIT_FAILURE;
    }

    quint16 echoPort = 0;
    if (parser.isSet(smokeEcho)) {
        bool ok = false;
        echoPort = parser.value(smokeEcho).toUShort(&ok);
        if (!parser.isSet(smokeTest) || !ok || !echoPort) {
            QTextStream(stderr) << "--smoke-echo-port requires --smoke-test and a valid TCP port." << Qt::endl;
            return EXIT_FAILURE;
        }
    }

    using namespace dingobench::logging;
    // Relocatable installs can ship DCMTK's optional character maps beside the executable.
    const QDir characterMaps(QDir(QCoreApplication::applicationDirPath()).filePath("dcmtk-charset"));
    if (!qEnvironmentVariableIsSet("DCMICONVPATH") &&
        (characterMaps.exists("esdb/esdb.alias.db") || characterMaps.exists("esdb/esdb.alias")))
        qputenv("DCMICONVPATH", QFile::encodeName(characterMaps.absolutePath()));
    Logger logger;
    QtErrorCapture qtErrors(logger);
    logger.log(Severity::Info, "Application", "DingoBench started.", {}, application->applicationVersion());
    int exitCode = EXIT_FAILURE;
    try {
        exitCode = runWorkspace(*static_cast<QApplication *>(application.get()), logger,
                                parser.isSet(smokeTest), echoPort);
    } catch (...) {
        const auto message = QStringLiteral("An unexpected error stopped the DingoBench workspace.");
        logger.log(Severity::Error, "Workspace", message);
        QTextStream(stderr) << message << '\n';
    }
    logger.log(Severity::Info, "Application", "DingoBench shut down.",
               {}, QStringLiteral("Exit code: %1").arg(exitCode));
    return exitCode;
}
