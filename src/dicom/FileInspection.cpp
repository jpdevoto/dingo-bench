// SPDX-License-Identifier: GPL-3.0-only
#include "dingobench/dicom/FileInspection.h"
#include "DicomFileInput.h"
#include "FileDatasetSnapshot.h"

#include <dcmtk/dcmdata/dcdeftag.h>
#include <dcmtk/dcmdata/dcmetinf.h>
#include <dcmtk/dcmdata/dcpixel.h>
#include <dcmtk/dcmdata/dcpixseq.h>
#include <dcmtk/dcmdata/dcvrui.h>
#include <dcmtk/dcmdata/dcxfer.h>

#include <QFileInfo>

namespace dingobench::dicom {
namespace {

constexpr Uint32 maxImmediateValue = 8192;

QString shortValue(DcmItem &item, const DcmTagKey &tag)
{
    DcmElement *element = nullptr;
    OFString value;
    if (item.findAndGetElement(tag, element).bad() || !element->valueLoaded() ||
        element->getLengthField() > maxImmediateValue || element->getOFStringArray(value).bad())
        return {};
    return QString::fromLatin1(value.data(), qsizetype(value.size())).trimmed();
}

bool validUid(const QString &uid)
{
    return !uid.isEmpty() && DcmUniqueIdentifier::checkStringValue(uid.toLatin1().constData(), "1").good();
}

QString pixelSummary(DcmDataset &dataset)
{
    DcmElement *element = nullptr;
    for (const auto &tag : {DCM_PixelData, DCM_FloatPixelData, DCM_DoubleFloatPixelData})
        if (dataset.findAndGetElement(tag, element).good())
            break;
    if (!element)
        return "Pixel Data absent";
    const auto length = element->getLengthField();
    QString summary;
    auto *pixel = dynamic_cast<DcmPixelData *>(element);
    if (pixel && DcmXfer(dataset.getOriginalXfer()).usesEncapsulatedFormat()) {
        E_TransferSyntax syntax = EXS_Unknown;
        const DcmRepresentationParameter *parameters = nullptr;
        pixel->getOriginalRepresentationKey(syntax, parameters);
        DcmPixelSequence *sequence = nullptr;
        if (pixel->getEncapsulatedRepresentation(syntax, parameters, sequence).good() && sequence->card() > 0)
            summary = QString("Encapsulated Pixel Data · %1 fragment(s)").arg(sequence->card() - 1);
    }
    if (summary.isEmpty())
        summary = length == DCM_UndefinedLength ? QString("Pixel Data · undefined length")
                                                : QString("Native Pixel Data · %1 bytes").arg(length);
    const auto shortNumber = [&](const DcmTagKey &tag, Uint16 &value) {
        DcmElement *number = nullptr;
        return dataset.findAndGetElement(tag, number).good() && number->valueLoaded() &&
            number->getLengthField() == sizeof(Uint16) && number->getUint16(value).good();
    };
    Uint16 rows = 0, columns = 0, bits = 0;
    if (shortNumber(DCM_Rows, rows) && shortNumber(DCM_Columns, columns))
        summary += QString(" · %1 × %2").arg(columns).arg(rows);
    const auto frames = shortValue(dataset, DCM_NumberOfFrames);
    if (!frames.isEmpty() && frames.size() <= 12)
        summary += " · " + frames + " frame(s)";
    if (shortNumber(DCM_BitsAllocated, bits))
        summary += QString(" · %1 bits allocated").arg(bits);
    return summary + " · payload omitted";
}

void checkIdentity(DcmItem &meta, DcmDataset &dataset, FileInspection &view)
{
    const auto metaClass = shortValue(meta, DCM_MediaStorageSOPClassUID);
    const auto metaInstance = shortValue(meta, DCM_MediaStorageSOPInstanceUID);
    const auto datasetClass = shortValue(dataset, DCM_SOPClassUID);
    const auto datasetInstance = shortValue(dataset, DCM_SOPInstanceUID);
    view.sopClass = datasetClass.isEmpty() ? metaClass : datasetClass;
    view.sopInstance = datasetInstance.isEmpty() ? metaInstance : datasetInstance;
    if (!validUid(datasetClass))
        view.issues.append("Dataset SOP Class UID is missing or invalid.");
    if (!validUid(datasetInstance))
        view.issues.append("Dataset SOP Instance UID is missing or invalid.");
    if (!meta.card())
        return;
    if (!validUid(metaClass) || !validUid(metaInstance))
        view.issues.append("File Meta Information has a missing or invalid Media Storage SOP UID.");
    if ((!metaClass.isEmpty() && !datasetClass.isEmpty() && metaClass != datasetClass) ||
        (!metaInstance.isEmpty() && !datasetInstance.isEmpty() && metaInstance != datasetInstance))
        view.issues.append("File Meta Information and dataset SOP UIDs disagree.");
}

}  // namespace

Operation inspectDicomFile(QString path, std::function<void(FileInspection)> deliver)
{
    return [path = std::move(path), deliver = std::move(deliver)](std::stop_token stop, const ReportDiagnostic &report) {
        const QFileInfo input(path);
        FileInspection view;
        view.size = input.size();
        const auto modified = input.lastModified();
        FileReadControl control{stop};
        const auto finish = [&](OperationResult result) {
            if (stop.stop_requested())
                return OperationResult::cancelled();
            view.physicalBytesRead = control.physicalBytesRead;
            if (deliver)
                deliver(std::move(view));
            return result;
        };
        const auto fail = [&](const QString &message) {
            view.issues.append(message);
            return finish(OperationResult::failed({ErrorCode::InvalidInput, message}));
        };
        if (!input.isFile() || !input.isReadable() || !view.size)
            return fail("Choose a readable, non-empty local file.");

        // Read the uncompressed File Meta Information first. A compressed
        // dataset stream cannot safely defer large values by file offset.
        FileInput headerStream(path, 0, &control);
        DcmMetaInfo header;
        header.transferInit();
        const auto headerRead = header.read(headerStream, EXS_Unknown, EGL_noChange, maxImmediateValue);
        header.transferEnd();
        if (headerRead.bad() || headerStream.status().bad())
            return fail(control.limitReached ? QString("File Meta Information exceeds the inspection read budget.")
                                             : QString("File Meta Information is malformed or truncated."));
        E_TransferSyntax declaredSyntax = EXS_Unknown;
        if (header.card()) {
            view.meta = snapshotFileMetadata(header, stop);
            view.syntaxUid = shortValue(header, DCM_TransferSyntaxUID);
            view.sopClass = shortValue(header, DCM_MediaStorageSOPClassUID);
            view.sopInstance = shortValue(header, DCM_MediaStorageSOPInstanceUID);
            const DcmXfer declared(view.syntaxUid.toLatin1().constData());
            if (!validUid(view.syntaxUid) || declared.getXfer() == EXS_Unknown)
                return fail("File Meta Information has a missing or unsupported Transfer Syntax UID.");
            declaredSyntax = declared.getXfer();
            view.syntaxName = QString::fromLatin1(declared.getXferName());
            if (declared.getStreamCompression() != ESC_none) {
                view.issues.append("Dataset stream compression prevents safe deferred reads. Showing File Meta Information only.");
                OperationResult result;
                result.status = OperationResult::Status::Warning;
                report({logging::Severity::Warning, "Showing File Meta Information for a compressed dataset stream."});
                return finish(result);
            }
        }

        DcmDataset dataset;
        dataset.transferInit();
        const auto parsed = dataset.read(headerStream, declaredSyntax, EGL_noChange, maxImmediateValue);
        dataset.transferEnd();
        if (parsed.bad() || headerStream.status().bad())
            return fail(control.limitReached ? QString("The dataset exceeds the metadata inspection read budget.")
                                             : QString("The DICOM dataset is malformed or truncated."));
        const DcmXfer syntax(dataset.getOriginalXfer());
        if (!dataset.card() || syntax.getXfer() == EXS_Unknown)
            return fail("No recognizable DICOM dataset was found.");
        if (!header.card()) {
            bool known = false;
            for (unsigned long i = 0; i < dataset.card() && !known; ++i) {
                const auto &key = dataset.getElement(i)->getTag();
                const DcmTag dictionaryTag(key.getXTag());
                known = !dictionaryTag.getVR().isUnknown();
            }
            if (!known)
                return fail("The file has no File Meta Information or recognizable DICOM attributes.");
            view.issues.append("File Meta Information is absent. The transfer syntax was inferred from the dataset.");
        }
        if (view.syntaxUid.isEmpty())
            view.syntaxUid = QString::fromLatin1(syntax.getXferID());
        if (view.syntaxName.isEmpty())
            view.syntaxName = QString::fromLatin1(syntax.getXferName());
        if (view.syntaxUid != QString::fromLatin1(syntax.getXferID()))
            view.issues.append("The parsed transfer syntax differs from the File Meta Information.");

        checkIdentity(header, dataset, view);
        view.characterSet = shortValue(dataset, DCM_SpecificCharacterSet);
        if (view.characterSet.isEmpty())
            view.characterSet = "Default repertoire (ISO IR 6)";
        view.pixelData = pixelSummary(dataset);
        view.dataset = snapshotFileMetadata(dataset, stop);
        const QFileInfo after(path);
        if (after.size() != view.size || after.lastModified() != modified) {
            view.dataset = {};
            return fail("The file changed while it was being inspected. Open it again.");
        }
        view.datasetRead = true;
        if (view.meta.truncated || view.dataset.truncated)
            view.issues.append("The metadata display limit was reached. Some attributes or values were omitted.");
        if (view.dataset.decodingIssue)
            view.issues.append("Some textual values could not be decoded with their declared character set.");
        OperationResult result;
        if (!view.issues.isEmpty())
            result.status = OperationResult::Status::Warning;
        return finish(result);
    };
}

}  // namespace dingobench::dicom
