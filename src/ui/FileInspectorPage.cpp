// SPDX-License-Identifier: GPL-3.0-only
#include "MainWindow.h"
#include "dingobench/dicom/FileInspection.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSortFilterProxyModel>
#include <QStandardItemModel>
#include <QTimer>
#include <QTreeView>
#include <QVBoxLayout>

#include <algorithm>
#include <memory>

namespace dingobench {
namespace {

enum RowRole { SectionRole = Qt::UserRole + 1, EmptyRole };
constexpr quint32 undefinedLength = 0xffffffff;

struct Search {
    QString text;
    bool exactTag = false, invalid = false;
};

QString compactName(QString name)
{
    name.remove(u' ');
    name.remove(u'\t');
    return name;
}

Search parseSearch(QString text)
{
    text = text.trimmed();
    static const QRegularExpression tag(R"(\A(?:\([\da-f]{4}\s*,\s*[\da-f]{4}\)|[\da-f]{4}\s*,\s*[\da-f]{4}|[\da-f]{8})\z)",
        QRegularExpression::CaseInsensitiveOption);
    if (tag.match(text).hasMatch()) {
        text.remove(u'(').remove(u')').remove(u',');
        text = compactName(text).toUpper();
        return {"(" + text.left(4) + "," + text.mid(4) + ")", true};
    }
    static const QRegularExpression numeric(R"(\A[0-9][0-9a-f]*\z)", QRegularExpression::CaseInsensitiveOption);
    const bool invalid = text.startsWith(u'(') || text.contains(u',') || numeric.match(text).hasMatch();
    return {compactName(text), false, invalid};
}

class FileAttributeFilter final : public QSortFilterProxyModel {
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;

