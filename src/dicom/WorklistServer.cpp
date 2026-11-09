// SPDX-License-Identifier: GPL-3.0-only
#include "dingobench/dicom/WorklistServer.h"
#include "dingobench/dicom/StorageReceive.h"
#include "dingobench/dicom/Verification.h"
#include "WorklistSampleFiles.h"

#include <dcmtk/config/osconfig.h>
#include <dcmtk/dcmdata/dcdeftag.h>
#include <dcmtk/dcmdata/dcfilefo.h>
#include <dcmtk/dcmdata/dcistrmb.h>
#include <dcmtk/dcmdata/dcostrmb.h>
#include <dcmtk/dcmdata/dcsequen.h>
#include <dcmtk/dcmdata/dcuid.h>
#include <dcmtk/dcmdata/dcvrcs.h>
#include <dcmtk/dcmdata/dcvrda.h>
#include <dcmtk/dcmdata/dcvrtm.h>
#include <dcmtk/dcmdata/dcvrui.h>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QLockFile>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QThread>

#include <algorithm>
#include <array>
#include <utility>

namespace dingobench::dicom {
namespace {

using logging::Severity;
constexpr auto component = "Worklist Server";
constexpr auto probeAe = "DINGO_MWL_PROBE";
constexpr int maxRecords = 500;
constexpr int sampleCount = 50;
constexpr qint64 maxRecordBytes = 1024 * 1024;

QString readValue(DcmItem &item, const DcmTagKey &tag)
{
    OFString text;
    return item.findAndGetOFStringArray(tag, text).good()
        ? QString::fromLatin1(text.c_str()).trimmed() : QString{};
}

bool completeSequence(DcmItem &item, const DcmTagKey &tag, const DcmTagKey &first,
                      const DcmTagKey &second, bool allowEmpty)
{
    if (!item.tagExists(tag))
        return true;
    DcmSequenceOfItems *sequence = nullptr;
    if (item.findAndGetSequence(tag, sequence).bad() || (!allowEmpty && sequence->card() == 0))
        return false;
    for (unsigned long index = 0; index < sequence->card(); ++index) {
        auto &child = *sequence->getItem(index);
        if (readValue(child, first).isEmpty() || readValue(child, second).isEmpty())
            return false;
    }
    return true;
}

bool hasProcedure(DcmItem &item, const DcmTagKey &description, const DcmTagKey &sequence)
{
    if (item.tagExists(description) && readValue(item, description).isEmpty())
        return false;
    return (item.tagExists(description) || item.tagExists(sequence)) &&
        completeSequence(item, sequence, DCM_CodeValue, DCM_CodingSchemeDesignator, false);
}

QString inspectRecord(const QByteArray &data, WorklistRecord &record)
{
    DcmInputBufferStream stream;
    stream.setBuffer(data.constData(), data.size());
    stream.setEos();
    DcmFileFormat file;
    file.transferInit();
    const auto parsed = file.read(stream, EXS_Unknown, EGL_noChange, Uint32(maxRecordBytes));
    file.transferEnd();
    if (parsed.bad())
        return "Cannot read the DICOM dataset.";
    auto &dataset = *file.getDataset();
    DcmSequenceOfItems *steps = nullptr;
    if (dataset.findAndGetSequence(DCM_ScheduledProcedureStepSequence, steps).bad() || steps->card() != 1)
        return "Expected one Scheduled Procedure Step Sequence item.";
    auto &step = *steps->getItem(0);
    record.station = readValue(step, DCM_ScheduledStationAETitle);
    record.modality = readValue(step, DCM_Modality);
    record.date = readValue(step, DCM_ScheduledProcedureStepStartDate);
    const auto time = readValue(step, DCM_ScheduledProcedureStepStartTime);
    if (record.station.isEmpty() || record.modality.isEmpty() || record.date.isEmpty() || time.isEmpty() ||
        readValue(step, DCM_ScheduledProcedureStepID).isEmpty())
        return "Missing station AE, modality, start date/time, or step ID.";
    for (const auto &station : record.station.split(u'\\'))
        if (!profiles::validAeTitle(station))
            return "Invalid Scheduled Station AE Title.";
    if (DcmCodeString::checkStringValue(record.modality.toLatin1().constData(), "1").bad())
        return "Invalid modality code.";
    if (!QDate::fromString(record.date, "yyyyMMdd").isValid() ||
        DcmDate::checkStringValue(record.date.toLatin1().constData(), "1").bad() ||
        DcmTime::checkStringValue(time.toLatin1().constData(), "1").bad())
        return "Invalid Scheduled Procedure Step start date or time.";
    if (readValue(dataset, DCM_PatientName).isEmpty() || readValue(dataset, DCM_PatientID).isEmpty() ||
        readValue(dataset, DCM_RequestedProcedureID).isEmpty())
        return "Missing Patient Name, Patient ID, or Requested Procedure ID.";
    const auto study = readValue(dataset, DCM_StudyInstanceUID);
    if (study.isEmpty() || DcmUniqueIdentifier::checkStringValue(study.toLatin1().constData(), "1").bad())
        return "Missing or invalid Study Instance UID.";
    if (!hasProcedure(step, DCM_ScheduledProcedureStepDescription, DCM_ScheduledProtocolCodeSequence) ||
        !hasProcedure(dataset, DCM_RequestedProcedureDescription, DCM_RequestedProcedureCodeSequence))
        return "Missing or incomplete scheduled or requested procedure description/code.";
    for (const auto &tag : {DCM_ReferencedStudySequence, DCM_ReferencedPatientSequence})
        if (!completeSequence(dataset, tag, DCM_ReferencedSOPClassUID, DCM_ReferencedSOPInstanceUID, true))
            return "Incomplete referenced study or patient sequence.";
    return {};
}

QFileInfoList worklistFiles(const QDir &directory)
{
    return directory.entryInfoList({"*.wl"}, QDir::Files, QDir::Name | QDir::IgnoreCase);
}

QByteArray sampleRecord(const QByteArray &id, const QByteArray &modality, QDate date, QTime time)
{
    DcmDataset dataset;
    OFCondition condition = EC_Normal;
    const auto add = [&](DcmItem &item, const DcmTagKey &tag, const QByteArray &value) {
        if (condition.good())
            condition = item.putAndInsertString(tag, value.constData());
    };
    char studyUid[65];
    add(dataset, DCM_PatientName, "Synthetic^Worklist^" + id);
    add(dataset, DCM_PatientID, "SAMPLE-" + id);
    add(dataset, DCM_AccessionNumber, "A" + id);
    add(dataset, DCM_StudyInstanceUID, dcmGenerateUniqueIdentifier(studyUid, SITE_STUDY_UID_ROOT));
    add(dataset, DCM_RequestedProcedureID, "R" + id);
    add(dataset, DCM_RequestedProcedureDescription, "Synthetic phantom calibration");
    if (condition.good())
        condition = dataset.insertEmptyElement(DCM_ReferencedStudySequence);
    if (condition.good())
        condition = dataset.insertEmptyElement(DCM_ReferencedPatientSequence);
    DcmItem *step = nullptr;
    if (condition.good())
        condition = dataset.findOrCreateSequenceItem(DCM_ScheduledProcedureStepSequence, step, 0);
    if (condition.bad())
        return {};
    add(*step, DCM_ScheduledStationAETitle, "SCANNER_" + modality);
    add(*step, DCM_ScheduledProcedureStepStartDate, date.toString("yyyyMMdd").toLatin1());
    add(*step, DCM_ScheduledProcedureStepStartTime, time.toString("HHmmss").toLatin1());
    add(*step, DCM_Modality, modality);
    add(*step, DCM_ScheduledProcedureStepDescription, "Synthetic " + modality + " phantom scan");
    add(*step, DCM_ScheduledProcedureStepID, "S" + id);
    if (condition.bad())
        return {};

    std::array<char, 4096> buffer{};
    DcmOutputBufferStream output(buffer.data(), buffer.size());
    dataset.transferInit();
    condition = dataset.write(output, EXS_LittleEndianExplicit, EET_ExplicitLength, nullptr);
    dataset.transferEnd();
    return condition.good() ? QByteArray(buffer.data(), qsizetype(output.filled())) : QByteArray{};
}

OperationResult appendSamples(const QDir &destination, QDate today, std::stop_token stop)
{
    const auto fail = [](const QString &message) { return OperationResult::failed({ErrorCode::StorageFailure, message}); };
    QLockFile lock(destination.filePath(".dingobench-worklist-generation.lock"));
    if (!lock.tryLock())
        return fail(lock.error() == QLockFile::LockFailedError
            ? "Sample generation is already running in this folder."
            : "Cannot lock the worklist folder. Choose a writable folder.");

    const auto atCapacity = [&] { return worklistFiles(destination).size() > maxRecords - sampleCount; };
    const auto capacityError = [&] {
        return fail(QString("This folder must contain at most %1 .wl files before adding %2 samples.")
            .arg(maxRecords - sampleCount).arg(sampleCount));
    };
    if (atCapacity())
        return capacityError();

    QTemporaryDir staging(destination.filePath(".dingobench-samples-XXXXXX"));
    if (!staging.isValid())
        return fail("Cannot stage samples. Check the folder permissions and available disk space.");
    const auto batch = QUuid::createUuid().toString(QUuid::Id128);
    const QDir staged(staging.path());
    const std::array<QByteArray, 5> modalities{"CT", "MR", "US", "CR", "DX"};
    QStringList names;
    names.reserve(sampleCount);
    for (int index = 0; index < sampleCount; ++index) {
        if (stop.stop_requested())
            return OperationResult::cancelled();
        // Cycling the dates assigns 17 records to yesterday and today, and 16 to tomorrow.
        const auto date = today.addDays(index % 3 - 1);
        const auto time = QTime(8, 0).addSecs(index * 15 * 60);
        const auto ordinal = QString::number(index + 1).rightJustified(2, u'0');
        const auto id = (batch.first(13) + ordinal).toLatin1();
        const auto name = "sample-" + batch + "-" + ordinal + ".wl";
        const auto data = sampleRecord(id, modalities[index % modalities.size()], date, time);
        WorklistRecord record{name};
        if (data.isEmpty() || !inspectRecord(data, record).isEmpty())
            return fail("Could not build a valid synthetic worklist record.");
        QFile file(staged.filePath(name));
        if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
            file.write(data) != data.size() || !file.flush())
            return fail("Cannot stage samples. Check the folder permissions and available disk space.");
        names.append(name);
    }
    if (atCapacity())
        return capacityError();

