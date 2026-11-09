// SPDX-License-Identifier: GPL-3.0-only
#include "DiagnosticReportAssertions.h"
#include "MainWindow.h"
#include "dingobench/dicom/WorklistFind.h"
#include "dingobench/dicom/WorklistServer.h"
#include "../src/dicom/WorklistSampleFiles.h"

#include <dcmtk/config/osconfig.h>
#include <dcmtk/dcmdata/dcdeftag.h>
#include <dcmtk/dcmdata/dcfilefo.h>
#include <dcmtk/dcmdata/dcuid.h>

#include <QCheckBox>
#include <QCryptographicHash>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QLabel>
#include <QLineEdit>
#include <QLockFile>
#include <QProcess>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeView>
#include <QtTest>

#include <algorithm>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <csignal>
#include <unistd.h>
#endif

using namespace dingobench;
using namespace dingobench::dicom;
using Status = OperationResult::Status;

namespace {

profiles::Profile source()
{
    return {1, "Synthetic modality", "TEST_MODALITY", {}, 11112, {}};
}

profiles::Profile destination(int port)
{
    return {2, "Synthetic Worklist SCP", "TEST_MWL", "127.0.0.1", port, {}};
}

const Attribute *findAttribute(const QList<Attribute> &attributes, const QString &tag)
{
    for (const auto &entry : attributes)
        if (entry.tag == tag)
            return &entry;
    return nullptr;
}

QString value(const QList<Attribute> &attributes, const QString &tag)
{
    const auto *entry = findAttribute(attributes, tag);
    return entry ? entry->value : QString{};
}

bool writeRecord(const QString &path, int number, const char *charset, const char *patient,
                 const char *modality, const char *date, const char *station)
{
    DcmFileFormat format;
    auto &dataset = *format.getDataset();
    OFCondition condition = EC_Normal;
    const auto add = [&](DcmItem &item, const DcmTagKey &tag, const char *text) {
        if (condition.good())
            condition = item.putAndInsertString(tag, text);
    };
    const QByteArray id = QByteArray::number(number);
    add(dataset, DCM_SpecificCharacterSet, charset);
    add(dataset, DCM_PatientName, patient);
    add(dataset, DCM_PatientID, (QByteArray("PAT-") + id).constData());
    add(dataset, DCM_AccessionNumber, (QByteArray("ACC-") + id).constData());
    add(dataset, DCM_RequestedProcedureID, (QByteArray("REQ-") + id).constData());
    add(dataset, DCM_RequestedProcedureDescription, "Phantom calibration");
    add(dataset, DCM_StudyInstanceUID, (QByteArray("1.2.826.0.1.3680043.10.999.14.") + id).constData());
    for (const auto &tag : {DCM_ReferencedStudySequence, DCM_ReferencedPatientSequence})
        if (condition.good())
            condition = dataset.insertEmptyElement(tag);
    DcmItem *step = nullptr;
    if (condition.good())
        condition = dataset.findOrCreateSequenceItem(DCM_ScheduledProcedureStepSequence, step, 0);
    if (condition.bad())
        return false;
    add(*step, DCM_ScheduledStationAETitle, station);
    add(*step, DCM_ScheduledProcedureStepStartDate, date);
    add(*step, DCM_ScheduledProcedureStepStartTime, "093015.25");
    add(*step, DCM_Modality, modality);
    add(*step, DCM_ScheduledProcedureStepID, (QByteArray("STEP-") + id).constData());
    add(*step, DCM_ScheduledProcedureStepDescription, number == 1 ? "R\xE9vision de fant\xF4me" : "Check phantom");
    add(*step, DCM_ScheduledStationName, "QA scanner");
    add(*step, DCM_ScheduledProcedureStepLocation, "Lab");
    DcmItem *code = nullptr;
    if (condition.good())
        condition = step->findOrCreateSequenceItem(DCM_ScheduledProtocolCodeSequence, code, 0);
    if (condition.bad())
        return false;
    add(*code, DCM_CodeValue, "QA14");
    add(*code, DCM_CodingSchemeDesignator, "99BENCH");
    add(*code, DCM_CodeMeaning, "Phantom protocol");
    return condition.good() && format.saveFile(OFFilename(QFile::encodeName(path).constData()),
        EXS_LittleEndianExplicit, EET_ExplicitLength, EGL_recalcGL, EPD_withoutPadding, 0, 0, EWM_dataset).good();
}

class Fixture {
public:
    QTemporaryDir files{QDir(QCoreApplication::applicationDirPath()).filePath("mwl-synthetic-XXXXXX")};
    QProcess process;
    int port = 0;

    ~Fixture()
    {
        if (process.state() != QProcess::NotRunning) {
            process.kill();
            process.waitForFinished(3000);
        }
    }

