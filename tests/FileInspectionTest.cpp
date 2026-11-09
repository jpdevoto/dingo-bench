// SPDX-License-Identifier: GPL-3.0-only
#include "MainWindow.h"
#include "dingobench/dicom/FileInspection.h"

#include <dcmtk/config/osconfig.h>
#include <dcmtk/dcmdata/dcdeftag.h>
#include <dcmtk/dcmdata/dcfilefo.h>
#include <dcmtk/dcmdata/dcpixel.h>
#include <dcmtk/dcmdata/dcpixseq.h>
#include <dcmtk/dcmdata/dcpxitem.h>
#include <dcmtk/dcmdata/dcuid.h>

#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QFileDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeView>
#include <QtEndian>
#include <QtTest>

using namespace dingobench;
using namespace dingobench::dicom;
using Status = OperationResult::Status;

namespace {

constexpr auto instanceUid = "1.2.826.0.1.3680043.10.999.18.1";

QString fixturePath()
{
    return QDir(QCoreApplication::applicationDirPath()).filePath("file-inspection-synthetic-XXXXXX");
}

QByteArray digest(const QString &path)
{
    QFile file(path);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    return file.open(QIODevice::ReadOnly) && hash.addData(&file) ? hash.result() : QByteArray{};
}

const Attribute *find(const QList<Attribute> &attributes, const QString &tag)
{
    for (const auto &entry : attributes)
        if (entry.tag == tag)
            return &entry;
    return nullptr;
}

bool generate(const QString &path, E_TransferSyntax syntax, bool raw = false, bool large = false)
{
    DcmFileFormat file;
    auto &d = *file.getDataset();
    if (d.putAndInsertString(DCM_SpecificCharacterSet, "ISO_IR 192").bad() ||
        d.putAndInsertString(DCM_PatientName, QString("García^検査").toUtf8().constData()).bad() ||
        d.putAndInsertString(DCM_SOPClassUID, UID_CTImageStorage).bad() ||
        d.putAndInsertString(DCM_SOPInstanceUID, instanceUid).bad() ||
        d.putAndInsertString(DCM_AccessionNumber, "").bad() ||
        d.putAndInsertString(DCM_Modality, "CT").bad() ||
        d.putAndInsertString(DCM_PhotometricInterpretation, "MONOCHROME2").bad() ||
        d.putAndInsertUint16(DCM_Rows, large ? 6144 : 2).bad() ||
        d.putAndInsertUint16(DCM_Columns, large ? 8192 : 2).bad() ||
        d.putAndInsertUint16(DCM_SamplesPerPixel, 1).bad() ||
        d.putAndInsertUint16(DCM_BitsAllocated, 16).bad() ||
        d.putAndInsertUint16(DCM_BitsStored, 16).bad() ||
        d.putAndInsertUint16(DCM_HighBit, 15).bad() ||
        d.putAndInsertUint16(DCM_PixelRepresentation, 0).bad())
        return false;
    DcmItem *series = nullptr, *nested = nullptr;
    if (d.findOrCreateSequenceItem(DCM_ReferencedSeriesSequence, series, 0).bad() ||
        series->putAndInsertString(DCM_SpecificCharacterSet, "ISO_IR 100").bad() ||
        series->putAndInsertString(DCM_SeriesDescription, "S\xE9rie de test").bad() ||
        series->findOrCreateSequenceItem(DCM_ReferencedInstanceSequence, nested, 0).bad() ||
        nested->putAndInsertString(DCM_ReferencedSOPInstanceUID, instanceUid).bad())
        return false;
    if (syntax == EXS_JPEGProcess1) {
        auto sequence = std::make_unique<DcmPixelSequence>(DcmTag(DCM_PixelData, EVR_OB));
        auto table = std::make_unique<DcmPixelItem>(DCM_ItemTag);
        if (sequence->insert(table.get()).bad())
            return false;
        table.release();
        auto fragment = std::make_unique<DcmPixelItem>(DCM_ItemTag);
        // Structurally encapsulated bytes. The inspector must never invoke a codec.
        const Uint8 bytes[]{0xff, 0xd8, 0xff, 0xd9};
        if (fragment->putUint8Array(bytes, sizeof(bytes)).bad() || sequence->insert(fragment.get()).bad())
            return false;
        fragment.release();
        auto pixel = std::make_unique<DcmPixelData>(DcmTag(DCM_PixelData, EVR_OB));
        pixel->putOriginalRepresentation(syntax, nullptr, sequence.release());
        if (d.insert(pixel.get()).bad())
            return false;
        pixel.release();
    } else if (!large) {
        const Uint16 pixels[]{1, 2, 3, 4};
        if (d.putAndInsertUint16Array(DCM_PixelData, pixels, 4).bad())
            return false;
    }
    if (file.saveFile(OFFilename(QFile::encodeName(path).constData()), syntax, EET_UndefinedLength,
                      EGL_recalcGL, EPD_withoutPadding, 0, 0, raw ? EWM_dataset : EWM_fileformat).bad())
        return false;
    if (!large)
        return true;

    QFile output(path);
    if (!output.open(QIODevice::ReadWrite) || !output.seek(output.size()))
        return false;
    constexpr quint32 bytes = 96 * 1024 * 1024;
    QByteArray header = QByteArray::fromHex("e07f10004f57000000000000");
    qToLittleEndian(bytes, header.data() + 8);
    if (output.write(header) != header.size())
        return false;
    const auto endOfPixel = output.pos() + bytes;
    if (!output.resize(endOfPixel) || !output.seek(endOfPixel))
        return false;
    // A valid element after Pixel Data verifies that parsing continues after the seek.
    const auto trailing = QByteArray::fromHex("fcfffcff4f420000020000000000");
    return output.write(trailing) == trailing.size();
}

struct Observation {
    OperationResult result;
    FileInspection view;
};

Observation inspect(const QString &path)
{
    Observation observation;
    observation.result = inspectDicomFile(path, [&](FileInspection view) { observation.view = std::move(view); })(
        {}, [](DiagnosticEvent) {});
    return observation;
}

}  // namespace