    return detail::publishWorklistSampleFiles(staged, destination, names, batch, stop);
}

QString directoryAe(const QString &value)
{
    static const QRegularExpression safe(R"(\A[A-Za-z0-9_-]{1,16}\z)");
    static const QRegularExpression reserved(R"(\A(?:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])\z)",
                                              QRegularExpression::CaseInsensitiveOption);
    const auto title = value.trimmed();
    return safe.match(title).hasMatch() && !reserved.match(title).hasMatch() ? title : QString{};
}

OperationResult checkUtilityVersion(const QString &executable, std::stop_token stop)
{
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.setStandardInputFile(QProcess::nullDevice());
    process.start(executable, {"--version"});
    QElapsedTimer deadline;
    deadline.start();
    while (process.state() != QProcess::NotRunning && deadline.elapsed() < 2000 && !stop.stop_requested())
        process.waitForFinished(50);
    if (process.state() != QProcess::NotRunning) {
        process.kill();
        process.waitForFinished(1000);
    }
    if (stop.stop_requested())
        return OperationResult::cancelled();
    if (deadline.elapsed() >= 2000 || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0 ||
        !process.readAll().contains("wlmscpfs v3.7.1 "))
        return OperationResult::failed({ErrorCode::StartFailed,
            "The server requires the DCMTK 3.7.1 wlmscpfs executable. Its version could not be verified."});
    return {};
}

}  // namespace