    bool start(const QString &executable, QStringList extra = {})
    {
        if (!files.isValid() || !QDir(files.path()).mkdir("TEST_MWL"))
            return false;
        QFile lock(files.filePath("TEST_MWL/lockfile"));
        if (!lock.open(QIODevice::WriteOnly))
            return false;
        lock.close();
        if (!writeRecord(files.filePath("TEST_MWL/one.wl"), 1, "ISO_IR 100", "B\xE9langer^Phantom",
                         "CT", "20261103", "SCANNER_CT") ||
            !writeRecord(files.filePath("TEST_MWL/two.wl"), 2, "ISO_IR 192", "Garc\xC3\xAD" "a^Phantom",
                         "MR", "20261103", "SCANNER_MR") ||
            !writeRecord(files.filePath("TEST_MWL/three.wl"), 3, "ISO_IR 192", "Phantom^Gamma",
                         "CT", "20261119", "SCANNER_CT2"))
            return false;
        QTcpServer reserve;
        if (!reserve.listen(QHostAddress::LocalHost, 0))
            return false;
        port = reserve.serverPort();
        reserve.close();
        auto args = QStringList{"--single-process", "--data-files-path", files.path(), "--keep-char-set",
                                "--disable-host-lookup", "--acse-timeout", "2", "--dimse-timeout", "2"};
        args.append(extra);
        args.append(QString::number(port));
        process.setProcessChannelMode(QProcess::MergedChannels);
        process.start(executable, args);
        if (!process.waitForStarted(3000))
            return false;
        QElapsedTimer deadline;
        deadline.start();
        while (deadline.elapsed() < 3000 && process.state() != QProcess::NotRunning) {
            QTcpSocket probe;
            probe.connectToHost(QHostAddress::LocalHost, quint16(port));
            if (probe.waitForConnected(50))
                return true;
            QTest::qWait(25);
        }
        return false;
    }
};

OperationResult query(int port, const WorklistRequest &request, QList<QueryMatch> &matches,
                      ClientTimeouts timeouts = {2, 2, 2})
{
    matches.clear();
    return worklistOperation(source(), destination(port), request, timeouts, true,
        [&](QueryMatch match) { matches.append(std::move(match)); })({}, [](DiagnosticEvent) {});
}

QStringList stages()
{
    return QDir(QDir::tempPath()).entryList({"dingobench-mwl-*"}, QDir::Dirs | QDir::NoDotAndDotDot);
}

bool killServer(qint64 pid)
{
    if (pid <= 0)
        return false;
#ifdef Q_OS_WIN
    const auto handle = OpenProcess(PROCESS_TERMINATE, FALSE, DWORD(pid));
    if (!handle)
        return false;
    const bool stopped = TerminateProcess(handle, 91);
    CloseHandle(handle);
    return stopped;
#else
    return ::kill(pid_t(pid), SIGKILL) == 0;
#endif
}

QByteArray digest(const QString &path)
{
    QFile file(path);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    return file.open(QIODevice::ReadOnly) && hash.addData(&file) ? hash.result() : QByteArray{};
}

bool writeFindRequest(const QString &path)
{
    DcmFileFormat request;
    DcmItem *step = nullptr;
    return request.getDataset()->putAndInsertString(DCM_PatientID, "").good() &&
        request.getDataset()->findOrCreateSequenceItem(DCM_ScheduledProcedureStepSequence, step, 0).good() &&
        step->putAndInsertString(DCM_Modality, "CT").good() &&
        step->putAndInsertString(DCM_ScheduledProcedureStepStartDate, "20261103").good() &&
        request.saveFile(OFFilename(QFile::encodeName(path).constData()), EXS_LittleEndianExplicit,
                         EET_ExplicitLength, EGL_recalcGL, EPD_withoutPadding, 0, 0, EWM_dataset).good();
}

}  // namespace

class WorklistFindTest final : public QObject {
    Q_OBJECT
    QTemporaryDir m_runtime{QDir(QCoreApplication::applicationDirPath()).filePath("worklist-runtime-XXXXXX")};

private slots:
    void initTestCase()
    {
        QVERIFY(m_runtime.isValid());
        const auto path = QFile::encodeName(QDir::toNativeSeparators(m_runtime.path()));
        for (const auto *name : {"TMP", "TEMP", "TMPDIR"})
            QVERIFY(qputenv(name, path));
        QCOMPARE(QDir(QDir::tempPath()).canonicalPath(), QDir(m_runtime.path()).canonicalPath());
    }

    void validatesFiltersAndHandlesRefusedConnection()
    {
        WorklistRequest request;
        QVERIFY(validateWorklist(request).isEmpty());
        request.scheduledDate = "20261101-";
        QVERIFY(validateWorklist(request).isEmpty());
        request.scheduledDate = "20261103-20261101";
        QVERIFY(!validateWorklist(request).isEmpty());
        request.scheduledDate = "20261131";
        QList<QueryMatch> matches;
        const auto invalid = query(1, request, matches);
        QVERIFY(invalid.error);
        QCOMPARE(invalid.error->code, ErrorCode::InvalidInput);
        QVERIFY(!invalid.protocol);
        request = {};
        request.stationAe = "ONE\\TWO";
        QVERIFY(!validateWorklist(request).isEmpty());
        request.stationAe = QString(17, u'A');
        QVERIFY(!validateWorklist(request).isEmpty());
        request.stationAe.clear();
        request.modality = "ct*";
        QVERIFY(!validateWorklist(request).isEmpty());
        request.modality.clear();
        request.patientName = "ONE^TWO^THREE^FOUR^FIVE^SIX";
        QVERIFY(!validateWorklist(request).isEmpty());
        request.patientName.clear();
        request.patientId = QString(65, u'X');
        QVERIFY(!validateWorklist(request).isEmpty());

        QTcpServer reserve;
        QVERIFY(reserve.listen(QHostAddress::LocalHost, 0));
        const auto port = reserve.serverPort();
        reserve.close();
        const auto refused = query(port, {}, matches);
        QVERIFY(refused.error);
        QCOMPARE(refused.error->code, ErrorCode::TcpFailure);
        QVERIFY(matches.isEmpty());
    }

