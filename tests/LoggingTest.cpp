// SPDX-License-Identifier: GPL-3.0-only
#include "dingobench/infrastructure/Logger.h"

#include <QSet>
#include <QSignalSpy>
#include <QThread>
#include <QtTest>

#include <thread>

using namespace dingobench::logging;

class LoggingTest final : public QObject {
    Q_OBJECT

private slots:
    void structuredFieldsAndOrder()
    {
        Logger logger;
        QSignalSpy notifications(&logger, &Logger::eventsChanged);
        const auto before = QDateTime::currentDateTimeUtc();
        for (int index = 0; index < 20; ++index)
            logger.log(Severity(index % 4), "Verification", QString("Event %1").arg(index),
                       QString("OP-%1").arg(index), "Presentation context: 1", true);
        const auto after = QDateTime::currentDateTimeUtc();
        const auto events = logger.snapshot();
        QCOMPARE(events.size(), 20);
        for (int index = 0; index < 20; ++index) {
            const auto &event = events.at(index);
            QCOMPARE(event.sequence, quint64(index + 1));
            QCOMPARE(event.severity, Severity(index % 4));
            QCOMPARE(event.component, QString("Verification"));
            QCOMPARE(event.message, QString("Event %1").arg(index));
            QCOMPARE(event.operation, QString("OP-%1").arg(index));
            QCOMPARE(event.details, QString("Presentation context: 1"));
            QVERIFY(event.synthetic);
            QCOMPARE(event.timestampUtc.timeSpec(), Qt::UTC);
            QVERIFY(event.timestampUtc >= before && event.timestampUtc <= after);
            QCOMPARE(QDateTime::fromString(event.timestampUtc.toString(Qt::ISODateWithMs),
                                           Qt::ISODateWithMs), event.timestampUtc);
        }
        QTRY_COMPARE(notifications.size(), 1);
    }

    void filterAndClear()
    {
        Logger logger;
        logger.log(Severity::Info, "Application", "Already recorded");
        logger.setMinimumSeverity(Severity::Warning);
        logger.log(Severity::Debug, "Application", "Skipped");
        logger.log(Severity::Info, "Application", "Skipped");
        logger.log(Severity::Warning, "Application", "Retained warning");
        logger.log(Severity::Error, "Application", "Retained error");
        const auto events = logger.snapshot();
        QCOMPARE(events.size(), 3);
        QCOMPARE(events.at(0).severity, Severity::Info);
        QCOMPARE(events.at(1).severity, Severity::Warning);
        QCOMPARE(events.at(2).severity, Severity::Error);
        QSignalSpy notifications(&logger, &Logger::eventsChanged);
        logger.clear();
        QVERIFY(logger.snapshot().isEmpty());
        QTRY_COMPARE(notifications.size(), 1);
        logger.log(Severity::Error, "Application", "After clearing");
        QVERIFY(logger.snapshot().first().sequence > events.last().sequence);
    }

    void capacity()
    {
        Logger logger;
        logger.setCapacity(100);
        for (int index = 0; index < 150; ++index)
            logger.log(Severity::Info, "Capacity test", QString::number(index));
        auto events = logger.snapshot();
        QCOMPARE(events.size(), 100);
        for (int index = 0; index < 100; ++index)
            QCOMPARE(events.at(index).message, QString::number(index + 50));
        logger.setCapacity(25);
        events = logger.snapshot();
        QCOMPARE(events.size(), 25);
        QCOMPARE(events.first().message, QString("125"));
        QCOMPARE(events.last().message, QString("149"));
    }

    void workers()
    {
        Logger logger;
        auto *const owner = QThread::currentThread();
        bool deliveredOnOwner = false;
        connect(&logger, &Logger::eventsChanged, this,
                [&] { deliveredOnOwner = QThread::currentThread() == owner; }, Qt::DirectConnection);
        auto producer = [&logger](QString component) {
            for (int index = 0; index < 10; ++index)
                logger.log(Severity::Debug, component, QString::number(index));
        };
        std::thread first(producer, QStringLiteral("Worker A"));
        std::thread second(producer, QStringLiteral("Worker B"));
        first.join();
        second.join();
        QTRY_VERIFY(deliveredOnOwner);
        const auto events = logger.snapshot();
        QCOMPARE(events.size(), 20);
        QSet<QString> observed;
        for (int index = 0; index < events.size(); ++index) {
            const auto &event = events.at(index);
            QCOMPARE(event.sequence, quint64(index + 1));
            observed.insert(event.component + "/" + event.message);
        }
        for (const auto &component : {"Worker A", "Worker B"})
            for (int index = 0; index < 10; ++index)
                QVERIFY(observed.contains(QString(component) + "/" + QString::number(index)));
    }

    void emptyUnicodeMultilineAndDetails()
    {
        Logger logger;
        logger.log(Severity::Debug, {}, " \t\n");
        logger.log(Severity::Info, "Inspector", QStringLiteral("Données — 検査\nSecond line"),
                   {}, "Association result\nContext ID: 3");
        const auto events = logger.snapshot();
        QCOMPARE(events.first().component, QString("Application"));
        QCOMPARE(events.first().message, QString("(No message provided)"));
        QVERIFY(events.first().operation.isEmpty());
        QCOMPARE(events.last().message, QStringLiteral("Données — 検査\nSecond line"));
        QCOMPARE(events.last().details, QString("Association result\nContext ID: 3"));
        logger.log(Severity::Warning, "Inspector", QString(4500, QChar('a')));
        QCOMPARE(logger.snapshot().last().message.size(), 4096);
    }

    void unexpectedQtError()
    {
        Logger logger;
        {
            QtErrorCapture bridge(logger);
            QTest::ignoreMessage(QtCriticalMsg, "Unexpected Qt critical diagnostic.");
            qCritical("Private input used as a test sentinel");
        }
        const auto events = logger.snapshot();
        QCOMPARE(events.size(), 1);
        QCOMPARE(events.first().severity, Severity::Error);
        QCOMPARE(events.first().component, QString("Qt"));
        QCOMPARE(events.first().message, QString("Unexpected Qt critical diagnostic."));
        QVERIFY(!events.first().synthetic);
    }
};

QTEST_GUILESS_MAIN(LoggingTest)
#include "LoggingTest.moc"