OperationResult detail::publishWorklistSampleFiles(const QDir &staged, const QDir &destination,
                                                   const QStringList &names, const QString &batch,
                                                   std::stop_token stop)
{
    for (qsizetype index = 0; index < names.size(); ++index) {
        const auto &name = names.at(index);
        if (!stop.stop_requested() && QFile::rename(staged.filePath(name), destination.filePath(name)))
            continue;

        int remaining = 0;
        for (qsizetype appended = 0; appended < index; ++appended) {
            const auto path = destination.filePath(names.at(appended));
            if (!QFile::remove(path) && QFileInfo::exists(path))
                ++remaining;
        }
        if (remaining)
            return OperationResult::failed({ErrorCode::StorageFailure,
                QString("Could not finish adding samples or remove %1 file(s) from batch %2. "
                        "Check the folder before retrying.").arg(remaining).arg(batch)});
        return stop.stop_requested() ? OperationResult::cancelled()
            : OperationResult::failed({ErrorCode::StorageFailure,
                "Could not append the samples. Check the folder permissions, free space, and filename conflicts."});
    }
    return {};
}

struct WorklistServer::Scan {
    QList<WorklistRecord> records;
    std::unique_ptr<QTemporaryDir> stage;
};

QString findWorklistServerUtility()
{
    const auto configured = qEnvironmentVariable("DINGOBENCH_WLMSCPFS");
    if (QDir::isAbsolutePath(configured) && QFileInfo(configured).isExecutable())
        return configured;
    const auto sibling = QDir(QCoreApplication::applicationDirPath()).filePath(
#ifdef Q_OS_WIN
        "wlmscpfs.exe"
#else
        "wlmscpfs"
#endif
    );
    return QFileInfo(sibling).isExecutable() ? sibling : QStandardPaths::findExecutable("wlmscpfs");
}