class FileInspectionTest final : public QObject {
    Q_OBJECT

private slots:
    void validTransferSyntaxes_data()
    {
        QTest::addColumn<int>("transferSyntax");
        QTest::newRow("explicit-little") << int(EXS_LittleEndianExplicit);
        QTest::newRow("implicit-little") << int(EXS_LittleEndianImplicit);
        QTest::newRow("explicit-big") << int(EXS_BigEndianExplicit);
        QTest::newRow("encapsulated-jpeg") << int(EXS_JPEGProcess1);
    }

    void validTransferSyntaxes()
    {
        QFETCH(int, transferSyntax);
        const auto syntax = E_TransferSyntax(transferSyntax);
        QTemporaryDir fixture(fixturePath());
        QVERIFY(fixture.isValid());
        const auto ascii = fixture.filePath("fixture.dcm");
        QVERIFY(generate(ascii, syntax));
        const auto path = fixture.filePath("métadonnées-検査.dcm");
        QVERIFY(QFile::rename(ascii, path));
        const auto before = digest(path);
        QVERIFY(!before.isEmpty());
        const auto [result, view] = inspect(path);
        QVERIFY2(result.status == Status::Succeeded, qPrintable(view.issues.join(u'\n')));
        QVERIFY(view.datasetRead);
        QVERIFY(view.issues.isEmpty());
        QCOMPARE(view.syntaxUid, QString::fromLatin1(DcmXfer(syntax).getXferID()));
        QCOMPARE(view.sopClass, QString(UID_CTImageStorage));
        QCOMPARE(view.sopInstance, QString(instanceUid));
        QCOMPARE(view.characterSet, QString("ISO_IR 192"));
        QCOMPARE(view.size, QFileInfo(path).size());

        const auto *ts = find(view.meta.attributes, "(0002,0010)");
        QVERIFY(ts);
        QCOMPARE(ts->vr, QString("UI"));
        QVERIFY(ts->encodedLength.value_or(0) >= quint32(view.syntaxUid.size()));
        const auto *patient = find(view.dataset.attributes, "(0010,0010)");
        QVERIFY(patient);
        QCOMPARE(patient->name, QString("PatientName"));
        QCOMPARE(patient->vr, QString("PN"));
        QCOMPARE(patient->value.trimmed(), QString("García^検査"));

        const auto *series = find(view.dataset.attributes, "(0008,1115)");
        QVERIFY(series);
        QCOMPARE(series->kind, Attribute::Sequence);
        QCOMPARE(series->encodedLength.value_or(0), 0xffffffff);
        QCOMPARE(series->children.size(), 1);
        const auto &attributes = series->children.first().children;
        const auto *description = find(attributes, "(0008,103E)");
        QVERIFY(description);
        QCOMPARE(description->value.trimmed(), QString("Série de test"));
        const auto *nested = find(attributes, "(0008,114A)");
        QVERIFY(nested);
        QCOMPARE(nested->children.size(), 1);
        QVERIFY(find(nested->children.first().children, "(0008,1155)"));
        QVERIFY(view.pixelData.contains("payload omitted"));
        if (syntax == EXS_JPEGProcess1) {
            QVERIFY(view.pixelData.contains("1 fragment"));
            const auto *pixel = find(view.dataset.attributes, "(7FE0,0010)");
            QVERIFY(pixel);
            QCOMPARE(pixel->encodedLength.value_or(0), 0xffffffff);
        } else {
            QVERIFY(view.pixelData.contains("8 bytes"));
        }
        QCOMPARE(digest(path), before);
    }

