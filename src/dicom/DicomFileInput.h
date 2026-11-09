// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <dcmtk/config/osconfig.h>
#include <dcmtk/dcmdata/dcistrma.h>
#include <dcmtk/dcmdata/dcfilefo.h>
#include <QFile>
#include <algorithm>
#include <utility>
#include <stop_token>

namespace dingobench::dicom {

struct FileReadControl {
    std::stop_token stop;
    qint64 physicalBytesRead = 0;
    int calls = 0;
    bool limitReached = false;
};

// Keep file opening Unicode-safe even with narrow-path DCMTK builds. DCMTK
// can reopen deferred value fields at their file offsets without a second copy.
class FileProducer final : public DcmProducer {
public:
    FileProducer(const QString &path, offile_off_t offset, FileReadControl *control)
        : m_file(path), m_control(control)
    {
        if (!m_file.open(QIODevice::ReadOnly) || offset > m_file.size() || !m_file.seek(offset))
            m_status = EC_InvalidStream;
    }

    OFBool good() const override { return m_status.good(); }
    OFCondition status() const override { return m_status; }
    OFBool eos() override { return !good() || m_file.atEnd(); }
    offile_off_t avail() override { return good() ? m_file.size() - m_file.pos() : 0; }
    offile_off_t position() const { return m_file.pos(); }

    offile_off_t read(void *buffer, offile_off_t length) override
    {
        if (!good() || length <= 0)
            return 0;
        if (!permit(length))
            return 0;
        const auto count = m_file.read(static_cast<char *>(buffer), length);
        if (count >= 0) {
            if (m_control)
                m_control->physicalBytesRead += count;
            return count;
        }
        m_status = EC_InvalidStream;
        return 0;
    }

    offile_off_t skip(offile_off_t length) override
    {
        if (!good() || length <= 0)
            return 0;
        if (!permit(0))
            return 0;
        const auto count = std::min<qint64>(length, avail());
        if (!m_file.seek(m_file.pos() + count)) {
            m_status = EC_InvalidStream;
            return 0;
        }
        if (count != length)
            m_status = EC_EndOfStream;
        return count;
    }

    void putback(offile_off_t count) override
    {
        if (!good() || count < 0 || count > m_file.pos() || !m_file.seek(m_file.pos() - count))
            m_status = EC_PutbackFailed;
    }

private:
    bool permit(offile_off_t bytes)
    {
        if (!m_control)
            return true;
        constexpr qint64 maxPhysicalBytes = 16 * 1024 * 1024;
        if (m_control->stop.stop_requested()) {
            m_status = EC_InvalidStream;
            return false;
        }
        if (++m_control->calls > 250000 || bytes > maxPhysicalBytes - m_control->physicalBytesRead) {
            m_control->limitReached = true;
            m_status = EC_InvalidStream;
            return false;
        }
        return true;
    }

    QFile m_file;
    FileReadControl *m_control;
    OFCondition m_status = EC_Normal;
};

class FileInput final : public DcmInputStream {
    class Factory final : public DcmInputStreamFactory {
    public:
        Factory(QString path, offile_off_t offset, FileReadControl *control)
            : m_path(std::move(path)), m_offset(offset), m_control(control) {}
        DcmInputStream *create() const override { return new FileInput(m_path, m_offset, m_control); }
        DcmInputStreamFactory *clone() const override { return new Factory(m_path, m_offset, m_control); }
        DcmInputStreamFactoryType ident() const override { return DFT_DcmInputFileStreamFactory; }

    private:
        QString m_path;
        offile_off_t m_offset;
        FileReadControl *m_control;
    };

public:
    explicit FileInput(QString path, offile_off_t offset = 0, FileReadControl *control = nullptr)
        : DcmInputStream(&m_producer), m_producer(path, offset, control), m_path(std::move(path)), m_control(control) {}

    DcmInputStreamFactory *newFactory() const override
    {
        return currentProducer() == &m_producer ? new Factory(m_path, m_producer.position(), m_control) : nullptr;
    }

private:
    FileProducer m_producer;
    QString m_path;
    FileReadControl *m_control;
};

inline OFCondition readPart10(const QString &path, DcmFileFormat &format)
{
    FileInput stream(path);
    format.setReadMode(ERM_fileOnly);
    format.transferInit();
    const auto condition = format.read(stream, EXS_Unknown, EGL_noChange, DCM_MaxReadLength);
    format.transferEnd();
    return condition.bad() ? condition : stream.status();
}

}  // namespace dingobench::dicom