WorklistServer::WorklistServer(logging::Logger &logger) : m_logger(logger), m_worker(logger)
{
    m_process.setProcessChannelMode(QProcess::MergedChannels);
    m_process.setStandardInputFile(QProcess::nullDevice());
    m_stopDeadline.setSingleShot(true);
    connect(&m_stopDeadline, &QTimer::timeout, &m_process, &QProcess::kill);
    connect(&m_process, &QProcess::started, this, &WorklistServer::startProbe);
    connect(&m_process, &QProcess::readyReadStandardOutput, this, &WorklistServer::readOutput);
    connect(&m_worker, &OperationRunner::finished, this, &WorklistServer::workerFinished);
    connect(&m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
        readOutput();
        if (m_state == State::Stopping) {
            finishStop();
        } else {
            const auto error = m_launchError.isEmpty()
                ? QString("wlmscpfs exited unexpectedly (code %1).").arg(code) : m_launchError;
            stopWith(error);
        }
    });
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            if (m_state == State::Stopping)
                finishStop();
            else
                stopWith("Could not launch wlmscpfs. Check the executable path and its dependencies.");
        }
    });
}

WorklistServer::~WorklistServer()
{
    m_process.disconnect(this);
    if (m_process.state() != QProcess::NotRunning) {
        m_process.terminate();
        if (!m_process.waitForFinished(400) && m_process.state() != QProcess::NotRunning) {
            m_process.kill();
            m_process.waitForFinished(1500);
        }
    }
}

bool WorklistServer::busy() const
{
    return m_state == State::Loading || m_state == State::Starting ||
           m_state == State::Listening || m_state == State::Stopping;
}

void WorklistServer::setState(State state, QString message, bool synthetic)
{
    m_state = state;
    m_message = std::move(message);
    m_logger.log(state == State::Error ? Severity::Error : Severity::Info, component, m_message, m_session, {}, synthetic);
    emit stateChanged();
}

void WorklistServer::refreshRecords(const QString &directory)
{
    if (!busy())
        prepare(directory, RecordAction::Refresh);
}

void WorklistServer::generateSamples(const QString &directory)
{
    if (!busy())
        prepare(directory, RecordAction::Generate);
}

void WorklistServer::start(WorklistServerSettings settings)
{
    if (busy())
        return;
    m_session = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!settings.allowWildcard) {
        setState(State::Error, "Confirm listening on all interfaces of the selected address family.");
        return;
    }
    settings.source.ae = directoryAe(settings.source.ae);
    if (settings.source.ae.isEmpty() || settings.port < 1 || settings.port > 65535) {
        setState(State::Error, "Use a valid port and a worklist AE with 1–16 letters, digits, underscores, or hyphens.");
        return;
    }
    const QFileInfo executable(settings.executable);
    if (!executable.isAbsolute() || !executable.isFile() || !executable.isExecutable() ||
        executable.completeBaseName().compare("wlmscpfs", Qt::CaseInsensitive) != 0) {
        setState(State::Error, "Choose the installed DCMTK wlmscpfs executable.");
        return;
    }
    settings.executable = executable.absoluteFilePath();
    m_settings = std::move(settings);
    prepare(m_settings.directory, RecordAction::Start);
}

