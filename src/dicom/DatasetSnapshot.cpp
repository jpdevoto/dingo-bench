// SPDX-License-Identifier: GPL-3.0-only
#include "dingobench/dicom/DiagnosticSession.h"
#include "FileDatasetSnapshot.h"

#include <dcmtk/config/osconfig.h>
#include <dcmtk/dcmdata/dcdeftag.h>
#include <dcmtk/dcmdata/dcitem.h>
#include <dcmtk/dcmdata/dcsequen.h>
#include <dcmtk/dcmdata/dcspchrs.h>

#include <QStringDecoder>
#include <algorithm>

namespace dingobench::dicom {
namespace {

struct Budget {
    int nodes = 1500;
    int bytes = 64 * 1024;
    bool truncated = false;
    bool file = false;
    bool decodingIssue = false;
    std::stop_token stop;
};

bool take(Budget &budget, int depth)
{
    if (budget.stop.stop_requested() || depth > 24 || budget.nodes == 0) {
        budget.truncated = true;
        return false;
    }
    --budget.nodes;
    return true;
}

QList<Attribute> snapshot(DcmItem &item, const OFString &inherited, int depth, Budget &budget)
{
    OFString charset = inherited;
    if (budget.file) {
        DcmElement *attribute = nullptr;
        if (item.findAndGetElement(DCM_SpecificCharacterSet, attribute).good()) {
            if (!attribute->valueLoaded() || attribute->getLengthField() > 8192 ||
                attribute->getOFStringArray(charset).bad())
                budget.decodingIssue = true;
        }
    } else if (item.findAndGetOFStringArray(DCM_SpecificCharacterSet, charset).bad()) {
        charset = inherited;
    }
    DcmSpecificCharacterSet converter;
    const bool utf8 = charset == "ISO_IR 192";
    const bool convertible = !charset.empty() && !utf8 &&
                             converter.selectCharacterSet(charset, "ISO_IR 192").good();
    QList<Attribute> entries;
    for (unsigned long index = 0; index < item.card() && take(budget, depth); ++index) {
        auto &element = *item.getElement(index);
        auto key = element.getTag();
        const auto length = element.getLengthField();
        const auto tag = QString("(%1,%2)").arg(QString::number(key.getGTag(), 16).rightJustified(4, u'0'),
                                                QString::number(key.getETag(), 16).rightJustified(4, u'0')).toUpper();
        const int vm = ((budget.file || element.isaString()) && !element.valueLoaded()) ||
                       (element.isaString() && length > 8192)
            ? -1 : int(element.getVM());
        Attribute entry{tag, QString::fromLatin1(key.getTagName()), QString::fromLatin1(key.getVRName()),
                        {}, vm};
        if (budget.file)
            entry.encodedLength = length;
        if (auto *sequence = dynamic_cast<DcmSequenceOfItems *>(&element)) {
            entry.kind = Attribute::Sequence;
            entry.value = QString("%1 item(s)").arg(sequence->card());
            for (unsigned long i = 0; i < sequence->card() && take(budget, depth + 1); ++i) {
                entry.children.append({{}, QString("Item %1").arg(i + 1), {}, {}, 0, Attribute::Item,
                    snapshot(*sequence->getItem(i), charset, depth + 2, budget)});
                if (budget.file)
                    entry.children.last().encodedLength = sequence->getItem(i)->getLengthField();
            }
        } else if (!length) {
            entry.kind = Attribute::Empty;
            entry.value = "<empty>";
        } else if (const auto vr = key.getEVR();
                   key == DCM_PixelData || (budget.file && (key == DCM_FloatPixelData || key == DCM_DoubleFloatPixelData)) ||
                   vr == EVR_OB || vr == EVR_OW || vr == EVR_UN ||
                   vr == EVR_OL || vr == EVR_OV || vr == EVR_OF || vr == EVR_OD || vr == EVR_ox ||
                   key.getVR().isUnknown() || !element.valueLoaded()) {
            entry.kind = Attribute::Binary;
            const bool pixel = key == DCM_PixelData || key == DCM_FloatPixelData || key == DCM_DoubleFloatPixelData;
            if (budget.file && pixel)
                entry.value = "<pixel payload omitted>";
            else
                entry.value = length == DCM_UndefinedLength ? "<binary or deferred value · undefined length>"
                    : QString("<binary or deferred value · %1 bytes>").arg(length);
        } else if (length > quint32(qMin(budget.bytes, 8192))) {
            entry.kind = Attribute::Unavailable;
            entry.value = "<value omitted by capture limit>";
            budget.truncated = true;
        } else {
            OFString raw, converted;
            const bool affected = element.isAffectedBySpecificCharacterSet();
            const auto delimiters = key.getEVR() == EVR_PN ? "\\^=" : "\\";
            if (element.getOFStringArray(raw, OFFalse).bad() ||
                (affected && !charset.empty() && !utf8 &&
                 (!convertible || converter.convertString(raw, converted, delimiters).bad()))) {
                entry.kind = Attribute::Unavailable;
                entry.value = "<value could not be decoded>";
                budget.decodingIssue = true;
            } else {
                const auto &text = affected && convertible ? converted : raw;
                if (text.size() > size_t(budget.bytes)) {
                    entry.kind = Attribute::Unavailable;
                    entry.value = "<value omitted by capture limit>";
                    budget.truncated = true;
                } else {
                    budget.bytes -= int(text.size());
                    if (budget.file && affected) {
                        QStringDecoder decoder(QStringDecoder::Utf8);
                        entry.value = decoder(QByteArrayView(text.data(), qsizetype(text.size())));
                        const bool outsideDefaultRepertoire = charset.empty() &&
                            std::ranges::any_of(raw, [](unsigned char c) { return c > 127; });
                        if (decoder.hasError() || outsideDefaultRepertoire) {
                            entry.kind = Attribute::Unavailable;
                            entry.value = "<value could not be decoded>";
                            budget.decodingIssue = true;
                        }
                    } else {
                        entry.value = QString::fromUtf8(text.data(), qsizetype(text.size()));
                    }
                }
            }
        }
        entries.append(std::move(entry));
    }
    return entries;
}

}  // namespace

DatasetSnapshot snapshotDataset(DcmItem &dataset)
{
    Budget budget;
    auto attributes = snapshot(dataset, {}, 0, budget);
    return {std::move(attributes), budget.truncated};
}

DatasetSnapshot snapshotFileMetadata(DcmItem &dataset, std::stop_token stop)
{
    Budget budget{6000, 512 * 1024};
    budget.file = true;
    budget.stop = stop;
    auto attributes = snapshot(dataset, {}, 0, budget);
    return {std::move(attributes), budget.truncated, budget.decodingIssue};
}

DimseRecord snapshotCommand(DcmItem &command, Direction direction, QDateTime timestamp, quint16 context)
{
    const auto number = [&](const DcmTagKey &tag) -> std::optional<quint16> {
        Uint16 value = 0;
        return command.findAndGetUint16(tag, value).good() ? std::optional<quint16>(value) : std::nullopt;
    };
    DimseRecord result;
    result.direction = direction;
    result.timestamp = timestamp;
    result.context = context;
    result.id = number(DCM_MessageID);
    result.responseTo = number(DCM_MessageIDBeingRespondedTo);
    result.status = number(DCM_Status);
    if (const auto type = number(DCM_CommandDataSetType))
        result.datasetPresent = *type != 0x0101;
    OFString sop;
    if (command.findAndGetOFString(DCM_AffectedSOPClassUID, sop).bad())
        command.findAndGetOFString(DCM_RequestedSOPClassUID, sop);
    result.sopClass = QString::fromLatin1(sop.c_str());
    switch (number(DCM_CommandField).value_or(0)) {
    case 0x0001: result.command = "C-STORE-RQ"; break;
    case 0x8001: result.command = "C-STORE-RSP"; break;
    case 0x0010: result.command = "C-GET-RQ"; break;
    case 0x8010: result.command = "C-GET-RSP"; break;
    case 0x0020: result.command = "C-FIND-RQ"; break;
    case 0x8020: result.command = "C-FIND-RSP"; break;
    case 0x0021: result.command = "C-MOVE-RQ"; break;
    case 0x8021: result.command = "C-MOVE-RSP"; break;
    case 0x0030: result.command = "C-ECHO-RQ"; break;
    case 0x8030: result.command = "C-ECHO-RSP"; break;
    case 0x0fff: result.command = "C-CANCEL-RQ"; break;
    default: result.command = "Unrecognized DIMSE command"; break;
    }
    result.commandSet = snapshotDataset(command);
    if (direction == Direction::Outgoing) {
        // DCMTK returns the command copy before it serializes the group length.
        for (auto &attribute : result.commandSet.attributes) {
            if (attribute.tag == "(0000,0000)") {
                attribute.kind = Attribute::Unavailable;
                attribute.value = "<set by DCMTK during serialization>";
                break;
            }
        }
    }
    return result;
}

void DiagnosticSession::captureCommand(DcmItem &command, Direction direction, QDateTime timestamp,
                                       quint16 context, DcmItem *dataset, DcmItem *statusDetail)
{
    auto message = snapshotCommand(command, direction, timestamp, context);
    if (detailedCapture && dataset)
        message.dataset = snapshotDataset(*dataset);
    if (statusDetail)
        message.statusDetail = snapshotDataset(*statusDetail);
    record(std::move(message));
}

}  // namespace dingobench::dicom
