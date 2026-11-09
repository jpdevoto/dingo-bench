// SPDX-License-Identifier: GPL-3.0-only
#include "dingobench/dicom/StorageReceive.h"
#include "DicomFileInput.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <utility>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <string>
#endif

namespace dingobench::dicom {
namespace {

using logging::Severity;
constexpr int startupMs = 5000;
constexpr int shutdownMs = 2500;

QString helperPath()
{
    return QDir(QCoreApplication::applicationDirPath()).filePath(
#ifdef Q_OS_WIN
        "DingoBenchReceiver.exe"
#else
        "DingoBenchReceiver"
#endif
    );
}

QString resolvedDirectory(const QFileInfo &info)
{
#ifdef Q_OS_WIN
    const auto path = QDir::toNativeSeparators(info.absoluteFilePath());
    const auto handle = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return {};
    const auto capacity = GetFinalPathNameByHandleW(handle, nullptr, 0, VOLUME_NAME_DOS);
    std::wstring buffer(capacity, L'\0');
    const auto length = capacity ? GetFinalPathNameByHandleW(handle, buffer.data(), capacity, VOLUME_NAME_DOS) : 0;
    CloseHandle(handle);
    if (!length || length >= capacity)
        return {};
    auto resolved = QDir::fromNativeSeparators(QString::fromWCharArray(buffer.data(), length));
    if (resolved.startsWith("//?/UNC/", Qt::CaseInsensitive))
        resolved = "//" + resolved.sliced(8);
    else if (resolved.startsWith("//?/"))
        resolved.remove(0, 4);
    return resolved;
#else
    return info.canonicalFilePath();
#endif
}

bool outsideCheckout(QDir ancestor)
{
    do {
        if (ancestor.dirName().compare("GitHub", Qt::CaseInsensitive) == 0 ||
            QFileInfo::exists(ancestor.filePath(".git")))
            return false;
    } while (!ancestor.isRoot() && ancestor.cdUp());
    return true;
}

}  // namespace

QString receiveDirectory(const QString &path)
{
    if (path.trimmed().isEmpty() || !QDir::isAbsolutePath(path))
        return {};
    const QFileInfo info(path);
    if (!info.isDir() || !outsideCheckout(QDir(info.absoluteFilePath())))
        return {};
    const auto resolved = resolvedDirectory(info);
    return !resolved.isEmpty() && outsideCheckout(QDir(resolved)) ? resolved : QString();
}

StorageReceiver::StorageReceiver(logging::Logger &logger, QObject *parent)
    : QObject(parent), m_logger(logger)
{
    m_deadline.setSingleShot(true);
    connect(&m_process, &QProcess::readyReadStandardOutput, this, &StorageReceiver::readEvents);
    // Toolkit stderr can include remote data. Drain it without exporting it to the session log.
    connect(&m_process, &QProcess::readyReadStandardError, this, [this] {
        m_process.readAllStandardError();
    });
    connect(&m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        readEvents();
        resetAfterExit();
        if (!m_failure.isEmpty())
            setState(State::Error, m_failure);
        else if (m_stopRequested)
            setState(State::Stopped, status == QProcess::NormalExit && code == 0
                ? "Listener stopped." : "Listener stopped after forcing the helper to exit.");
        else
            setState(State::Error, QString("Receiver process exited unexpectedly (exit code %1).").arg(code));
    });
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            resetAfterExit();
            setState(State::Error, "Could not launch the receiver helper. " + m_process.errorString());
        }
    });
    connect(&m_deadline, &QTimer::timeout, this, [this] {
        if (!active())
            return;
        if (!m_stopRequested)
            m_failure = "Receiver did not report successful startup before the deadline.";
        m_process.kill();
    });
}

StorageReceiver::~StorageReceiver()
{
    m_process.disconnect(this);
    if (active()) {
        m_process.closeWriteChannel();
        if (!m_process.waitForFinished(shutdownMs)) {
            m_process.kill();
            m_process.waitForFinished(1000);
        }
    }
}

void StorageReceiver::setState(State state, QString message)
{
    m_state = state;
    m_message = std::move(message);
    m_logger.log(state == State::Error ? Severity::Error : Severity::Info,
                 "Storage Receive", m_message, m_listenerId);
    emit stateChanged();
}