void WorklistServer::prepare(QString directory, RecordAction action)
{
    const bool launch = action == RecordAction::Start;
    const bool generate = action == RecordAction::Generate;
    if (!launch)
        m_session = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_failure.clear();
    m_launchError.clear();
    m_recordAction = action;
    m_scan = std::make_shared<Scan>();
    if (!generate) {
        m_records.clear();
        emit recordsChanged();
    }
    const auto scan = m_scan;
    const auto ae = launch ? m_settings.source.ae : QString{};
    const auto executable = launch ? m_settings.executable : QString{};
    const auto today = QDate::currentDate();
    setState(State::Loading, generate ? "Generating 50 synthetic worklist records…" : "Checking worklist record files…",
             generate);
    m_job = m_worker.start(generate ? "Generate worklist samples" : "Validate worklist files",
        [scan, directory = std::move(directory), ae, executable, launch, generate, today](
        std::stop_token stop, const ReportDiagnostic &report) {
        const auto fail = [](const QString &message) { return OperationResult::failed({ErrorCode::InvalidInput, message}); };
        const auto source = receiveDirectory(directory);
        if (source.isEmpty())
            return fail("Choose an existing worklist directory outside Git checkouts.");
        const QDir input(source);
        if (generate) {
            const auto result = appendSamples(input, today, stop);
            if (result.status != OperationResult::Status::Succeeded)
                return result;
            report({Severity::Info, QString("Added %1 synthetic worklist records.").arg(sampleCount)});
        }
        const auto files = worklistFiles(input);
        if (files.size() > maxRecords)
            return fail(QString("Use a directory with at most %1 .wl files.").arg(maxRecords));
        QDir destination;
        if (launch) {
            const auto version = checkUtilityVersion(executable, stop);
            if (version.status != OperationResult::Status::Succeeded)
                return version;
            const auto temp = receiveDirectory(QDir::tempPath());
            if (temp.isEmpty())
                return fail("The system temporary directory is unavailable or inside a Git checkout.");
            scan->stage = std::make_unique<QTemporaryDir>(QDir(temp).filePath("dingobench-mwl-XXXXXX"));
            if (!scan->stage->isValid())
                return fail("Could not create a private worklist snapshot.");
            QDir stage(scan->stage->path());
            if (!stage.mkdir(ae) || !stage.cd(ae))
                return fail("Could not create the Called AE directory for wlmscpfs.");
            destination = stage;
            QFile lock(destination.filePath("lockfile"));
            if (!lock.open(QIODevice::WriteOnly | QIODevice::NewOnly))
                return fail("Could not create the worklist snapshot lock file.");
        }
        int valid = 0;
        for (const auto &info : files) {
            if (stop.stop_requested())
                return OperationResult::cancelled();
            WorklistRecord record{info.fileName()};
            QFile file(info.absoluteFilePath());
            if (info.isSymLink() || info.canonicalPath() != source ||
                !file.open(QIODevice::ReadOnly)) {
                record.issue = "Cannot read a regular file in the selected directory.";
            } else {
                const auto data = file.read(maxRecordBytes + 1);
                if (data.isEmpty() || data.size() > maxRecordBytes || !file.atEnd())
                    record.issue = "Record is empty, unreadable, or exceeds 1 MiB.";
                else
                    record.issue = inspectRecord(data, record);
                if (record.issue.isEmpty() && launch) {
                    QFile copy(destination.filePath(QString("record-%1.wl").arg(valid + 1)));
                    if (!copy.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
                        copy.write(data) != data.size() || !copy.flush())
                        return fail("Could not write the private worklist snapshot.");
                }
            }
            valid += record.issue.isEmpty();
            scan->records.append(std::move(record));
        }
        report({scan->records.size() == valid ? Severity::Info : Severity::Warning,
                QString("Worklist validation found %1 available and %2 rejected record(s).")
                    .arg(valid).arg(scan->records.size() - valid)});
        if (launch && !valid)
            return fail("No valid worklist records are available to serve.");
        return OperationResult{};
    }, generate, component);
}