    void apply(Search query, int section, bool hideEmpty)
    {
        m_query = std::move(query);
        m_section = section;
        m_hideEmpty = hideEmpty;
        invalidateFilter();
    }

protected:
    bool filterAcceptsRow(int row, const QModelIndex &parent) const override
    {
        const auto index = sourceModel()->index(row, 0, parent);
        if (m_query.invalid || (m_section && index.data(SectionRole).toInt() != m_section) ||
            (m_hideEmpty && index.data(EmptyRole).toBool()))
            return false;
        if (m_query.text.isEmpty())
            return true;
        if (m_query.exactTag)
            return index.data().toString() == m_query.text;
        return compactName(index.siblingAtColumn(1).data().toString()).contains(m_query.text, Qt::CaseInsensitive);
    }

private:
    Search m_query;
    int m_section = 0;
    bool m_hideEmpty = false;
};

QString lengthText(std::optional<quint32> length)
{
    if (!length)
        return {};
    return *length == undefinedLength ? QString("Undefined") : QString::number(*length);
}

QStandardItem *appendRow(QStandardItem *parent, const QStringList &cells, int section, bool empty = false)
{
    QList<QStandardItem *> row;
    for (const auto &text : cells) {
        auto *item = new QStandardItem(text);
        item->setEditable(false);
        if (!text.isEmpty())
            item->setToolTip("<qt>" + text.toHtmlEscaped() + "</qt>");
        row.append(item);
    }
    row.first()->setData(section, SectionRole);
    row.first()->setData(empty, EmptyRole);
    parent->appendRow(row);
    return row.first();
}

void appendAttributes(QStandardItem *parent, const QList<dicom::Attribute> &attributes, int section)
{
    for (const auto &entry : attributes) {
        auto *item = appendRow(parent, {entry.tag, entry.name, entry.vr, lengthText(entry.encodedLength), entry.value},
                               section, entry.kind == dicom::Attribute::Empty);
        appendAttributes(item, entry.children, section);
    }
}

}  // namespace

QWidget *MainWindow::dicomFiles()
{
    using ui::Symbol;
    using Status = dicom::OperationResult::Status;
    QVBoxLayout *body;
    auto *widget = page("DICOM Files", "Inspect local DICOM metadata and nested sequences.", &body);
    auto *summary = ui::section("File summary", body);
    auto *actions = new QHBoxLayout;
    summary->addLayout(actions);
    auto *open = ui::button("Open file", Symbol::Folder, "openDicomFile", true);
    auto *cancel = ui::button("Cancel", Symbol::Close, "cancelDicomInspection");
    actions->addWidget(open);
    actions->addWidget(cancel);
    actions->addStretch();
    cancel->setEnabled(false);

    auto *forms = new QHBoxLayout;
    summary->addLayout(forms);
    auto *left = new QFormLayout;
    auto *right = new QFormLayout;
    forms->addLayout(left, 1);
    forms->addLayout(right, 1);
    const auto field = [](QFormLayout *form, const QString &title, const char *name) {
        auto *label = ui::note("—");
        label->setObjectName(QLatin1String(name));
        label->setTextFormat(Qt::PlainText);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        form->addRow(title, label);
        return label;
    };
    auto *file = field(left, "File", "dicomFileName");
    auto *size = field(left, "File size", "dicomFileSize");
    auto *syntax = field(left, "Transfer syntax", "dicomFileSyntax");
    auto *charset = field(left, "Character set", "dicomFileCharset");
    auto *sopClass = field(right, "SOP Class UID", "dicomFileSopClass");
    auto *instance = field(right, "SOP Instance UID", "dicomFileInstance");
    auto *pixel = field(right, "Pixel Data", "dicomFilePixel");
    auto *status = ui::note("Open a local DICOM file to inspect its metadata.");
    status->setObjectName("dicomFileStatus");
    status->setTextFormat(Qt::PlainText);
    summary->addWidget(status);
    auto *issues = ui::note({});
    issues->setObjectName("dicomFileIssues");
    issues->setTextFormat(Qt::PlainText);
    issues->setTextInteractionFlags(Qt::TextSelectableByMouse);
    issues->hide();
    summary->addWidget(issues);

    auto *tags = ui::section("DICOM attributes", body);
    auto *filters = new QHBoxLayout;
    tags->addLayout(filters);
    auto *search = new QLineEdit;
    search->setObjectName("tagSearch");
    search->setPlaceholderText("Tag number or keyword");
    search->setAccessibleName("Search by tag number or keyword");
    search->setClearButtonEnabled(true);
    search->setMaxLength(160);
    search->addAction(ui::icon(Symbol::Search), QLineEdit::LeadingPosition);
    filters->addWidget(search, 1);
    auto *section = new QComboBox;
    section->setObjectName("dicomAttributeScope");
    section->setAccessibleName("Attribute section");
    section->addItems({"All elements", "File Meta Information", "Dataset"});
    filters->addWidget(section);
    auto *hideEmpty = new QCheckBox("Hide empty values");
    hideEmpty->setObjectName("dicomHideEmpty");
    filters->addWidget(hideEmpty);
    auto *searchStatus = ui::note({});
    searchStatus->setObjectName("dicomSearchStatus");
    tags->addWidget(searchStatus);

    auto *model = new QStandardItemModel(widget);
    model->setHorizontalHeaderLabels({"Tag", "Name / Keyword", "VR", "Length (bytes)", "Value"});
    auto *proxy = new FileAttributeFilter(widget);
    proxy->setSourceModel(model);
    proxy->setRecursiveFilteringEnabled(true);
    auto *tree = new QTreeView;
    tree->setObjectName("dicomTagTree");
    tree->setModel(proxy);
    tree->setUniformRowHeights(true);
    tree->setAlternatingRowColors(true);
    tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tree->setSelectionBehavior(QAbstractItemView::SelectRows);
    tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    tree->setMinimumHeight(240);
    tree->setColumnWidth(0, 142);
    tree->setColumnWidth(1, 200);
    tree->setColumnWidth(2, 52);
    tree->setColumnWidth(3, 112);
    tags->addWidget(tree, 1);

    auto *tools = new QHBoxLayout;
    tags->addLayout(tools);
    auto *expand = ui::button("Expand all", Symbol::Expand);
    auto *collapse = ui::button("Collapse all", Symbol::Collapse);
    auto *copy = ui::button("Copy selected tags", Symbol::Copy, "copyDicomTags");
    copy->setToolTip("Copies tag, keyword, VR, length, and displayed value. Values may contain patient information.");
    copy->setEnabled(false);
    for (auto *button : {expand, collapse, copy})
        tools->addWidget(button);
    tools->addStretch();
    connect(expand, &QPushButton::clicked, tree, &QTreeView::expandAll);
    connect(collapse, &QPushButton::clicked, tree, &QTreeView::collapseAll);
    connect(tree->selectionModel(), &QItemSelectionModel::selectionChanged, widget, [=] {
        const auto selected = tree->selectionModel()->selectedRows();
        copy->setEnabled(std::ranges::any_of(selected, [](const QModelIndex &index) {
            return !index.data().toString().isEmpty();
        }));
    });
    connect(copy, &QPushButton::clicked, widget, [tree] {
        QStringList lines;
        for (const auto &index : tree->selectionModel()->selectedRows()) {
            if (index.data().toString().isEmpty())
                continue;
            QStringList cells;
            for (int column = 0; column < 5; ++column)
                cells.append(index.siblingAtColumn(column).data().toString());
            lines.append(cells.join(u'\t'));
        }
        if (!lines.isEmpty())
            QApplication::clipboard()->setText(lines.join(u'\n'));
    });

    auto *debounce = new QTimer(widget);
    debounce->setSingleShot(true);
    debounce->setInterval(80);
    const auto filter = [=] {
        const auto query = parseSearch(search->text());
        proxy->apply(query, section->currentIndex(), hideEmpty->isChecked());
        searchStatus->setText(query.invalid ? QString("Use (0010,0010), 0010,0010, or 00100010 for a tag number.")
            : proxy->rowCount() == 0 && model->rowCount() ? QString("No matching elements.") : QString{});
        searchStatus->setVisible(!searchStatus->text().isEmpty());
        if (!query.text.isEmpty() && !query.invalid)
            tree->expandAll();
    };
    connect(search, &QLineEdit::textChanged, debounce, qOverload<>(&QTimer::start));
    connect(debounce, &QTimer::timeout, widget, filter);
    connect(section, &QComboBox::currentIndexChanged, widget, filter);
    connect(hideEmpty, &QCheckBox::toggled, widget, filter);
    filter();

    auto pending = std::make_shared<std::optional<dicom::FileInspection>>();
    connect(open, &QPushButton::clicked, widget, [=, this] {
        if (!m_fileId.isNull())
            return;
        const auto path = QFileDialog::getOpenFileName(this, "Open local DICOM file", {},
            "DICOM files (*.dcm *.dicom);;All files (*)", nullptr, QFileDialog::DontUseNativeDialog);
        if (path.isEmpty())
            return;
        pending->reset();
        file->setText(QFileInfo(path).fileName());
        file->setToolTip("<qt>" + path.toHtmlEscaped() + "</qt>");
        for (auto *label : {size, syntax, charset, sopClass, instance, pixel})
            label->setText("—");
        issues->hide();
        model->removeRows(0, model->rowCount());
        filter();
        ui::status(status, "Inspecting file…", "info");
        open->setEnabled(false);
        cancel->setEnabled(true);
        m_fileId = m_fileRunner.start("Inspect local metadata", dicom::inspectDicomFile(path,
            [pending](dicom::FileInspection result) { *pending = std::move(result); }), false, "DICOM Files");
    });
    connect(cancel, &QPushButton::clicked, widget, [=, this] {
        if (m_fileRunner.cancel(m_fileId)) {
            cancel->setEnabled(false);
            ui::status(status, "Cancelling…", "warning");
        }
    });
    connect(&m_fileRunner, &dicom::OperationRunner::finished, widget,
        [=, this](const QUuid &id, const dicom::OperationResult &result) {
            if (id != m_fileId)
                return;
            m_fileId = {};
            open->setEnabled(true);
            cancel->setEnabled(false);
            if (*pending && result.status != Status::Cancelled) {
                const auto &view = **pending;
                const auto set = [](QLabel *label, const QString &text) {
                    label->setText(text.isEmpty() ? QString("—") : text);
                };
                set(size, QString("%1 bytes").arg(view.size));
                set(syntax, view.syntaxName.isEmpty() ? view.syntaxUid : view.syntaxName + " · " + view.syntaxUid);
                set(charset, view.characterSet);
                set(sopClass, view.sopClass);
                set(instance, view.sopInstance);
                set(pixel, view.pixelData);
                issues->setText(view.issues.join(u'\n'));
                issues->setVisible(!view.issues.isEmpty());
                if (!view.meta.attributes.isEmpty() || view.datasetRead) {
                    QStandardItem sections;
                    auto *meta = appendRow(&sections, {{}, "File Meta Information", {}, {},
                        view.meta.attributes.isEmpty() ? QString("Absent") : QString{}}, 1);
                    auto *dataset = appendRow(&sections, {{}, "Dataset", {}, {},
                        view.datasetRead ? QString{} : QString("Unavailable")}, 2);
                    appendAttributes(meta, view.meta.attributes, 1);
                    appendAttributes(dataset, view.dataset.attributes, 2);
                    auto *root = model->invisibleRootItem();
                    root->appendRow(sections.takeRow(0));
                    root->appendRow(sections.takeRow(0));
                    tree->expandToDepth(0);
                }
            }
            pending->reset();
            filter();
            const bool cancelled = result.status == Status::Cancelled;
            const bool warning = result.status == Status::Warning;
            ui::status(status, result.error ? result.error->message : cancelled ? QString("Inspection cancelled.")
                : warning ? QString("Inspection completed with notes.") : QString("Metadata loaded."),
                result.error ? "error" : cancelled || warning ? "warning" : "success");
            closeWhenOperationsFinish();
        });
    return widget;
}

}  // namespace dingobench