    void nativeQueriesAndLiveWorkspace()
    {
        const auto executable = qEnvironmentVariable("DINGOBENCH_WLMSCPFS");
        if (executable.isEmpty())
            QSKIP("Set DINGOBENCH_WLMSCPFS to a local DCMTK wlmscpfs executable.");
        Fixture peer;
        const auto started = peer.start(executable);
        QVERIFY2(started, peer.process.readAll().constData());

        QList<QueryMatch> matches;
        auto result = query(peer.port, {}, matches);
        QVERIFY2(result.status == Status::Succeeded, qPrintable(result.error ? result.error->message : result.details));
        QCOMPARE(result.dicomStatus.value_or(1), 0);
        QCOMPARE(matches.size(), 3);
        QVERIFY(result.protocol);
        QCOMPARE(result.protocol->association.contexts.first().abstractSyntax,
                 QString(UID_FINDModalityWorklistInformationModel));
        QVERIFY(!result.protocol->association.contexts.first().acceptedSyntax.isEmpty());
        QCOMPARE(result.protocol->messages.size(), 5);
        QVERIFY(result.protocol->messages.first().dataset);
        const auto &identifier = result.protocol->messages.first().dataset->attributes;
        QVERIFY(!findAttribute(identifier, "(0008,0052)"));
        QVERIFY(!findAttribute(identifier, "(0008,0060)"));
        const auto *requestStep = findAttribute(identifier, "(0040,0100)");
        QVERIFY(requestStep);
        QCOMPARE(requestStep->children.size(), 1);
        QVERIFY(findAttribute(requestStep->children.first().children, "(0008,0060)"));

        QStringList names;
        for (const auto &match : matches)
            names.append(value(match.dataset.attributes, "(0010,0010)"));
        QVERIFY(names.contains(QString::fromUtf8("B\xC3\xA9langer^Phantom")));
        QVERIFY(names.contains(QString::fromUtf8("Garc\xC3\xAD" "a^Phantom")));

        WorklistRequest request{"20261101-20261104", "CT"};
        result = query(peer.port, request, matches);
        QCOMPARE(result.status, Status::Succeeded);
        QCOMPARE(matches.size(), 1);
        const auto &root = matches.first().dataset.attributes;
        QCOMPARE(value(root, "(0010,0020)"), QString("PAT-1"));
        const auto *sequence = findAttribute(root, "(0040,0100)");
        QVERIFY(sequence);
        QCOMPARE(sequence->kind, Attribute::Sequence);
        QCOMPARE(sequence->children.size(), 1);
        const auto &step = sequence->children.first().children;
        QCOMPARE(value(step, "(0040,0002)"), QString("20261103"));
        QCOMPARE(value(step, "(0040,0003)"), QString("093015.25"));
        QCOMPARE(value(step, "(0040,0001)"), QString("SCANNER_CT"));
        QCOMPARE(value(step, "(0040,0007)"), QString::fromUtf8("R\xC3\xA9vision de fant\xC3\xB4me"));
        const auto *codes = findAttribute(step, "(0040,0008)");
        QVERIFY(codes);
        QCOMPARE(codes->children.size(), 1);
        QCOMPARE(value(codes->children.first().children, "(0008,0104)"), QString("Phantom protocol"));

        request = {"20261119", "CT", "SCANNER_CT2", "PAT-3", "Phantom^Gamma"};
        result = query(peer.port, request, matches);
        QCOMPARE(result.status, Status::Succeeded);
        QCOMPARE(matches.size(), 1);
        QCOMPARE(value(matches.first().dataset.attributes, "(0008,0050)"), QString("ACC-3"));
        request = {};
        request.patientName = QString::fromUtf8("Garc\xC3\xAD" "a^Phantom");
        result = query(peer.port, request, matches);
        QCOMPARE(result.status, Status::Succeeded);
        QCOMPARE(matches.size(), 1);
        QCOMPARE(value(matches.first().dataset.attributes, "(0010,0020)"), QString("PAT-2"));
        QVERIFY(result.protocol->messages.first().dataset);
        QCOMPARE(value(result.protocol->messages.first().dataset->attributes, "(0008,0005)"), QString("ISO_IR 192"));

        request.stationAe = "UNKNOWN";
        result = query(peer.port, request, matches);
        QCOMPARE(result.status, Status::Succeeded);
        QCOMPARE(result.dicomStatus.value_or(1), 0);
        QVERIFY(matches.isEmpty());

        const auto profilePath = peer.files.filePath("profiles.json");
        profiles::Store profiles(profilePath);
        auto target = destination(peer.port);
        target.id = 0;
        QVERIFY(profiles.save(profiles::Kind::Destination, target).isEmpty());
        logging::Logger logger;
        MainWindow window(logger, false, profilePath);
        window.show();
        window.findChild<QPushButton *>("nav5")->click();
        window.findChild<QLineEdit *>("worklistModality")->setText("CT");
        window.findChild<QLineEdit *>("worklistDate")->setText("20261103");
        window.findChild<QPushButton *>("sendWorklist")->click();
        auto *summary = window.findChild<QLabel *>("worklistSummary");
        QTRY_VERIFY_WITH_TIMEOUT(summary->text().contains("Query complete"), 5000);
        QVERIFY(summary->text().contains("0x0000"));
        QVERIFY(summary->text().contains(" ms"));
        auto *table = window.findChild<QTableWidget *>("worklistResults");
        QCOMPARE(table->rowCount(), 1);
        QCOMPARE(table->item(0, 0)->text(), QString("2026-11-03 09:30:15.25"));
        QCOMPARE(table->item(0, 3)->text(), QString("STEP-1"));
        QCOMPARE(table->item(0, 5)->text(), QString::fromUtf8("B\xC3\xA9langer^Phantom"));
        table->selectRow(0);
        auto *inspect = window.findChild<QPushButton *>("inspectWorklistDataset");
        QVERIFY(inspect->isEnabled());
        bool expandableSequence = false;
        QTimer::singleShot(0, &window, [&] {
            if (auto *dialog = window.findChild<QDialog *>("dicomDatasetInspector")) {
                auto *tree = dialog->findChild<QTreeView *>("datasetTree");
                const auto *model = tree->model();
                for (int row = 0; row < model->rowCount(); ++row) {
                    const auto index = model->index(row, 0);
                    if (index.data().toString() == "(0040,0100)") {
                        tree->expand(index);
                        expandableSequence = tree->isExpanded(index) && model->rowCount(index) == 1;
                    }
                }
                dialog->accept();
            }
        });
        inspect->click();
        QVERIFY(expandableSequence);
        QVERIFY(window.findChild<QPushButton *>("worklistProtocolDetails")->isEnabled());
        bool negotiated = false;
        for (const auto &event : logger.snapshot()) {
            if (event.component != "Worklist")
                continue;
            QVERIFY(!event.message.contains("PAT-1") && !event.details.contains("PAT-1"));
            negotiated |= !event.synthetic && event.message.contains("Presentation context");
        }
        QVERIFY(negotiated);
        window.findChild<QLineEdit *>("worklistStation")->setText("UNKNOWN");
        window.findChild<QPushButton *>("sendWorklist")->click();
        QTRY_VERIFY_WITH_TIMEOUT(summary->text().contains("No worklist items"), 5000);
        QCOMPARE(table->rowCount(), 0);
        QVERIFY(window.close());
    }