void WorklistServer::workerFinished(QUuid id, const OperationResult &result)
{
    if (id != m_job)
        return;
    m_job = {};
    if (m_state == State::Stopping) {
        finishStop();
        return;
    }
    if (m_state == State::Starting) {
        readOutput();
        if (result.status == OperationResult::Status::Succeeded && m_probeSeen &&
            m_process.state() == QProcess::Running) {
            setState(State::Listening, QString("Listening as %1 on %2 port %3 · PID %4")
                .arg(m_settings.source.ae, m_settings.ipv6 ? "all IPv6 interfaces" : "all IPv4 interfaces")
                .arg(m_settings.port).arg(processId()));
        } else {
            stopWith(m_launchError.isEmpty() ? QString("Could not verify the wlmscpfs listener with C-ECHO.") : m_launchError);
        }
        return;
    }
    const bool generate = m_recordAction == RecordAction::Generate;
    if (!generate || result.status == OperationResult::Status::Succeeded) {
        m_records = std::move(m_scan->records);
        emit recordsChanged();
    }
    m_stage = std::move(m_scan->stage);
    m_scan.reset();
    if (result.error || m_recordAction != RecordAction::Start) {
        m_stage.reset();
        const auto message = generate ? QString("Added %1 synthetic worklist records. Worklist server stopped.").arg(sampleCount)
                                      : QString("Records refreshed. Worklist server stopped.");
        setState(result.error ? State::Error : State::Stopped, result.error ? result.error->message : message, generate);
        return;
    }

    QTcpServer preflight;
    if (!preflight.listen(m_settings.ipv6 ? QHostAddress::AnyIPv6 : QHostAddress::AnyIPv4, quint16(m_settings.port))) {
        m_stage.reset();
        setState(State::Error, QString("Cannot bind worklist port %1. It may be in use or unavailable.").arg(m_settings.port));
        return;
    }
    preflight.close();
    m_buffer.clear();
    m_probeSeen = m_probeAssociation = m_discardLine = false;
    m_process.setWorkingDirectory(m_stage->path());
    m_process.setProgram(m_settings.executable);
    m_process.setArguments({"--single-process", "--verbose", "--enable-file-reject", "--keep-char-set",
        "--disable-host-lookup", "--acse-timeout", "3", "--dimse-timeout", "5",
        m_settings.ipv6 ? "--ipv6" : "--ipv4", "--data-files-path", m_stage->path(),
        QString::number(m_settings.port)});
    setState(State::Starting, "Starting wlmscpfs and verifying the listener…");
    m_process.start();
}

void WorklistServer::startProbe()
{
    if (m_state != State::Starting)
        return;
    const profiles::Profile source{1, "Worklist startup probe", probeAe, {}, 11112, {}};
    const profiles::Profile peer{2, "Managed Worklist SCP", m_settings.source.ae,
        m_settings.ipv6 ? "::1" : "127.0.0.1", m_settings.port, {}};
    m_job = m_worker.start("Verify worklist listener", [source, peer](std::stop_token stop, const ReportDiagnostic &) {
        auto verify = verificationOperation(source, peer, {1, 1, 1});
        for (int attempt = 0; attempt < 4 && !stop.stop_requested(); ++attempt) {
            QThread::msleep(100);
            if (verify(stop, [](DiagnosticEvent) {}).status == OperationResult::Status::Succeeded)
                return OperationResult{};
        }
        return stop.stop_requested() ? OperationResult::cancelled()
            : OperationResult::failed({ErrorCode::StartFailed, "Worklist startup C-ECHO failed."});
    }, false, component);
}

void WorklistServer::stop()
{
    if (busy() && m_state != State::Stopping)
        stopWith({});
}