void StorageReceiver::start(const profiles::Profile &source, const QString &output, bool enforceCalledAe)
{
    if (active())
        return;
    m_listenerId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_buffer.clear();
    m_failure.clear();
    m_stopRequested = false;
    if (const auto invalid = profiles::Store::validate(profiles::Kind::Source, source); !invalid.isEmpty()) {
        setState(State::Error, invalid);
        return;
    }
    m_output = receiveDirectory(output);
    if (m_output.isEmpty()) {
        setState(State::Error, "Choose an existing output directory outside Git checkouts.");
        return;
    }
    m_stage = std::make_unique<QTemporaryDir>(QDir(m_output).filePath(".dingobench-receive-XXXXXX"));
    if (!m_stage->isValid()) {
        resetAfterExit();
        setState(State::Error, "Cannot create a staging directory in the selected output directory.");
        return;
    }
    m_identity.emplace(source);
    m_listenerAe = source.ae.trimmed();
    m_process.setProgram(helperPath());
    m_process.setArguments({"--ae", m_listenerAe, "--port", QString::number(source.port),
        "--bind", source.address.trimmed().isEmpty() ? QString("127.0.0.1") : source.address.trimmed(),
        "--output", m_output, "--stage", m_stage->path(),
        enforceCalledAe ? "--strict-ae" : "--allow-any-ae"});
    setState(State::Starting, "Starting the receiver…");
    m_deadline.start(startupMs);
    m_process.start();
}

void StorageReceiver::stop()
{
    if (!active() || m_stopRequested)
        return;
    m_stopRequested = true;
    setState(State::Stopping, "Stopping the receiver…");
    m_process.closeWriteChannel();
    m_deadline.start(shutdownMs);
}

void StorageReceiver::resetAfterExit()
{
    m_deadline.stop();
    finishSession(QDateTime::currentDateTimeUtc(), m_stopRequested ? "Cancelled" : "Failed",
                  "Receiver exited without a confirmed association release");
    m_stage.reset();
    m_buffer.clear();
}

void StorageReceiver::readEvents()
{
    m_buffer += m_process.readAllStandardOutput();
    for (qsizetype newline; (newline = m_buffer.indexOf('\n')) >= 0;) {
        const auto object = QJsonDocument::fromJson(m_buffer.left(newline)).object();
        m_buffer.remove(0, newline + 1);
        const auto type = object.value("type").toString();
        if (type == "ready" && m_state == State::Starting) {
            m_deadline.stop();
            m_endpoint = object.value("endpoint").toString();
            setState(State::Listening, "Listening on " + m_endpoint + u'.');
        } else if (type == "error") {
            m_failure = object.value("message").toString();
        } else if (type == "association" || type == "store") {
            ReceiveEvent event;
            event.timestamp = QDateTime::fromString(object.value("timestamp").toString(), Qt::ISODateWithMs);
            event.association = object.value("association").toString();
            event.callingAe = object.value("callingAe").toString();
            event.calledAe = object.value("calledAe").toString();
            event.peer = object.value("peer").toString();
            event.action = object.value("action").toString();
            event.message = object.value("message").toString();
            event.sopClass = object.value("sopClass").toString();
            event.sopInstance = object.value("sopInstance").toString();
            event.moveOriginatorAe = object.value("moveOriginatorAe").toString();
            const auto originatorId = object.value("moveOriginatorId").toInt(-1);
            if (originatorId >= 0 && originatorId <= 65535)
                event.moveOriginatorId = quint16(originatorId);
            event.bytes = object.value("bytes").toInteger();
            if (const auto value = object.value("status"); value.isDouble())
                event.status = quint16(value.toInt());
            const auto name = object.value("file").toString();
            if (!name.isEmpty() && name == QFileInfo(name).fileName())
                event.path = QDir(m_output).filePath(name);
            auto logged = m_logger.log(event.status.value_or(0) || event.action == "Rejected" || event.action == "Interrupted"
                             ? Severity::Warning : Severity::Info,
                         "Storage Receive", event.message, event.association,
                         QString("Calling AE %1 · Called AE %2 · Peer %3%4")
                             .arg(event.callingAe, event.calledAe, event.peer,
                                  event.sopInstance.isEmpty() ? QString() : " · SOP Instance UID " + event.sopInstance));
            captureEvent(object, event, std::move(logged));
            emit eventReceived(event);
        }
    }
    if (m_buffer.size() > 64 * 1024) {
        m_failure = "Receiver sent an invalid status record.";
        m_buffer.clear();
        m_process.kill();
    }
}

void StorageReceiver::finishSession(const QDateTime &ended, QString result, QString closure)
{
    if (!m_diagnostic)
        return;
    auto session = std::exchange(m_diagnostic, std::nullopt).value();
    session.finish(ended, std::move(result), std::move(closure));
    emit sessionFinished(std::move(session));
}

