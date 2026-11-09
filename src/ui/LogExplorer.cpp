// SPDX-License-Identifier: GPL-3.0-only
#include "MainWindow.h"
#include "LogArchive.h"
#include "ProtocolInspector.h"

#include <QAbstractTableModel>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSettings>
#include <QShortcut>
#include <QSortFilterProxyModel>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace dingobench {
namespace {
using logging::Event;
using logging::Logger;
using logging::Severity;
using ui::Symbol;

enum Column { Time, Level, Component, Operation, Message, ColumnCount };

class LogModel final : public QAbstractTableModel {
public:
    using QAbstractTableModel::QAbstractTableModel;

    int rowCount(const QModelIndex &parent = {}) const override
    {
        return parent.isValid() ? 0 : int(m_rows.size());
    }

    int columnCount(const QModelIndex &parent = {}) const override
    {
        return parent.isValid() ? 0 : ColumnCount;
    }

    QVariant headerData(int section, Qt::Orientation direction, int role) const override
    {
        static const QStringList headings{"Timestamp", "Severity", "Component", "Operation", "Message"};
        if (direction == Qt::Horizontal && role == Qt::DisplayRole)
            return headings.value(section);
        return {};
    }

    const Event &event(int row) const { return m_rows.at(row); }
    const QList<Event> &events() const { return m_rows; }

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (!index.isValid() || index.row() >= rowCount())
            return {};
        const auto &entry = event(index.row());
        if (role == Qt::DisplayRole) {
            switch (index.column()) {
            case Time: return entry.timestampUtc.toLocalTime().toString("HH:mm:ss.zzz");
            case Level: return logging::severityName(entry.severity);
            case Component: return entry.component;
            case Operation: return entry.operation;
            case Message: return entry.message;
            }
        }
        if (role == Qt::ToolTipRole && index.column() == Time)
            return entry.timestampUtc.toLocalTime().toString(Qt::ISODateWithMs);
        if (index.column() == Level) {
            const QColor color = entry.severity == Severity::Error ? QColor("#ff9ba5")
                : entry.severity == Severity::Warning ? QColor("#ffc582") : QColor("#acc5df");
            if (role == Qt::ForegroundRole)
                return color;
            if (role == Qt::DecorationRole) {
                const auto symbol = entry.severity == Severity::Error ? Symbol::Error
                    : entry.severity == Severity::Warning ? Symbol::Warning : Symbol::Info;
                return ui::icon(symbol, color);
            }
        }
        return {};
    }

    void reset(const QList<Event> &events)
    {
        beginResetModel();
        m_rows = events;
        endResetModel();
    }

    void updateLive(const QList<Event> &events)
    {
        const auto first = events.isEmpty() ? 0 : events.first().sequence;
        const auto kept = std::lower_bound(m_rows.cbegin(), m_rows.cend(), first,
            [](const Event &event, quint64 value) { return event.sequence < value; });
        const int removed = events.isEmpty() ? rowCount() : int(kept - m_rows.cbegin());
        if (removed) {
            beginRemoveRows({}, 0, removed - 1);
            m_rows.remove(0, removed);
            endRemoveRows();
        }
        const quint64 last = m_rows.isEmpty() ? 0 : m_rows.last().sequence;
        const auto added = std::upper_bound(events.cbegin(), events.cend(), last,
            [](quint64 value, const Event &event) { return value < event.sequence; });
        if (added != events.cend()) {
            beginInsertRows({}, rowCount(), rowCount() + int(events.cend() - added) - 1);
            for (auto event = added; event != events.cend(); ++event)
                m_rows.append(*event);
            endInsertRows();
        }
    }

private:
    QList<Event> m_rows;
};

class LogFilter final : public QSortFilterProxyModel {
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;
    int levels = 15;
    QString component, operation, text;
    QRegularExpression expression;
    bool regex = false;

    const Event &event(int row) const
    {
        return static_cast<const LogModel *>(sourceModel())->event(mapToSource(index(row, 0)).row());
    }