    void nativeTimeoutAndCancellation()
    {
        const auto executable = qEnvironmentVariable("DINGOBENCH_WLMSCPFS");
        if (executable.isEmpty())
            QSKIP("Set DINGOBENCH_WLMSCPFS to a local DCMTK wlmscpfs executable.");
        for (const bool cancel : {false, true}) {
            Fixture peer;
            const auto started = peer.start(executable, {"--sleep-before", "3"});
            QVERIFY2(started, peer.process.readAll().constData());
            QElapsedTimer clock;
            clock.start();
            std::stop_source stop;
            const auto result = worklistOperation(source(), destination(peer.port), {}, {2, 2, 1}, true, {})(
                stop.get_token(), [&](DiagnosticEvent event) {
                    if (cancel && event.message.startsWith("Sent C-FIND"))
                        stop.request_stop();
                });
            QVERIFY(clock.elapsed() < 2600);
            QVERIFY(result.protocol);
            QVERIFY(!result.protocol->messages.isEmpty());
            if (cancel) {
                QCOMPARE(result.status, Status::Cancelled);
                QCOMPARE(result.protocol->messages.size(), 2);
                QVERIFY(result.protocol->messages.last().command.contains("C-CANCEL"));
            } else {
                QCOMPARE(result.status, Status::Failed);
                QVERIFY(result.error);
                QCOMPARE(result.error->code, ErrorCode::DimseTimeout);
            }
        }
    }
    void generatesWorklistSamples()
    {
        QTemporaryDir fixture(m_runtime.filePath("generate-XXXXXX"));
        QVERIFY(fixture.isValid());
        const QDir files(fixture.path());
        logging::Logger logger;
        MainWindow window(logger, false, fixture.filePath("profiles.json"));
        window.show();
        window.findChild<QPushButton *>("nav5")->click();
        window.findChild<QTabWidget *>("worklistTabs")->setCurrentIndex(1);
        auto *generate = window.findChild<QPushButton *>("worklistServerGenerate");
        auto *directory = window.findChild<QLineEdit *>("worklistServerDirectory");
        auto *status = window.findChild<QLabel *>("worklistServerStatus");
        auto *count = window.findChild<QLabel *>("worklistServerRecordCount");
        const auto originalCount = count->text();
        bool pickerOpened = false;
        QTimer::singleShot(0, &window, [&] {
            if (auto *dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget())) {
                pickerOpened = true;
                dialog->reject();
            }
        });
        generate->click();
        QVERIFY(pickerOpened);
        QVERIFY(directory->text().isEmpty());
        QCOMPARE(count->text(), originalCount);
        QVERIFY(files.entryList({"*.wl"}, QDir::Files).isEmpty());

