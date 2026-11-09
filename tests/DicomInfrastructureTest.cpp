// SPDX-License-Identifier: GPL-3.0-only
#include "dingobench/dicom/Dependencies.h"
#include "dingobench/dicom/OperationRunner.h"
#include "dingobench/dicom/UtilityOperation.h"

#include <QApplication>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QtTest>

#include <cstdio>
#include <stdexcept>
#include <string_view>
#include <thread>

using namespace dingobench::dicom;
using dingobench::logging::Logger;
using dingobench::logging::Severity;
using namespace std::chrono_literals;

class DicomInfrastructureTest final : public QObject {
    Q_OBJECT

private slots:
    void toolkit()
    {
        const auto info = dependencies();
        QCOMPARE(info.version, QStringLiteral("3.7.1"));
        const QStringList modules{"dcmnet", "dcmdata", "ofstd", "oflog"};
        QCOMPARE(info.modules, modules);
        QVERIFY(info.associationParametersAvailable);
    }

    void backgroundLifecycle()
    {
        Logger logger;
        OperationRunner runner(logger);
        QSignalSpy finished(&runner, &OperationRunner::finished);
        QVERIFY(finished.isValid());
        QThread *deliveryThread = nullptr;
        connect(&runner, &OperationRunner::finished, this,
                [&] { deliveryThread = QThread::currentThread(); });

        int ticks = 0;
        QTimer heartbeat;
        connect(&heartbeat, &QTimer::timeout, this, [&] { ++ticks; });
        heartbeat.start(10);
        auto *guiThread = QThread::currentThread();
        const auto id = runner.start("mock operation", [guiThread](auto, const auto &report) {
            if (QThread::currentThread() == guiThread)
                return OperationResult::failed({ErrorCode::Internal, "Running on GUI thread"});
            report({Severity::Debug, "Synthetic operation progress"});
            std::this_thread::sleep_for(300ms);
            return OperationResult{};
        }, true);
        QVERIFY(!id.isNull());
        QVERIFY(runner.start("busy", [](auto, const auto &) { return OperationResult{}; }).isNull());
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(finished.first().at(0).value<QUuid>(), id);
        QCOMPARE(finished.first().at(1).value<OperationResult>().status, OperationResult::Status::Succeeded);
        QCOMPARE(deliveryThread, guiThread);
        QVERIFY2(ticks >= 3, "GUI event loop stalled during background work.");
        const auto events = logger.snapshot();
        QCOMPARE(events.size(), 3);
        QCOMPARE(events.at(1).message, QStringLiteral("Synthetic operation progress"));
        for (const auto &event : events) {
            QCOMPARE(event.operation, id.toString(QUuid::WithoutBraces));
            QVERIFY(event.details.startsWith(QStringLiteral("mock operation")));
            QVERIFY(event.synthetic);
        }
        QVERIFY(events.last().details.contains(QStringLiteral("Elapsed ")));
        QVERIFY(!runner.cancel(id));
    }

    void cancellationAndFailure()
    {
        Logger logger;
        OperationRunner runner(logger);
        QSignalSpy finished(&runner, &OperationRunner::finished);
        const auto id = runner.start("cancel", [](auto stop, const auto &report) {
            report({Severity::Info, "Waiting for cancellation"});
            while (!stop.stop_requested())
                std::this_thread::sleep_for(5ms);
            return OperationResult::cancelled();
        }, true);
        QTRY_COMPARE(logger.snapshot().size(), 2);
        QVERIFY(!runner.cancel(QUuid::createUuid()));
        QVERIFY(runner.cancel(id));
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(finished.takeFirst().at(1).value<OperationResult>().status, OperationResult::Status::Cancelled);

        QVERIFY(!runner.start("failure", [](auto, const auto &) {
            return OperationResult::failed({ErrorCode::TimedOut, "Synthetic timeout"});
        }, true).isNull());
        QTRY_COMPARE(finished.size(), 1);
        const auto failure = finished.takeFirst().at(1).value<OperationResult>();
        QCOMPARE(failure.status, OperationResult::Status::Failed);
        QVERIFY(failure.error.has_value());
        QCOMPARE(failure.error->code, ErrorCode::TimedOut);
        QCOMPARE(failure.error->message, QStringLiteral("Synthetic timeout"));
        QCOMPARE(logger.snapshot().last().severity, Severity::Error);
        QCOMPARE(logger.snapshot().last().message, failure.error->message);

        QVERIFY(!runner.start("exception", [](auto, const auto &) -> OperationResult {
            throw std::runtime_error("private exception details");
        }, true).isNull());
        QTRY_COMPARE(finished.size(), 1);
        const auto exception = finished.first().at(1).value<OperationResult>();
        QVERIFY(exception.error.has_value());
        QCOMPARE(exception.error->code, ErrorCode::Internal);
        QVERIFY(!logger.snapshot().last().message.contains("private exception details"));
    }