void WorklistServer::stopWith(QString failure)
{
    m_failure = std::move(failure);
    setState(State::Stopping, "Stopping the worklist server…");
    if (!m_job.isNull())
        m_worker.cancel(m_job);
    if (m_process.state() != QProcess::NotRunning) {
        m_process.terminate();
        m_stopDeadline.start(400);
    }
    finishStop();
}

void WorklistServer::finishStop()
{
    if (!m_job.isNull() || m_process.state() != QProcess::NotRunning)
        return;
    m_stopDeadline.stop();
    finishSession(QDateTime::currentDateTimeUtc(), m_failure.isEmpty() ? "Cancelled" : "Failed",
                  "Utility exited without a reported association release");
    m_scan.reset();
    m_stage.reset();
    m_buffer.clear();
    setState(m_failure.isEmpty() ? State::Stopped : State::Error,
             m_failure.isEmpty() ? QString("Worklist server stopped.") : m_failure);
}

void WorklistServer::finishSession(const QDateTime &ended, QString result, QString closure)
{
    if (!m_diagnostic)
        return;
    auto session = std::exchange(m_diagnostic, std::nullopt).value();
    session.finish(ended, std::move(result), std::move(closure));
    emit sessionFinished(std::move(session));
}

void WorklistServer::reportActivity(QString event, QString detail, Severity severity)
{
    if (auto logged = m_logger.log(severity, component, event,
            m_diagnostic ? m_diagnostic->id.toString(QUuid::WithoutBraces) : m_session, detail);
        m_diagnostic && logged)
        m_diagnostic->addLog(std::move(*logged));
    emit activity(std::move(event), std::move(detail));
}

void WorklistServer::readOutput()
{
    m_buffer += m_process.readAllStandardOutput();
    qsizetype start = 0;
    for (qsizetype newline; (newline = m_buffer.indexOf('\n', start)) >= 0; start = newline + 1) {
        if (!m_discardLine && newline - start <= 4096)
            readLine(m_buffer.mid(start, newline - start).trimmed());
        m_discardLine = false;
    }
    m_buffer.remove(0, start);
    if (m_buffer.size() > 4096) {
        m_buffer.clear();
        m_discardLine = true;
    }
}