    void largeDeferredPixelData()
    {
        QTemporaryDir fixture(fixturePath());
        QVERIFY(fixture.isValid());
        const auto path = fixture.filePath("large-synthetic.dcm");
        QVERIFY(generate(path, EXS_LittleEndianExplicit, false, true));
        const auto before = digest(path);
        QVERIFY(!before.isEmpty());
        const auto [result, view] = inspect(path);
        QVERIFY2(result.status == Status::Succeeded, qPrintable(view.issues.join(u'\n')));
        QVERIFY(view.size > 96 * 1024 * 1024);
        QVERIFY2(view.physicalBytesRead < 64 * 1024, qPrintable(QString::number(view.physicalBytesRead)));
        qInfo() << "Bytes physically read from the 96 MiB Pixel Data fixture:" << view.physicalBytesRead;
        const auto *pixel = find(view.dataset.attributes, "(7FE0,0010)");
        QVERIFY(pixel);
        QCOMPARE(pixel->encodedLength.value_or(0), 96 * 1024 * 1024);
        QCOMPARE(pixel->value, QString("<pixel payload omitted>"));
        QVERIFY(find(view.dataset.attributes, "(FFFC,FFFC)"));
        QCOMPARE(digest(path), before);
    }

    void rawMalformedAndTruncated()
    {
        QTemporaryDir fixture(fixturePath());
        QVERIFY(fixture.isValid());
        const auto raw = fixture.filePath("raw.dcm");
        QVERIFY(generate(raw, EXS_LittleEndianExplicit, true));
        const auto rawHash = digest(raw);
        const auto rawView = inspect(raw);
        QCOMPARE(rawView.result.status, Status::Warning);
        QVERIFY(rawView.view.datasetRead);
        QVERIFY(rawView.view.meta.attributes.isEmpty());
        QVERIFY(rawView.view.issues.join(' ').contains("inferred"));
        QCOMPARE(digest(raw), rawHash);

        const auto truncated = fixture.filePath("truncated.dcm");
        QVERIFY(generate(truncated, EXS_LittleEndianExplicit));
        QVERIFY(QFile(truncated).resize(QFileInfo(truncated).size() - 3));
        QFile bad(fixture.filePath("malformed.dcm"));
        QVERIFY(bad.open(QIODevice::WriteOnly));
        QVERIFY(bad.write("This is not a DICOM data set.") > 0);
        bad.close();
        for (const auto &path : {bad.fileName(), truncated}) {
            const auto before = digest(path);
            const auto checked = inspect(path);
            QCOMPARE(checked.result.status, Status::Failed);
            QVERIFY(checked.result.error);
            QVERIFY(!checked.view.datasetRead);
            QVERIFY(!checked.view.issues.isEmpty());
            QCOMPARE(digest(path), before);
        }
    }

    void reportsMismatchedSopIdentity()
    {
        QTemporaryDir fixture(fixturePath());
        QVERIFY(fixture.isValid());
        const auto path = fixture.filePath("mismatch.dcm");
        QVERIFY(generate(path, EXS_LittleEndianExplicit));
        QFile input(path);
        QVERIFY(input.open(QIODevice::ReadWrite));
        const QByteArray uid(instanceUid);
        const auto offset = input.readAll().indexOf(uid, 132);
        QVERIFY(offset >= 132);
        QVERIFY(input.seek(offset + uid.size() - 1));
        QCOMPARE(input.write("9"), 1);
        input.close();

        const auto before = digest(path);
        const auto [result, view] = inspect(path);
        QCOMPARE(result.status, Status::Warning);
        QVERIFY(view.datasetRead);
        QCOMPARE(view.sopInstance, QString(instanceUid));
        QVERIFY(view.issues.join(' ').contains("SOP UIDs disagree"));
        QCOMPARE(digest(path), before);
    }