    void destructorStopsWorker()
    {
        Logger logger;
        {
            OperationRunner runner(logger);
            QVERIFY(!runner.start("shutdown", [](auto stop, const auto &report) {
                report({Severity::Info, "Waiting for shutdown"});
                while (!stop.stop_requested())
                    std::this_thread::sleep_for(5ms);
                return OperationResult::cancelled();
            }, true).isNull());
            QTRY_COMPARE(logger.snapshot().size(), 2);
        }
        QCOMPARE(logger.snapshot().last().message, QStringLiteral("Operation cancelled."));
    }

    void supervisedUtility_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<int>("expectedError");
        QTest::newRow("normal exit") << QString("success") << -1;
        QTest::newRow("nonzero exit") << QString("failure") << int(ErrorCode::ProcessFailed);
        QTest::newRow("timeout") << QString("sleep") << int(ErrorCode::TimedOut);
        QTest::newRow("missing executable") << QString("missing") << int(ErrorCode::StartFailed);
        QTest::newRow("relative executable") << QString("relative") << int(ErrorCode::InvalidInput);
        QTest::newRow("cancel running utility") << QString("cancel") << -1;
    }

    void supervisedUtility()
    {
        QFETCH(QString, mode);
        QFETCH(int, expectedError);
        Logger logger;
        OperationRunner runner(logger);
        QSignalSpy finished(&runner, &OperationRunner::finished);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QString executable = QCoreApplication::applicationFilePath();
        if (mode == "missing")
            executable = directory.filePath("missing-dcmtk-test-utility");
        if (mode == "relative")
            executable = "relative-utility";
        const auto marker = directory.filePath("child-started");
        const auto id = runner.start("test utility", utilityOperation(executable,
            {"--mock-utility", mode, "a single argument with spaces", marker},
            mode == "sleep" ? 100ms : 10s), true);
        QVERIFY(!id.isNull());
        if (mode == "cancel") {
            QTRY_VERIFY(QFile::exists(marker));
            QVERIFY(finished.isEmpty());
            QVERIFY(runner.cancel(id));
        }
        QTRY_COMPARE(finished.size(), 1);
        const auto result = finished.first().at(1).value<OperationResult>();
        if (expectedError < 0) {
            QCOMPARE(result.status, mode == "cancel" ? OperationResult::Status::Cancelled
                                                     : OperationResult::Status::Succeeded);
            QVERIFY(!result.error);
        } else {
            QCOMPARE(result.status, OperationResult::Status::Failed);
            QVERIFY(result.error.has_value());
            QCOMPARE(int(result.error->code), expectedError);
            if (mode == "failure")
                QCOMPARE(result.error->exitCode.value_or(-1), 7);
        }
        for (const auto &event : logger.snapshot()) {
            QVERIFY(!event.message.contains("private child output"));
            QVERIFY(!event.details.contains("a single argument with spaces"));
        }
    }
};

int main(int argc, char **argv)
{
    if (argc == 5 && std::string_view(argv[1]) == "--mock-utility") {
        if (std::string_view(argv[3]) != "a single argument with spaces")
            return 99;
        std::fputs("private child output\n", stderr);
        const std::string_view mode(argv[2]);
        if (mode == "cancel") {
            QFile marker(QString::fromLocal8Bit(argv[4]));
            if (!marker.open(QIODevice::WriteOnly))
                return 98;
            marker.close();
        }
        if (mode == "sleep" || mode == "cancel")
            std::this_thread::sleep_for(20s);
        return mode == "failure" ? 7 : 0;
    }
    QApplication app(argc, argv);
    DicomInfrastructureTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "DicomInfrastructureTest.moc"
