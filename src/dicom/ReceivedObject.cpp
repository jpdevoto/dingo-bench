// SPDX-License-Identifier: GPL-3.0-only
#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif
#include "ReceivedObject.h"
#include "dingobench/dicom/StorageReceive.h"

#include <dcmtk/config/osconfig.h>
#include <dcmtk/dcmdata/dcdeftag.h>
#include <dcmtk/dcmdata/dcfilefo.h>
#include <dcmtk/dcmdata/dcostrma.h>
#include <dcmtk/dcmdata/dcvrui.h>
#include <dcmtk/dcmnet/dimse.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <QDir>
#include <QFileInfo>
#include <QScopeGuard>
#include <limits>

namespace dingobench::dicom {
namespace {

// Stage in the destination's filesystem and publish with an atomic
// no-replace operation. The temporary file still owns its staging name.
bool publishFile(const QString &source, const QString &destination)
{
#ifdef _WIN32
    return MoveFileExW(QDir::toNativeSeparators(source).toStdWString().c_str(),
                       QDir::toNativeSeparators(destination).toStdWString().c_str(), MOVEFILE_WRITE_THROUGH) != 0;
#else
    return ::link(QFile::encodeName(source).constData(), QFile::encodeName(destination).constData()) == 0;
#endif
}

class FileConsumer final : public DcmConsumer {
public:
    explicit FileConsumer(QFile &file) : m_file(file) {}
    OFBool good() const override { return m_good; }
    OFCondition status() const override { return m_good ? EC_Normal : EC_InvalidStream; }
    OFBool isFlushed() const override { return true; }
    offile_off_t avail() const override { return m_good ? std::numeric_limits<offile_off_t>::max() : 0; }
    offile_off_t write(const void *buffer, offile_off_t length) override
    {
        const auto count = m_file.write(static_cast<const char *>(buffer), length);
        if (count != length)
            m_good = false;
        return qMax<qint64>(0, count);
    }
    void flush() override { m_good = m_file.flush() && m_good; }

private:
    QFile &m_file;
    bool m_good = true;
};

class FileOutput final : public DcmOutputStream {
public:
    explicit FileOutput(QFile &file) : DcmOutputStream(&m_consumer), m_consumer(file) {}

private:
    FileConsumer m_consumer;
};

}  // namespace

bool stagingAllowed(const QString &output, const QString &stage)
{
    const QFileInfo info(stage);
    return receiveDirectory(output) == output && !output.isEmpty() &&
        info.isDir() && info.canonicalFilePath() == stage && info.absolutePath() == output &&
        info.fileName().startsWith(".dingobench-receive-");
}

StoreOutcome saveReceived(std::unique_ptr<DcmDataset> dataset, const T_DIMSE_C_StoreRQ &request,
                          const QString &output, const QString &stage, std::stop_token stop)
{
    OFString sopClass, sopInstance;
    if (dataset->findAndGetOFStringArray(DCM_SOPClassUID, sopClass).bad() ||
        dataset->findAndGetOFStringArray(DCM_SOPInstanceUID, sopInstance).bad() ||
        DcmUniqueIdentifier::checkStringValue(sopClass, "1").bad() ||
        DcmUniqueIdentifier::checkStringValue(sopInstance, "1").bad() ||
        sopClass != request.AffectedSOPClassUID || sopInstance != request.AffectedSOPInstanceUID)
        return {{}, "Dataset SOP identity did not match the C-STORE request.", 0, 0xa900};
    if (stop.stop_requested())
        return {{}, "Receiver stopped before the object was saved."};
    if (!stagingAllowed(output, stage))
        return {{}, "Output or staging directory is unavailable."};
    const auto name = QUuid::createUuid().toString(QUuid::WithoutBraces) + ".dcm";
    const auto stagingPath = QDir(stage).filePath(name + ".partial");
    QFile temporary(stagingPath);
    if (!temporary.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        return {{}, "Could not open a staging file."};
    const auto cleanup = qScopeGuard([&] {
        temporary.close();
        QFile::remove(stagingPath);
    });
    const auto syntax = dataset->getOriginalXfer();
    DcmFileFormat format(dataset.release(), OFFalse);
    FileOutput stream(temporary);
    format.transferInit();
    const auto written = format.write(stream, syntax, EET_ExplicitLength, nullptr, EGL_recalcGL);
    format.transferEnd();
    stream.flush();
    if (written.bad() || !stream.good())
        return {{}, "Could not write the received DICOM file."};
    const auto bytes = temporary.size();
    temporary.close();
    if (stop.stop_requested() || !stagingAllowed(output, stage))
        return {{}, "Receiver stopped or the output directory became unavailable."};
    if (!publishFile(temporary.fileName(), QDir(output).filePath(name)))
        return {{}, "Could not publish the received file without overwriting an existing file."};
    return {name, "C-STORE object saved.", bytes, STATUS_Success};
}

}  // namespace dingobench::dicom
