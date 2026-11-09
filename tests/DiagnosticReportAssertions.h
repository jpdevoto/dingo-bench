// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "dingobench/dicom/DiagnosticSession.h"

#include <QDir>
#include <QTemporaryDir>
#include <QtTest>

// Exercises the same serializers and disk reader as the diagnostic panel.
inline void verifyDiagnosticReport(const dingobench::dicom::DiagnosticSession &session, const QString &name)
{
    using namespace dingobench::dicom;
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto directory = qEnvironmentVariable("DINGOBENCH_DIAGNOSTIC_REPORT_TEST_DIR", temporary.path());
    const auto textPath = QDir(directory).filePath(name + ".txt");
    const auto jsonPath = QDir(directory).filePath(name + ".json");
    const auto textError = writeReport(textPath, session, false);
    QVERIFY2(textError.isEmpty(), qPrintable(textError));
    const auto jsonError = writeReport(jsonPath, session, true);
    QVERIFY2(jsonError.isEmpty(), qPrintable(jsonError));
    QString error;
    const auto text = readReport(textPath, error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(text, sessionText(session));
    const auto json = readReport(jsonPath, error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(json.contains(session.id.toString(QUuid::WithoutBraces)));
    QVERIFY(!session.logs.isEmpty());
    for (const auto &event : session.logs)
        QCOMPARE(event.operation, session.id.toString(QUuid::WithoutBraces));
}