void StorageReceiver::captureEvent(const QJsonObject &object, const ReceiveEvent &event, std::optional<logging::Event> logged)
{
    const auto id = QUuid::fromString(event.association);
    if (id.isNull())
        return;
    if (m_diagnostic && m_diagnostic->id != id)
        finishSession(event.timestamp, "Failed", "Association end was not reported");
    if (!m_diagnostic) {
        m_diagnostic.emplace();
        auto &session = *m_diagnostic;
        session.id = id;
        session.operation = "Storage SCP";
        session.captureSource = "Receiver helper";
        session.inbound = true;
        session.identities.source = m_identity;
        session.started = QDateTime::fromString(object.value("associationStarted").toString(), Qt::ISODateWithMs);
        session.association.connection = "Inbound TCP accepted";
        session.association.callingAe = event.callingAe;
        session.association.calledAe = event.calledAe;
        session.association.localEndpoint = m_endpoint;
        session.association.remoteEndpoint = event.peer;
    }
    auto &session = *m_diagnostic;
    if (logged)
        session.addLog(std::move(*logged));
    auto &association = session.association;
    if (event.action == "Accepted" || event.action == "Rejected") {
        association.requestSent = true;  // The helper decoded the incoming request.
        association.applicationContext = object.value("applicationContext").toString();
        association.outcome = event.action == "Accepted" ? "Association acknowledged" : "Association rejected";
        if (object.contains("localMaxPdu"))
            association.localMaxPdu = quint32(object.value("localMaxPdu").toInteger());
        if (object.contains("peerMaxPdu"))
            association.peerMaxPdu = quint32(object.value("peerMaxPdu").toInteger());
        for (const auto &value : object.value("contexts").toArray()) {
            const auto entry = value.toObject();
            PresentationContext context;
            context.id = entry.value("id").toInt();
            context.abstractSyntax = entry.value("abstractSyntax").toString();
            for (const auto &syntax : entry.value("syntaxes").toArray())
                context.proposedSyntaxes.append(syntax.toString());
            context.acceptedSyntax = entry.value("acceptedSyntax").toString();
            context.proposedRole = entry.value("proposedRole").toString();
            context.acceptedRole = entry.value("acceptedRole").toString();
            const auto reason = entry.value("reason").toInt(-1);
            if (reason == 0)
                context.outcome = "Accepted";
            else if (reason >= 1 && reason <= 4) {
                context.rejectionReason = reason;
                context.outcome = reason == 4 ? "Rejected: transfer syntaxes not supported"
                    : reason == 3 ? "Rejected: abstract syntax not supported" : "Rejected";
            }
            association.contexts.append(std::move(context));
        }
    }
    if (object.value("type") == "store") {
        DimseRecord message;
        message.timestamp = event.timestamp;
        message.sopClass = event.sopClass;
        message.context = quint16(object.value("context").toInt());
        const auto messageId = quint16(object.value("messageId").toInt());
        if (event.action == "Receiving") {
            ++session.counts["C-STORE requests"];
            message.direction = Direction::Incoming;
            message.command = "C-STORE-RQ";
            message.id = messageId;
            message.datasetPresent = true;
            session.record(std::move(message));
        } else if (event.status) {
            ++session.counts[*event.status == 0 ? "Objects stored" : "Storage failures"];
            if (*event.status != 0)
                session.errorCategory = "Storage failed";
            if (object.value("responseSent").toBool()) {
                message.direction = Direction::Outgoing;
                message.command = "C-STORE-RSP";
                message.responseTo = messageId;
                message.datasetPresent = false;
                message.status = event.status;
                session.status = event.status;
                session.record(std::move(message));
            }
        }
        return;
    }
    if (event.action == "Released")
        finishSession(event.timestamp, session.errorCategory.isEmpty() ? "Succeeded" : "Failed", "Association release acknowledged");
    else if (event.action == "Rejected") {
        session.errorCategory = "Called AE Title not recognized";
        finishSession(event.timestamp, "Failed", "Inbound association rejected");
    } else if (event.action == "Interrupted") {
        session.errorCategory = "Inbound association interrupted";
        finishSession(event.timestamp, m_stopRequested ? "Cancelled" : "Failed", "Association ended without a confirmed release");
    }
}

Operation inspectReceivedFile(QString path, std::function<void(DatasetSnapshot)> deliver)
{
    return [path = std::move(path), deliver = std::move(deliver)](std::stop_token stop, const ReportDiagnostic &) {
        DcmFileFormat format;
        if (readPart10(path, format).bad())
            return OperationResult::failed({ErrorCode::InvalidInput, "Could not read the received DICOM file."});
        if (stop.stop_requested())
            return OperationResult::cancelled();
        deliver(snapshotDataset(*format.getDataset()));
        return OperationResult{};
    };
}

}  // namespace dingobench::dicom