        const auto today = QDate::currentDate();
        QTimer::singleShot(0, &window, [&] {
            if (auto *dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget())) {
                dialog->setDirectory(files);
                QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
            }
        });
        generate->click();
        QVERIFY(!generate->isEnabled());
        QTRY_COMPARE_WITH_TIMEOUT(count->text(), QString("50 available · 0 rejected"), 5000);
        QCOMPARE(QDir(directory->text()).canonicalPath(), files.canonicalPath());
        QTRY_VERIFY(generate->isEnabled());
        QVERIFY(status->text().contains("Added 50"));
        QVERIFY(status->text().contains("server stopped"));

        const auto first = files.entryList({"*.wl"}, QDir::Files);
        QCOMPARE(first.size(), 50);
        QHash<QString, QByteArray> originalFiles;
        for (const auto &name : first)
            originalFiles.insert(name, digest(files.filePath(name)));
        generate->click();
        QTRY_COMPARE_WITH_TIMEOUT(count->text(), QString("100 available · 0 rejected"), 5000);
        const auto all = files.entryList({"*.wl"}, QDir::Files);
        QCOMPARE(all.size(), 100);
        QMap<QString, int> dates;
        QMap<QString, QSet<QString>> modalities;
        QSet<QString> studyUids, patientIds, stepIds, times;
        for (const auto &name : all) {
            const auto path = files.filePath(name);
            if (originalFiles.contains(name))
                QCOMPARE(digest(path), originalFiles.value(name));
            DcmFileFormat file;
            QVERIFY(file.loadFile(QFile::encodeName(path).constData()).good());
            const auto tag = [](DcmItem &item, const DcmTagKey &key) {
                OFString data;
                return item.findAndGetOFString(key, data).good() ? QString::fromLatin1(data.c_str()) : QString{};
            };
            auto &dataset = *file.getDataset();
            QVERIFY(tag(dataset, DCM_PatientName).startsWith("Synthetic^"));
            studyUids.insert(tag(dataset, DCM_StudyInstanceUID));
            patientIds.insert(tag(dataset, DCM_PatientID));
            DcmItem *step = nullptr;
            QVERIFY(dataset.findAndGetSequenceItem(DCM_ScheduledProcedureStepSequence, step).good());
            const auto date = tag(*step, DCM_ScheduledProcedureStepStartDate);
            const auto modality = tag(*step, DCM_Modality);
            QCOMPARE(tag(*step, DCM_ScheduledStationAETitle), "SCANNER_" + modality);
            stepIds.insert(tag(*step, DCM_ScheduledProcedureStepID));
            times.insert(tag(*step, DCM_ScheduledProcedureStepStartTime));
            ++dates[date];
            modalities[date].insert(modality);
        }
        QCOMPARE(studyUids.size(), 100);
        QCOMPARE(patientIds.size(), 100);
        QCOMPARE(stepIds.size(), 100);
        QCOMPARE(times.size(), 50);
        QCOMPARE(dates.size(), 3);
        QCOMPARE(dates.value(today.addDays(-1).toString("yyyyMMdd")), 34);
        QCOMPARE(dates.value(today.toString("yyyyMMdd")), 34);
        QCOMPARE(dates.value(today.addDays(1).toString("yyyyMMdd")), 32);
        for (const auto &set : modalities)
            QCOMPARE(set, (QSet<QString>{"CT", "MR", "US", "CR", "DX"}));
        const auto logged = logger.snapshot();
        QVERIFY(std::ranges::any_of(logged, [](const auto &event) {
            return event.synthetic && event.message.contains("Added 50 synthetic worklist records");
        }));
    }

    void guardsWorklistSampleGeneration()
    {
        QTemporaryDir fixture(m_runtime.filePath("generate-guards-XXXXXX"));
        QVERIFY(fixture.isValid());
        const QDir files(fixture.path());
        QVERIFY(files.mkdir("GitHub"));
        const auto checkout = files.filePath("GitHub");
        logging::Logger logger;
        WorklistServer server(logger);
        using State = WorklistServer::State;
        server.generateSamples(checkout);
        QTRY_COMPARE_WITH_TIMEOUT(server.state(), State::Error, 5000);
        QVERIFY(server.message().contains("outside Git checkouts"));
        QVERIFY(QDir(checkout).isEmpty());

        QLockFile lock(files.filePath(".dingobench-worklist-generation.lock"));
        QVERIFY(lock.tryLock());
        server.generateSamples(files.path());
        QTRY_COMPARE_WITH_TIMEOUT(server.state(), State::Error, 5000);
        QVERIFY(server.message().contains("already running"));
        QVERIFY(files.entryList({"*.wl"}, QDir::Files).isEmpty());
        lock.unlock();

        QCOMPARE(files.entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot), QStringList{"GitHub"});

        for (int i = 0; i < 451; ++i) {
            QFile entry(files.filePath(QString("existing-%1.wl").arg(i)));
            QVERIFY(entry.open(QIODevice::WriteOnly | QIODevice::NewOnly));
        }
        server.generateSamples(files.path());
        QTRY_COMPARE_WITH_TIMEOUT(server.state(), State::Error, 5000);
        QVERIFY(server.message().contains("at most 450"));
        QCOMPARE(files.entryList({"*.wl"}, QDir::Files).size(), 451);
        QVERIFY(QFile::remove(files.filePath("existing-450.wl")));
        server.generateSamples(files.path());
        QTRY_COMPARE_WITH_TIMEOUT(server.state(), State::Stopped, 5000);
        QCOMPARE(files.entryList({"*.wl"}, QDir::Files).size(), 500);
        QCOMPARE(std::ranges::count_if(server.records(), [](const auto &record) { return record.issue.isEmpty(); }), 50);
        QCOMPARE(files.entryList(QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot), QStringList{"GitHub"});
    }

    void rollsBackWorklistSampleConflict()
    {
        QTemporaryDir destination(m_runtime.filePath("generate-conflict-XXXXXX"));
        QTemporaryDir staging(m_runtime.filePath("generate-stage-XXXXXX"));
        QVERIFY(destination.isValid());
        QVERIFY(staging.isValid());
        const QStringList names{"first.wl", "conflict.wl", "last.wl"};
        for (const auto &name : names) {
            QFile file(staging.filePath(name));
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::NewOnly));
            QCOMPARE(file.write("synthetic"), 9);
        }
        QFile existing(destination.filePath("conflict.wl"));
        QVERIFY(existing.open(QIODevice::WriteOnly | QIODevice::NewOnly));
        QCOMPARE(existing.write("original"), 8);
        existing.close();
        const auto expected = digest(existing.fileName());

        std::stop_source cancel;
        cancel.request_stop();
        auto result = detail::publishWorklistSampleFiles(QDir(staging.path()), QDir(destination.path()),
                                                         names, "test", cancel.get_token());
        QCOMPARE(result.status, Status::Cancelled);
        QCOMPARE(QDir(staging.path()).entryList({"*.wl"}, QDir::Files).size(), 3);

        result = detail::publishWorklistSampleFiles(QDir(staging.path()), QDir(destination.path()),
                                                    names, "test", {});
        QCOMPARE(result.status, Status::Failed);
        QCOMPARE(QDir(destination.path()).entryList({"*.wl"}, QDir::Files), QStringList{"conflict.wl"});
        QCOMPARE(digest(existing.fileName()), expected);
        QVERIFY(!QFileInfo::exists(staging.filePath("first.wl")));
        QVERIFY(QFileInfo::exists(staging.filePath("last.wl")));
    }

    void queriesGeneratedWorklist()
    {
        const auto executable = qEnvironmentVariable("DINGOBENCH_WLMSCPFS");
        if (executable.isEmpty())
            QSKIP("Set DINGOBENCH_WLMSCPFS to a local DCMTK executable.");
        QTemporaryDir fixture(m_runtime.filePath("generate-query-XXXXXX"));
        QVERIFY(fixture.isValid());
        QTcpServer reserve;
        QVERIFY(reserve.listen(QHostAddress::AnyIPv4, 0));
        const auto port = reserve.serverPort();
        reserve.close();

        const auto profilePath = fixture.filePath("profiles.json");
        profiles::Store profiles(profilePath);
        auto peer = destination(port);
        peer.id = 0;
        QVERIFY(profiles.save(profiles::Kind::Source, {0, "Sample MWL Server", "TEST_MWL", {}, port, {}}).isEmpty());
        QVERIFY(profiles.save(profiles::Kind::Destination, peer).isEmpty());
        logging::Logger logger;
        MainWindow window(logger, false, profilePath);
        window.show();
        window.findChild<QPushButton *>("nav5")->click();
        auto *tabs = window.findChild<QTabWidget *>("worklistTabs");
        tabs->setCurrentIndex(1);
        window.findChild<QLineEdit *>("worklistServerDirectory")->setText(fixture.path());
        window.findChild<QLineEdit *>("worklistServerExecutable")->setText(executable);
        auto *generate = window.findChild<QPushButton *>("worklistServerGenerate");
        const auto today = QDate::currentDate().toString("yyyyMMdd");
        generate->click();
        auto *count = window.findChild<QLabel *>("worklistServerRecordCount");
        QTRY_COMPARE_WITH_TIMEOUT(count->text(), QString("50 available · 0 rejected"), 5000);
        QTRY_VERIFY(generate->isEnabled());
        window.findChild<QCheckBox *>("worklistServerAllowWildcard")->setChecked(true);
        window.findChild<QPushButton *>("worklistServerStart")->click();
        auto *status = window.findChild<QLabel *>("worklistServerStatus");
        QTRY_VERIFY_WITH_TIMEOUT(status->text().contains("Listening as TEST_MWL") ||
                                 status->property("tone") == "error", 7000);
        QVERIFY2(status->text().contains("Listening as TEST_MWL"), qPrintable(status->text()));
        QVERIFY(!generate->isEnabled());
        tabs->setCurrentIndex(0);
        window.findChild<QLineEdit *>("worklistDate")->setText(today);
        window.findChild<QLineEdit *>("worklistModality")->setText("CT");
        window.findChild<QLineEdit *>("worklistStation")->setText("SCANNER_CT");
        window.findChild<QPushButton *>("sendWorklist")->click();
        auto *summary = window.findChild<QLabel *>("worklistSummary");
        QTRY_VERIFY_WITH_TIMEOUT(summary->text().contains("Query complete"), 5000);
        QVERIFY(summary->text().contains("0x0000"));
        auto *table = window.findChild<QTableWidget *>("worklistResults");
        QCOMPARE(table->rowCount(), 3);
        for (int row = 0; row < table->rowCount(); ++row) {
            QCOMPARE(table->item(row, 1)->text(), QString("CT"));
            QCOMPARE(table->item(row, 2)->text(), QString("SCANNER_CT"));
            QVERIFY(table->item(row, 4)->text().startsWith("SAMPLE-"));
        }
        tabs->setCurrentIndex(1);
        window.findChild<QPushButton *>("worklistServerStop")->click();
        QTRY_VERIFY_WITH_TIMEOUT(generate->isEnabled(), 5000);
    }

    void managedServerLifecycle()
    {
        const auto executable = qEnvironmentVariable("DINGOBENCH_WLMSCPFS");
        const auto findscu = qEnvironmentVariable("DINGOBENCH_FINDSCU");
        if (executable.isEmpty() || findscu.isEmpty())
            QSKIP("Set DINGOBENCH_WLMSCPFS and DINGOBENCH_FINDSCU to local DCMTK executables.");
        QTemporaryDir fixture(QDir(QCoreApplication::applicationDirPath()).filePath("mwl-server-XXXXXX"));
        QVERIFY(fixture.isValid());
        QVERIFY(QDir(fixture.path()).mkdir("records"));
        QVERIFY(QDir(fixture.path()).mkdir("extracted"));
        const QDir files(fixture.filePath("records"));
        const auto ct = files.filePath("one.wl"), mr = files.filePath("two.wl");
        QVERIFY(writeRecord(ct, 1, "ISO_IR 100", "B\xE9langer^Phantom", "CT", "20261103", "SCANNER_CT"));
        QVERIFY(writeRecord(mr, 2, "ISO_IR 192", "Phantom^Beta", "MR", "20261119", "SCANNER_MR"));
        const auto original = digest(ct);
        QVERIFY(!original.isEmpty());
        QFile corrupt(files.filePath("broken.wl"));
        QVERIFY(corrupt.open(QIODevice::WriteOnly));
        QVERIFY(corrupt.write("invalid test record") > 0);
        corrupt.close();
        DcmFileFormat incomplete;
        QVERIFY(incomplete.getDataset()->putAndInsertString(DCM_PatientID, "SYNTH-INCOMPLETE").good());
        QVERIFY(incomplete.saveFile(OFFilename(QFile::encodeName(files.filePath("incomplete.wl")).constData()),
            EXS_LittleEndianExplicit, EET_ExplicitLength, EGL_recalcGL, EPD_withoutPadding, 0, 0, EWM_dataset).good());

        logging::Logger logger;
        WorklistServer server(logger);
        QSignalSpy activity(&server, &WorklistServer::activity);
        QSignalSpy sessions(&server, &WorklistServer::sessionFinished);
        using State = WorklistServer::State;
        server.refreshRecords(files.path());
        QTRY_COMPARE_WITH_TIMEOUT(server.state(), State::Stopped, 5000);
        QCOMPARE(server.records().size(), 4);
        QCOMPARE(std::ranges::count_if(server.records(), [](const auto &record) { return record.issue.isEmpty(); }), 2);
        QVERIFY(!server.records().at(0).issue.isEmpty());
        QVERIFY(!server.records().at(1).issue.isEmpty());
        QVERIFY(!QFileInfo::exists(files.filePath("lockfile")));

        QTcpServer occupied;
        QVERIFY(occupied.listen(QHostAddress::AnyIPv4, 0));
        const int port = occupied.serverPort();
        const profiles::Profile profile{1, "Managed MWL", "TEST_MWL", "127.0.0.1", port, {}};
        WorklistServerSettings settings{profile, files.path(), executable, port, false, false};
        server.start(settings);
        QCOMPARE(server.state(), State::Error);
        QVERIFY(server.message().contains("all interfaces"));
        settings.allowWildcard = true;
        server.start(settings);
        QTRY_COMPARE_WITH_TIMEOUT(server.state(), State::Error, 5000);
        QVERIFY(server.message().contains("Cannot bind worklist port"));
        QCOMPARE(server.processId(), 0);
        occupied.close();

        const auto before = stages();
        server.start(settings);
        QTRY_VERIFY_WITH_TIMEOUT(server.state() == State::Listening || server.state() == State::Error, 7000);
        QVERIFY2(server.state() == State::Listening, qPrintable(server.message()));
        QVERIFY(server.processId() > 0);
        QCOMPARE(server.records().size(), 4);
        const auto running = stages();
        QStringList added;
        for (const auto &stage : running)
            if (!before.contains(stage))
                added.append(stage);
        QCOMPARE(added.size(), 1);
        const auto staged = QDir(QDir::tempPath()).filePath(added.first());
        QCOMPARE(QDir(QDir(staged).filePath("TEST_MWL")).entryList({"*.wl"}, QDir::Files).size(), 2);

        const auto request = fixture.filePath("query.dcm");
        QVERIFY(writeFindRequest(request));
        QProcess external;
        external.setProcessChannelMode(QProcess::MergedChannels);
        external.start(findscu, {"-W", "-X", "-sr", "-od", fixture.filePath("extracted"),
            "-to", "3", "-ta", "3", "-td", "3", "-aet", "FIND_SCU", "-aec", "TEST_MWL",
            "127.0.0.1", QString::number(port), request});
        QVERIFY(external.waitForFinished(7000));
        QVERIFY2(external.exitStatus() == QProcess::NormalExit && external.exitCode() == 0, external.readAll().constData());
        const QDir responses(fixture.filePath("extracted"));
        const auto names = responses.entryList({"*.dcm"}, QDir::Files);
        QCOMPARE(names.size(), 1);
        DcmFileFormat returned;
        QVERIFY(returned.loadFile(QFile::encodeName(responses.filePath(names.first())).constData()).good());
        OFString patient;
        QVERIFY(returned.getDataset()->findAndGetOFString(DCM_PatientID, patient).good());
        QCOMPARE(QString::fromLatin1(patient.c_str()), QString("PAT-1"));
        QTRY_COMPARE(sessions.size(), 1);
        const auto inbound = sessions.first().first().value<DiagnosticSession>();
        QCOMPARE(inbound.captureSource, QString("Utility diagnostics"));
        QCOMPARE(inbound.association.callingAe, QString("FIND_SCU"));
        QVERIFY(inbound.association.contexts.isEmpty());
        QCOMPARE(inbound.status.value_or(1), quint16(0));
        QCOMPARE(inbound.counts.value("Pending responses"), 1);
        verifyDiagnosticReport(inbound, "worklist-scp-success");

        auto wrong = destination(port);
        wrong.ae = "WRONG_AE";
        const auto rejected = worklistOperation(source(), wrong, {}, {2, 2, 2}, false, {})(
            {}, [](DiagnosticEvent) {});
        QVERIFY(rejected.error);
        QCOMPARE(rejected.error->code, ErrorCode::AssociationRejected);

        QList<QueryMatch> matches;
        auto result = query(port, {"20261115-20261120", "MR"}, matches);
        QVERIFY2(result.status == Status::Succeeded, qPrintable(result.error ? result.error->message : result.details));
        QCOMPARE(matches.size(), 1);
        QCOMPARE(value(matches.first().dataset.attributes, "(0010,0020)"), QString("PAT-2"));
        result = query(port, {"20261103", "MR"}, matches);
        QCOMPARE(result.status, Status::Succeeded);
        QVERIFY(matches.isEmpty());
        QTRY_VERIFY(std::ranges::any_of(activity, [](const auto &row) { return row.first() == "C-FIND complete"; }));
        for (const auto &event : logger.snapshot())
            if (event.component == "Worklist Server")
                QVERIFY(!event.message.contains("PAT-") && !event.details.contains("Phantom"));

        // The original directory can change while the running snapshot stays stable.
        QVERIFY(writeRecord(files.filePath("three.wl"), 3, "ISO_IR 192", "Phantom^Gamma",
                            "CT", "20261103", "SCANNER_CT"));
        result = query(port, {"20261103", "CT"}, matches);
        QCOMPARE(result.status, Status::Succeeded);
        QCOMPARE(matches.size(), 1);
        server.stop();
        QTRY_COMPARE_WITH_TIMEOUT(server.state(), State::Stopped, 5000);
        QCOMPARE(server.processId(), 0);
        QVERIFY(!QFileInfo::exists(staged));
        QCOMPARE(digest(ct), original);
        QVERIFY(!QFileInfo::exists(files.filePath("lockfile")));
        QVERIFY(occupied.listen(QHostAddress::AnyIPv4, quint16(port)));
        occupied.close();

        server.refreshRecords(files.path());
        QTRY_COMPARE_WITH_TIMEOUT(server.state(), State::Stopped, 5000);
        QCOMPARE(std::ranges::count_if(server.records(), [](const auto &record) { return record.issue.isEmpty(); }), 3);
        server.start(settings);
        QTRY_VERIFY_WITH_TIMEOUT(server.state() == State::Listening || server.state() == State::Error, 7000);
        QVERIFY2(server.state() == State::Listening, qPrintable(server.message()));
        result = query(port, {"20261103", "CT"}, matches);
        QCOMPARE(result.status, Status::Succeeded);
        QCOMPARE(matches.size(), 2);
        QVERIFY(killServer(server.processId()));
        QTRY_COMPARE_WITH_TIMEOUT(server.state(), State::Error, 5000);
        QVERIFY(server.message().contains("exited unexpectedly"));
        QCOMPARE(server.processId(), 0);
        QCOMPARE(stages(), before);
        QVERIFY(occupied.listen(QHostAddress::AnyIPv4, quint16(port)));
    }

    void managedServerStopsOnWindowClose()
    {
        const auto executable = qEnvironmentVariable("DINGOBENCH_WLMSCPFS");
        if (executable.isEmpty())
            QSKIP("Set DINGOBENCH_WLMSCPFS to a local DCMTK executable.");
        QTemporaryDir fixture(QDir(QCoreApplication::applicationDirPath()).filePath("mwl-server-ui-XXXXXX"));
        QVERIFY(fixture.isValid());
        QVERIFY(writeRecord(fixture.filePath("one.wl"), 1, "ISO_IR 192", "Phantom^UI",
                            "CT", "20261103", "SCANNER_CT"));
        QTcpServer reserve;
        QVERIFY(reserve.listen(QHostAddress::AnyIPv4, 0));
        const auto port = reserve.serverPort();
        reserve.close();

        const auto profilePath = fixture.filePath("profiles.json");
        profiles::Store profiles(profilePath);
        QVERIFY(profiles.save(profiles::Kind::Source, {0, "Managed Worklist UI", "TEST_MWL", {}, port, {}}).isEmpty());
        const auto before = stages();
        logging::Logger logger;
        MainWindow window(logger, false, profilePath);
        window.show();
        window.findChild<QPushButton *>("nav5")->click();
        window.findChild<QTabWidget *>("worklistTabs")->setCurrentIndex(1);
        QCOMPARE(window.findChild<QSpinBox *>("worklistServerPort")->value(), port);
        window.findChild<QLineEdit *>("worklistServerDirectory")->setText(fixture.path());
        window.findChild<QLineEdit *>("worklistServerExecutable")->setText(executable);
        auto *start = window.findChild<QPushButton *>("worklistServerStart");
        QVERIFY(!start->isEnabled());
        window.findChild<QCheckBox *>("worklistServerAllowWildcard")->setChecked(true);
        QVERIFY(start->isEnabled());
        start->click();
        auto *status = window.findChild<QLabel *>("worklistServerStatus");
        QTRY_VERIFY_WITH_TIMEOUT(status->text().contains("Listening as TEST_MWL") ||
                                 status->property("tone") == "error", 7000);
        QVERIFY2(status->text().contains("Listening as TEST_MWL"), qPrintable(status->text()));
        QCOMPARE(window.findChild<QLabel *>("worklistServerRecordCount")->text(), QString("1 available · 0 rejected"));
        QVERIFY(!window.close());
        QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 5000);
        QCOMPARE(stages(), before);
        QVERIFY(reserve.listen(QHostAddress::AnyIPv4, port));
    }

    void managedServerIpv6()
    {
        const auto executable = qEnvironmentVariable("DINGOBENCH_WLMSCPFS");
        if (executable.isEmpty())
            QSKIP("Set DINGOBENCH_WLMSCPFS to a local DCMTK executable.");
        QTcpServer reserve;
        if (!reserve.listen(QHostAddress::AnyIPv6, 0))
            QSKIP("IPv6 is unavailable on this host.");
        const int port = reserve.serverPort();
        reserve.close();
        QTemporaryDir fixture(QDir(QCoreApplication::applicationDirPath()).filePath("mwl-server-v6-XXXXXX"));
        QVERIFY(fixture.isValid());
        QVERIFY(writeRecord(fixture.filePath("one.wl"), 1, "ISO_IR 192", "Phantom^IPv6", "CT", "20261103", "SCANNER_CT"));
        logging::Logger logger;
        WorklistServer server(logger);
        const profiles::Profile profile{1, "IPv6 MWL", "TEST_MWL", "::1", port, {}};
        using State = WorklistServer::State;
        server.start({profile, fixture.path(), executable, port, true, true});
        QTRY_VERIFY_WITH_TIMEOUT(server.state() == State::Listening || server.state() == State::Error, 7000);
        QVERIFY2(server.state() == State::Listening, qPrintable(server.message()));
        QVERIFY(server.message().contains("IPv6"));
        auto peer = destination(port);
        peer.address = "::1";
        QList<QueryMatch> matches;
        const auto result = worklistOperation(source(), peer, {"20261103", "CT"}, {2, 2, 2}, false,
            [&](QueryMatch match) { matches.append(std::move(match)); })({}, [](DiagnosticEvent) {});
        QVERIFY2(result.status == Status::Succeeded, qPrintable(result.error ? result.error->message : result.details));
        QCOMPARE(matches.size(), 1);
        server.stop();
        QTRY_COMPARE_WITH_TIMEOUT(server.state(), State::Stopped, 5000);
        QVERIFY(reserve.listen(QHostAddress::AnyIPv6, quint16(port)));
    }

};

QTEST_MAIN(WorklistFindTest)
#include "WorklistFindTest.moc"