void WorklistServer::readLine(QByteArray line)
{
    const auto now = QDateTime::currentDateTimeUtc();
    if (line.startsWith("I: Association Received (")) {
        m_probeAssociation = m_state == State::Starting && line.contains(":DINGO_MWL_PROBE -> ");
        if (m_probeAssociation) {
            m_probeSeen = true;
            return;
        }
        finishSession(now, "Failed", "Utility did not report the previous association end");
        m_diagnostic.emplace();
        auto &session = *m_diagnostic;
        session.id = QUuid::createUuid();
        session.operation = "Worklist SCP";
        session.captureSource = "Utility diagnostics";
        session.inbound = true;
        session.identities.source.emplace(m_settings.source);
        session.started = now;
        session.association.connection = "Inbound TCP accepted (utility diagnostic)";
        session.association.requestSent = true;
        const auto metadata = line.mid(sizeof("I: Association Received (") - 1).chopped(1);
        const auto arrow = metadata.lastIndexOf(" -> ");
        const auto colon = arrow > 0 ? metadata.indexOf(':') : -1;
        // The utility uses a colon to separate the peer and Calling AE without escaping.
        // Leave the peer and Calling AE unavailable if the split is ambiguous.
        if (colon > 0 && colon < arrow && metadata.mid(colon + 1, arrow - colon - 1).count(':') == 0) {
            session.association.remoteEndpoint = QString::fromUtf8(metadata.first(colon)).trimmed();
            session.association.callingAe = QString::fromUtf8(metadata.mid(colon + 1, arrow - colon - 1)).trimmed();
        }
        if (arrow > 0 && metadata.indexOf(" -> ") == arrow)
            session.association.calledAe = QString::fromUtf8(metadata.mid(arrow + 4)).trimmed();
        reportActivity("Inbound association received", "wlmscpfs received an association request.");
        return;
    }
    if (line.startsWith("I: Association Release") || line.startsWith("I: Association Aborted")) {
        if (!m_probeAssociation) {
            const bool released = line.endsWith("Release");
            const auto closure = released ? "Peer released the association." : "Peer aborted the association.";
            reportActivity("Association ended", closure);
            if (m_diagnostic) {
                const auto status = m_diagnostic->status;
                const auto result = !released || !m_diagnostic->errorCategory.isEmpty() ? "Failed"
                    : status == 0xfe00 ? "Cancelled" : status == 0 ? "Succeeded" : "Unavailable";
                finishSession(now, result, closure);
            }
        }
        m_probeAssociation = false;
        return;
    }
    if (m_probeAssociation)
        return;
    if (line.startsWith("I: Association Acknowledged")) {
        if (m_diagnostic)
            m_diagnostic->association.outcome = "Association acknowledged (utility diagnostic)";
        reportActivity("Association accepted", "wlmscpfs acknowledged the association.");
    } else if (line.startsWith("I: Refusing Association") || line.startsWith("I: No Acceptable Presentation Contexts")) {
        reportActivity("Association rejected", "Check the Called AE and proposed presentation contexts.", Severity::Warning);
        if (m_diagnostic) {
            m_diagnostic->association.outcome = "Association refused (utility diagnostic)";
            m_diagnostic->errorCategory = line.startsWith("I: No Acceptable Presentation Contexts")
                ? "No usable presentation context" : "Association refused";
            finishSession(now, "Failed", "Inbound association refused");
        }
    } else if (line.startsWith("I: Find SCP Request Identifiers:")) {
        reportActivity("C-FIND request received", "Worklist matching started.");
        if (m_diagnostic) {
            DimseRecord request;
            request.timestamp = now;
            request.direction = Direction::Incoming;
            request.command = "C-FIND-RQ";
            request.datasetPresent = true;
            ++m_diagnostic->counts["C-FIND requests"];
            m_diagnostic->status.reset();
            m_diagnostic->record(std::move(request));
        }
    } else if (line.startsWith("I: Worklist Find SCP Response ")) {
        std::optional<quint16> status;
        if (line.endsWith("(Success)")) {
            status = 0x0000;
            reportActivity("C-FIND complete", "Success (0x0000).");
        } else if (line.endsWith("(Pending)")) {
            status = 0xff00;
            reportActivity("C-FIND match", "Pending (0xFF00).", Severity::Debug);
        } else if (line.endsWith("(Pending: WarningUnsupportedOptionalKeys)")) {
            status = 0xff01;
            reportActivity("C-FIND match with warning", "Optional keys unsupported (0xFF01).", Severity::Warning);
        } else if (line.endsWith("(Cancel: MatchingTerminatedDueToCancelRequest)")) {
            status = 0xfe00;
            reportActivity("C-FIND cancelled", "Matching terminated (0xFE00).", Severity::Warning);
        } else if (line.endsWith("(Error: DataSetDoesNotMatchSOPClass)")) {
            status = 0xa900;
            reportActivity("C-FIND identifier rejected", "Identifier does not match the SOP Class (0xA900).", Severity::Warning);
        } else {
            reportActivity("C-FIND response warning or failure", "wlmscpfs reported a non-success status.", Severity::Warning);
        }
        if (m_diagnostic) {
            DimseRecord response;
            response.timestamp = now;
            response.direction = Direction::Outgoing;
            response.command = "C-FIND-RSP";
            response.status = status;
            ++m_diagnostic->counts["C-FIND responses"];
            if (status == 0xff00 || status == 0xff01)
                ++m_diagnostic->counts["Pending responses"];
            m_diagnostic->record(std::move(response));
            if (status != 0xff00 && status != 0xff01) {
                m_diagnostic->status = status;
                if (status != 0 && status != 0xfe00)
                    m_diagnostic->errorCategory = "Worklist query failed (utility diagnostic)";
            }
        }
    } else if (line.startsWith("E: ") || line.startsWith("F: ")) {
        if (m_state == State::Starting && (line.toLower().contains("network") || line.toLower().contains("bind")))
            m_launchError = QString("wlmscpfs could not start its network listener on port %1.").arg(m_settings.port);
        reportActivity("wlmscpfs error", m_launchError.isEmpty() ? "The utility reported a network or DIMSE error." : m_launchError,
                       Severity::Error);
    }
}

}  // namespace dingobench::dicom
