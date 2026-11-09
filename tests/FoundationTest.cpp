// SPDX-License-Identifier: GPL-3.0-only

#include "dingobench/infrastructure/AppPaths.h"

#include <QDir>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QtTest>

class FoundationTest final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QCoreApplication::setApplicationName(QStringLiteral("DingoBenchTest"));
        QCoreApplication::setOrganizationName(QStringLiteral("DingoBench"));
        QStandardPaths::setTestModeEnabled(true);
    }

    void applicationPaths()
    {
        using namespace dingobench::paths;

        const auto config = configDirectory();
        const auto data = localDataDirectory();
        const auto cache = cacheDirectory();
        QVERIFY(QDir::isAbsolutePath(config));
        QVERIFY(QDir::isAbsolutePath(data));
        QVERIFY(QDir::isAbsolutePath(cache));
        QCOMPARE(config, QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation));
        QCOMPARE(data, QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation));
        QCOMPARE(cache, QStandardPaths::writableLocation(QStandardPaths::CacheLocation));
    }

    void informationalCommand_data()
    {
        QTest::addColumn<QString>("option");
        QTest::addColumn<QByteArray>("expected");
        QTest::newRow("help") << QStringLiteral("--help") << QByteArray("Usage:");
        QTest::newRow("help-short") << QStringLiteral("-h") << QByteArray("Usage:");
        QTest::newRow("help-question-mark") << QStringLiteral("-?") << QByteArray("Usage:");
        QTest::newRow("version") << QStringLiteral("--version")
                                 << QByteArray("DingoBench " DINGOBENCH_VERSION);
    }

    void informationalCommand()
    {
        QFETCH(QString, option);
        QFETCH(QByteArray, expected);
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("unavailable-for-test"));

        QProcess child;
        child.setProcessEnvironment(environment);
        child.start(QStringLiteral(DINGOBENCH_EXECUTABLE), {option});
        QVERIFY2(child.waitForStarted(5000), qPrintable(child.errorString()));
        QVERIFY2(child.waitForFinished(5000), qPrintable(child.errorString()));
        QCOMPARE(child.exitStatus(), QProcess::NormalExit);
        QVERIFY2(child.exitCode() == 0, child.readAllStandardError().constData());
        const auto output = child.readAllStandardOutput();
        QVERIFY2(output.contains(expected), output.constData());
    }
};

QTEST_GUILESS_MAIN(FoundationTest)
#include "FoundationTest.moc"