    void refresh() { invalidateFilter(); }

protected:
    bool filterAcceptsRow(int row, const QModelIndex &) const override
    {
        const auto &event = static_cast<const LogModel *>(sourceModel())->event(row);
        if (!(levels & (1 << int(event.severity))) ||
            (!component.isEmpty() && event.component.compare(component, Qt::CaseInsensitive)) ||
            (!operation.isEmpty() && event.operation.compare(operation, Qt::CaseInsensitive)))
            return false;
        if (text.isEmpty())
            return true;
        for (const auto *field : {&event.message, &event.component, &event.operation}) {
            if (regex ? expression.match(*field).hasMatch() : field->contains(text, Qt::CaseInsensitive))
                return true;
        }
        return false;
    }
};

class Explorer final : public QWidget {
public:
    Explorer(Logger &logger, bool compact, bool remember, std::function<void()> collapse)
        : m_logger(logger), m_prefix(compact ? "console" : "log"), m_remember(remember)
    {
        setObjectName(compact ? "console" : "logExplorer");
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(compact ? 10 : 0, 5, compact ? 10 : 0, 6);
        layout->setSpacing(5);
        auto *tools = new QHBoxLayout;
        auto *filters = new QHBoxLayout;
        layout->addLayout(tools);
        layout->addLayout(filters);

        m_status = ui::note({});
        m_status->setObjectName(m_prefix + "Status");
        tools->addWidget(m_status, 1);
        m_follow = new QCheckBox("Follow");
        m_follow->setToolTip("Follow incoming events. Scrolling away from the end switches to manual review.");
        m_follow->setObjectName(m_prefix + "Follow");
        m_follow->setChecked(true);
        tools->addWidget(m_follow);
        m_pause = ui::button("Pause", Symbol::Pause);
        m_pause->setCheckable(true);
        m_pause->setToolTip("Freeze this view while the logger keeps collecting.");
        m_pause->setObjectName(m_prefix + "Pause");
        tools->addWidget(m_pause);
        m_expand = ui::button(compact ? QString() : QString("Details"), Symbol::Expand);
        m_expand->setToolTip("Show full details for the current event");
        m_expand->setAccessibleName("Event details");
        m_expand->setCheckable(true);
        tools->addWidget(m_expand);
        auto *actions = ui::button("Actions", Symbol::Down);
        tools->addWidget(actions);
        auto *menu = new QMenu(actions);
        actions->setMenu(menu);
        auto *clear = ui::button(compact ? QString() : QString("Clear"), Symbol::Clear);
        clear->setObjectName(m_prefix + "Clear");
        clear->setAccessibleName("Clear session records");
        clear->setToolTip("Clear retained live records, or close the displayed saved session.");
        tools->addWidget(clear);
        if (compact) {
            auto *hide = ui::button({}, Symbol::Collapse);
            hide->setToolTip("Collapse diagnostic console");
            hide->setAccessibleName("Collapse diagnostic console");
            tools->addWidget(hide);
            connect(hide, &QPushButton::clicked, this, std::move(collapse));
        }

        auto *levels = ui::button("Severity", Symbol::Filter);
        levels->setObjectName(m_prefix + "Severity");
        auto *levelsMenu = new QMenu(levels);
        levels->setMenu(levelsMenu);
        filters->addWidget(levels);
        for (int i = 0; i < 4; ++i) {
            auto *item = levelsMenu->addAction(logging::severityName(Severity(i)));
            item->setObjectName(m_prefix + "Level" + QString::number(i));
            item->setCheckable(true);
            item->setChecked(true);
            m_levels.append(item);
            connect(item, &QAction::toggled, this, [this] { applyFilter(); });
        }
        m_component = input(filters, "Component", "Component");
        m_operation = input(filters, "Operation ID", "Operation");
        m_operation->setAccessibleName("Operation or session identifier");
        m_search = input(filters, "Search messages", "Search", 2);
        m_search->addAction(ui::icon(Symbol::Search), QLineEdit::LeadingPosition);
        m_regex = new QCheckBox("Regex");
        m_regex->setObjectName(m_prefix + "Regex");
        m_regex->setToolTip("Interpret the message search as a case-insensitive regular expression");
        filters->addWidget(m_regex);
        auto *reset = ui::button({}, Symbol::Clear);
        reset->setObjectName(m_prefix + "Reset");
        reset->setAccessibleName("Clear filters");
        reset->setToolTip("Clear filters");
        filters->addWidget(reset);

        m_model = new LogModel(this);
        m_filter = new LogFilter(this);
        m_filter->setSourceModel(m_model);
        m_table = new QTableView;
        m_table->setObjectName(m_prefix + "Table");
        m_table->setModel(m_filter);
        m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
        m_table->setAlternatingRowColors(true);
        m_table->setShowGrid(false);
        m_table->setWordWrap(false);
        m_table->setVerticalScrollMode(QAbstractItemView::ScrollPerItem);
        m_table->verticalHeader()->hide();
        m_table->verticalHeader()->setDefaultSectionSize(26);
        auto *header = m_table->horizontalHeader();
        header->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        header->setStretchLastSection(true);
        for (int index = 0; index < 4; ++index)
            m_table->setColumnWidth(index, QList<int>{107, 105, 131, 112}.at(index));
        layout->addWidget(m_table, 1);
        m_details = new QPlainTextEdit;
        m_details->setObjectName(m_prefix + "Details");
        m_details->setAccessibleName("Selected event details");
        m_details->setReadOnly(true);
        m_details->setMaximumHeight(135);
        m_details->hide();
        layout->addWidget(m_details);

        auto *open = menu->addAction(ui::icon(Symbol::Folder), "Open saved session…", this, [this] { import(); });
        open->setObjectName(m_prefix + "Import");
        m_live = menu->addAction(ui::icon(Symbol::Terminal), "Return to live session", this, [this] {
            m_saved = false;
            m_model->reset({});
            setLiveControls();
            refreshLive();
        });
        m_live->setObjectName(m_prefix + "Live");
        menu->addSeparator();
        auto *details = menu->addAction("Include technical details in exports");
        details->setCheckable(true);
        auto *all = menu->addAction(ui::icon(Symbol::Export), "Export all retained…", this,
            [this, details] { exportEvents(m_saved ? m_model->events() : m_logger.snapshot(),
                                           details->isChecked()); });
        all->setObjectName(m_prefix + "ExportAll");
        auto *filtered = menu->addAction(ui::icon(Symbol::Export), "Export filtered…", this,
            [this, details] { exportEvents(visibleEvents(), details->isChecked()); });
        filtered->setObjectName(m_prefix + "Export");
        auto *exportSelected = menu->addAction(ui::icon(Symbol::Export), "Export selected…", this,
            [this, details] { exportEvents(selectedEvents(), details->isChecked()); });
        exportSelected->setObjectName(m_prefix + "ExportSelected");

        auto *context = new QMenu(m_table);
        auto *copyMessage = context->addAction(ui::icon(Symbol::Copy), "Copy selected message", this, [this] {
            const auto index = m_table->currentIndex();
            if (index.isValid())
                QApplication::clipboard()->setText(m_filter->event(index.row()).message);
        });
        copyMessage->setObjectName(m_prefix + "CopyMessage");
        auto *copy = context->addAction(ui::icon(Symbol::Copy), "Copy selected events", this,
                                        [this] { copyEvents(selectedEvents()); });
        copy->setObjectName(m_prefix + "Copy");
        auto *copyVisible = context->addAction(ui::icon(Symbol::Copy), "Copy visible events", this,
                                               [this] { copyEvents(visibleEvents()); });
        copyVisible->setObjectName(m_prefix + "CopyVisible");
        context->addSeparator();
        context->addAction(ui::icon(Symbol::Expand), "Show event details", this, [this] {
            m_expand->setChecked(true);
        });
        m_table->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(m_table, &QTableView::customContextMenuRequested, this, [this, context](QPoint point) {
            context->popup(m_table->viewport()->mapToGlobal(point));
        });
        auto *copyShortcut = new QShortcut(QKeySequence::Copy, m_table);
        copyShortcut->setContext(Qt::WidgetShortcut);
        connect(copyShortcut, &QShortcut::activated, copy, &QAction::trigger);
        auto *find = new QShortcut(QKeySequence::Find, this);
        find->setContext(Qt::WidgetWithChildrenShortcut);
        connect(find, &QShortcut::activated, this, [this] { m_search->setFocus(); m_search->selectAll(); });

        for (auto *input : {m_component, m_operation, m_search})
            connect(input, &QLineEdit::textChanged, this, [this] { applyFilter(); });
        connect(m_regex, &QCheckBox::toggled, this, [this] { applyFilter(); });
        connect(reset, &QPushButton::clicked, this, [this] {
            for (auto *input : {m_component, m_operation, m_search})
                input->clear();
            for (auto *level : m_levels)
                level->setChecked(true);
            m_regex->setChecked(false);
        });
        connect(m_table->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
            if (!m_adjusting && value < m_table->verticalScrollBar()->maximum())
                m_follow->setChecked(false);
        });
        connect(m_follow, &QCheckBox::toggled, this, [this](bool enabled) {
            if (enabled && !m_paused && !m_saved)
                m_table->scrollToBottom();
            updateStatus();
        });
        connect(m_pause, &QPushButton::toggled, this, [this](bool paused) {
            if (paused)
                refreshLive();
            m_paused = paused;
            m_pause->setText(paused ? "Resume" : "Pause");
            m_pause->setIcon(ui::icon(paused ? Symbol::Run : Symbol::Pause));
            refreshLive();
        });
        connect(m_expand, &QPushButton::toggled, m_details, &QWidget::setVisible);
        connect(m_table->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
                [this] { refreshDetails(); });
        connect(m_table, &QTableView::doubleClicked, this, [this] { m_expand->setChecked(true); });
        connect(clear, &QPushButton::clicked, this, [this] {
            if (m_saved) {
                m_live->trigger();
                return;
            }
            m_logger.clear();
            m_model->reset({});
            refreshLive();
        });
        m_timer.setSingleShot(true);
        m_timer.setInterval(35);
        connect(&m_timer, &QTimer::timeout, this, [this] { refreshLive(); });
        connect(&m_logger, &Logger::eventsChanged, this, [this] {
            if (!m_timer.isActive() && !m_saved)
                m_timer.start();
        });