    void workspaceSearchFilterAndCopy()
    {
        QTemporaryDir fixture(fixturePath());
        QVERIFY(fixture.isValid());
        const auto path = fixture.filePath("gui-synthetic.dcm");
        QVERIFY(generate(path, EXS_LittleEndianExplicit));
        const auto before = digest(path);
        logging::Logger logger;
        MainWindow window(logger, false);
        window.show();
        window.findChild<QPushButton *>("nav6")->click();

        QTimer picker;
        picker.setInterval(25);
        connect(&picker, &QTimer::timeout, &window, [&] {
            auto *dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            picker.stop();
            dialog->setDirectory(QFileInfo(path).absolutePath());
            auto *name = dialog->findChild<QLineEdit *>("fileNameEdit");
            if (!name) {
                dialog->reject();
                return;
            }
            name->setText(QDir::toNativeSeparators(path));
            static_cast<QDialog *>(dialog)->accept();
            if (dialog->isVisible())
                dialog->reject();
        });
        picker.start();
        window.findChild<QPushButton *>("openDicomFile")->click();
        auto *status = window.findChild<QLabel *>("dicomFileStatus");
        QTRY_COMPARE_WITH_TIMEOUT(status->text(), QString("Metadata loaded."), 5000);
        auto *tree = window.findChild<QTreeView *>("dicomTagTree");
        auto *model = tree->model();
        QCOMPARE(model->rowCount(), 2);
        QCOMPARE(model->headerData(3, Qt::Horizontal).toString(), QString("Length (bytes)"));
        QCOMPARE(tree->editTriggers(), QAbstractItemView::NoEditTriggers);
        auto *search = window.findChild<QLineEdit *>("tagSearch");
        auto *searchStatus = window.findChild<QLabel *>("dicomSearchStatus");
        search->setText("Patient Name");
        QTRY_COMPARE(model->rowCount(), 1);
        QCOMPARE(model->index(0, 1).data().toString(), QString("Dataset"));
        auto patient = model->index(0, 0, model->index(0, 0));
        QCOMPARE(patient.data().toString(), QString("(0010,0010)"));
        tree->selectionModel()->select(patient, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        auto *copy = window.findChild<QPushButton *>("copyDicomTags");
        QVERIFY(copy->isEnabled());
        copy->click();
        QVERIFY(QApplication::clipboard()->text().contains("(0010,0010)\tPatientName\tPN\t"));
        QVERIFY(QApplication::clipboard()->text().contains("García^検査"));

        search->setText("(0010,xyz)");
        QTRY_VERIFY(searchStatus->text().contains("for a tag number"));
        QCOMPARE(model->rowCount(), 0);
        search->setText("00100010");
        QTRY_COMPARE(model->rowCount(), 1);
        QCOMPARE(model->index(0, 0, model->index(0, 0)).data().toString(), QString("(0010,0010)"));
        search->setText("[");
        QTRY_COMPARE(searchStatus->text(), QString("No matching elements."));
        search->clear();
        QTRY_COMPARE(model->rowCount(), 2);
        window.findChild<QComboBox *>("dicomAttributeScope")->setCurrentIndex(1);
        QCOMPARE(model->rowCount(), 1);
        QCOMPARE(model->index(0, 1).data().toString(), QString("File Meta Information"));
        window.findChild<QComboBox *>("dicomAttributeScope")->setCurrentIndex(2);
        const auto prior = model->rowCount(model->index(0, 0));
        window.findChild<QCheckBox *>("dicomHideEmpty")->setChecked(true);
        QCOMPARE(model->rowCount(model->index(0, 0)), prior - 1);
        QVERIFY(window.close());
        QCOMPARE(digest(path), before);
        for (const auto &event : logger.snapshot())
            QVERIFY(!event.message.contains("García") && !event.details.contains("gui-synthetic.dcm"));
    }
};

QTEST_MAIN(FileInspectionTest)
#include "FileInspectionTest.moc"