        if (m_remember) {
            const QSettings settings;
            header->restoreState(settings.value(key("columns")).toByteArray());
            m_follow->setChecked(settings.value(key("follow"), true).toBool());
            const int flags = settings.value(key("severity"), 15).toInt();
            for (int i = 0; i < m_levels.size(); ++i)
                m_levels[i]->setChecked(flags & (1 << i));
            m_regex->setChecked(settings.value(key("regex"), false).toBool());
        }
        setLiveControls();
        refreshLive();
    }

    ~Explorer() override
    {
        if (!m_remember)
            return;
        QSettings settings;
        settings.setValue(key("columns"), m_table->horizontalHeader()->saveState());
        settings.setValue(key("follow"), m_follow->isChecked());
        settings.setValue(key("severity"), m_filter->levels);
        settings.setValue(key("regex"), m_regex->isChecked());
    }

private:
    QString key(const QString &setting) const { return "diagnostics/" + m_prefix + "/" + setting; }

    QLineEdit *input(QHBoxLayout *layout, const QString &placeholder, const QString &name, int stretch = 1)
    {
        auto *field = new QLineEdit;
        field->setObjectName(m_prefix + name);
        field->setPlaceholderText(placeholder);
        field->setAccessibleName(placeholder);
        field->setToolTip(name == "Search" ? "Search message, component and operation text" :
                          "Match this field exactly, ignoring case");
        field->setMaxLength(name == "Search" ? 256 : 128);
        layout->addWidget(field, stretch);
        return field;
    }

    void changeRows(const std::function<void()> &change)
    {
        const QPersistentModelIndex top(m_filter->index(m_table->rowAt(0), 0));
        m_adjusting = true;
        change();
        if (!m_saved && !m_paused && m_follow->isChecked())
            m_table->scrollToBottom();
        else if (top.isValid())
            m_table->scrollTo(top, QAbstractItemView::PositionAtTop);
        else if (m_filter->rowCount())
            m_table->scrollToTop();
        m_adjusting = false;
        refreshDetails();
        updateStatus();
    }

    void applyFilter()
    {
        m_filter->levels = 0;
        for (int i = 0; i < m_levels.size(); ++i)
            if (m_levels[i]->isChecked())
                m_filter->levels |= 1 << i;
        m_filter->component = m_component->text().trimmed();
        m_filter->operation = m_operation->text().trimmed();
        m_filter->text = m_search->text();
        m_filter->regex = m_regex->isChecked();
        m_error.clear();
        if (m_filter->regex && !m_filter->text.isEmpty()) {
            const QRegularExpression original(m_filter->text, QRegularExpression::CaseInsensitiveOption);
            m_filter->expression = QRegularExpression("(*LIMIT_MATCH=2000)(*LIMIT_DEPTH=100)" + m_filter->text,
                                                       QRegularExpression::CaseInsensitiveOption);
            if (!original.isValid() || !m_filter->expression.isValid()) {
                m_error = "Invalid regex: " + (original.isValid()
                    ? m_filter->expression.errorString() : original.errorString());
                m_filter->text.clear();
            }
        }
        changeRows([this] { m_filter->refresh(); });
    }

    void refreshLive()
    {
        if (m_saved)
            return;
        const auto snapshot = m_logger.session();
        m_retained = snapshot.events.size();
        m_evicted = snapshot.evicted;
        m_pending = m_paused ? snapshot.received - m_displayed : 0;
        if (m_paused) {
            updateStatus();
            return;
        }
        m_displayed = snapshot.received;
        changeRows([&] { m_model->updateLive(snapshot.events); });
    }

    void setLiveControls()
    {
        m_follow->setEnabled(!m_saved);
        m_pause->setEnabled(!m_saved);
        m_live->setEnabled(m_saved);
    }

    void updateStatus()
    {
        QString state = m_saved ? "Saved session · " + m_file
            : m_paused ? QString("Paused · %1 received").arg(m_pending)
            : m_follow->isChecked() ? "Following live" : "Manual scroll";
        state += QString(" · %1 visible / %2 retained").arg(m_filter->rowCount())
                    .arg(m_saved ? m_model->rowCount() : m_retained);
        const auto hidden = m_model->rowCount() - m_filter->rowCount();
        if (hidden)
            state += QString(" · %1 filtered").arg(hidden);
        if (!m_saved && m_evicted)
            state += QString(" · %1 evicted").arg(m_evicted);
        if (!m_filter->rowCount())
            state += m_model->rowCount() ? " · No matching events" : " · No events";
        if (!m_error.isEmpty())
            state += " · " + m_error;
        m_status->setToolTip(state);
        m_status->setTextFormat(Qt::PlainText);
        ui::status(m_status, state, m_error.isEmpty() ? "neutral" : "error");
    }

    void refreshDetails()
    {
        const auto index = m_table->currentIndex();
        const QString text = index.isValid() ? archive::describe(m_filter->event(index.row()), true)
            : QString("Select an event to inspect its full message and technical details.");
        if (m_details->toPlainText() != text)
            m_details->setPlainText(text);
    }

    QList<Event> visibleEvents() const
    {
        QList<Event> events;
        for (int row = 0; row < m_filter->rowCount(); ++row)
            events.append(m_filter->event(row));
        return events;
    }

    QList<Event> selectedEvents() const
    {
        auto selected = m_table->selectionModel()->selectedRows();
        std::ranges::sort(selected, {}, &QModelIndex::row);
        QList<Event> events;
        for (const auto &index : selected)
            events.append(m_filter->event(index.row()));
        return events;
    }

    void copyEvents(const QList<Event> &events)
    {
        QStringList blocks;
        for (const auto &event : events)
            blocks.append(archive::describe(event, false));
        if (!blocks.isEmpty())
            QApplication::clipboard()->setText(blocks.join("\n\n"));
    }

    void exportEvents(const QList<Event> &events, bool includeDetails)
    {
        QString format;
        auto path = QFileDialog::getSaveFileName(this, "Export diagnostic records", "dingobench-log.jsonl",
            "JSON Lines (*.jsonl);;Plain text (*.txt)", &format, QFileDialog::DontUseNativeDialog);
        if (path.isEmpty())
            return;
        const bool plain = path.endsWith(".txt", Qt::CaseInsensitive) ||
            (!path.endsWith(".jsonl", Qt::CaseInsensitive) && format.startsWith("Plain"));
        if (QFileInfo(path).suffix().isEmpty())
            path += plain ? ".txt" : ".jsonl";
        QString error;
        if (!archive::save(path, events, plain ? archive::Format::Text : archive::Format::JsonLines,
                           includeDetails, error))
            ui::status(m_status, "Export failed: " + error, "error");
        else
            ui::status(m_status, QString("Exported %1 events. ").arg(events.size()) + m_status->text(), "success");
    }

    void import()
    {
        const auto path = QFileDialog::getOpenFileName(this, "Open saved DingoBench session", {},
            "DingoBench exports (*.txt *.jsonl)", nullptr, QFileDialog::DontUseNativeDialog);
        if (path.isEmpty())
            return;
        QList<Event> events;
        QString error;
        if (!archive::load(path, events, error)) {
            ui::status(m_status, "Could not open saved session: " + error, "error");
            return;
        }
        m_pause->setChecked(false);
        m_saved = true;
        m_file = QFileInfo(path).fileName();
        setLiveControls();
        changeRows([&] { m_model->reset(events); });
    }

    Logger &m_logger;
    const QString m_prefix;
    const bool m_remember;
    QTimer m_timer;
    LogModel *m_model;
    LogFilter *m_filter;
    QTableView *m_table;
    QLabel *m_status;
    QLineEdit *m_search, *m_component, *m_operation;
    QCheckBox *m_follow, *m_regex;
    QPushButton *m_pause, *m_expand;
    QPlainTextEdit *m_details;
    QAction *m_live;
    QList<QAction *> m_levels;
    bool m_adjusting = false, m_saved = false, m_paused = false;
    qsizetype m_retained = 0;
    quint64 m_displayed = 0, m_pending = 0, m_evicted = 0;
    QString m_error, m_file;
};
}  // namespace

QWidget *MainWindow::logExplorer(bool compact)
{
    return new Explorer(m_logger, compact, m_rememberLayout, [this] { toggleConsole(false); });
}

QWidget *MainWindow::logs()
{
    QVBoxLayout *body;
    auto *widget = page("Logs", "Follow live diagnostics or inspect a saved DingoBench session.", &body);
    auto *diagnostics = new QHBoxLayout;
    addProtocolControls(diagnostics, {}, "diagnosticSessions", "openDiagnosticSession");
    auto *openReport = ui::button("Open saved report", Symbol::Files, "openDiagnosticReport");
    connect(openReport, &QPushButton::clicked, this, [this] { ui::openDiagnosticReport(this); });
    diagnostics->addWidget(openReport);
    body->addLayout(diagnostics);
    body->addWidget(logExplorer(false), 1);
    return widget;
}

}  // namespace dingobench
