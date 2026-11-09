// SPDX-License-Identifier: GPL-3.0-only

#include "MainWindow.h"
#include "ProtocolInspector.h"
#include "dingobench/dicom/Dependencies.h"

#include <QAction>
#include <QButtonGroup>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QScreen>
#include <QSettings>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <array>

namespace dingobench {
using ui::Symbol;
using logging::Severity;

namespace {
struct Workspace {
    const char *name;
    Symbol symbol;
};

constexpr std::array workspaces{
    Workspace{"Dashboard", Symbol::Dashboard}, Workspace{"Verification", Symbol::Verification},
    Workspace{"Storage Send", Symbol::Send}, Workspace{"Storage Receive", Symbol::Receive},
    Workspace{"Query / Retrieve", Symbol::Query}, Workspace{"Worklist", Symbol::Worklist},
    Workspace{"DICOM Files", Symbol::Files}, Workspace{"Logs", Symbol::Logs},
    Workspace{"Profiles", Symbol::Profiles},
};
}

MainWindow::MainWindow(logging::Logger &logger, bool rememberLayout, QString profilePath)
    : m_logger(logger), m_verificationRunner(logger), m_storageRunner(logger), m_queryRunner(logger), m_moveRunner(logger), m_getRunner(logger),
      m_worklistRunner(logger), m_fileRunner(logger), m_receiver(logger), m_worklistServer(logger),
      m_profiles(std::move(profilePath)),
      m_rememberLayout(rememberLayout)
{
    connect(&m_receiver, &dicom::StorageReceiver::sessionFinished, this, &MainWindow::retainProtocol);
    connect(&m_worklistServer, &dicom::WorklistServer::sessionFinished, this, &MainWindow::retainProtocol);
    setWindowTitle(QCoreApplication::applicationName());
    setMinimumSize(790, 440);
    const QSize desktop = screen()->availableGeometry().size();
    resize(qMin(1240, desktop.width() - 28), qMin(760, desktop.height() - 48));

    if (!m_profiles.loadWarning().isEmpty())
        m_logger.log(Severity::Warning, "Profiles", m_profiles.loadWarning());

    auto *root = new QWidget;
    auto *shell = new QHBoxLayout(root);
    shell->setContentsMargins(0, 0, 0, 0);
    shell->setSpacing(0);
    setCentralWidget(root);

    auto *sidebar = new QWidget;
    sidebar->setObjectName("sidebar");
    sidebar->setFixedWidth(168);
    auto *navigation = new QVBoxLayout(sidebar);
    navigation->setContentsMargins(9, 11, 9, 12);
    navigation->setSpacing(3);
    auto *brand = new QLabel;
    const qreal density = devicePixelRatioF();
    QPixmap artwork = QPixmap(":/branding/startup-logo.png")
        .scaledToWidth(qRound(148 * density), Qt::SmoothTransformation);
    artwork.setDevicePixelRatio(density);
    brand->setPixmap(artwork);
    brand->setAccessibleName("DingoBench");
    navigation->addWidget(brand, 0, Qt::AlignHCenter);
    navigation->addSpacing(12);
    auto *buttons = new QButtonGroup(this);
    for (int index = 0; index < int(workspaces.size()); ++index) {
        auto *nav = ui::button(workspaces[index].name, workspaces[index].symbol);
        nav->setObjectName(QString("nav%1").arg(index));
        nav->setProperty("nav", true);
        nav->setCheckable(true);
        buttons->addButton(nav, index);
        navigation->addWidget(nav);
    }
    connect(buttons, &QButtonGroup::idClicked, this, &MainWindow::selectPage);
    navigation->addStretch();
    auto *about = ui::button("About", Symbol::Help, "aboutButton");
    navigation->addWidget(about);
    connect(about, &QPushButton::clicked, this, &MainWindow::showAbout);
    shell->addWidget(sidebar);

    auto *main = new QVBoxLayout;
    main->setContentsMargins(0, 0, 0, 0);
    main->setSpacing(0);
    shell->addLayout(main, 1);

    auto *toolbar = new QWidget;
    toolbar->setObjectName("toolbar");
    auto *tools = new QHBoxLayout(toolbar);
    tools->setContentsMargins(18, 7, 14, 7);
    tools->setSpacing(8);
    m_title = new QLabel;
    m_title->setObjectName("toolTitle");
    tools->addWidget(m_title, 1);
    tools->addWidget(new QLabel("Source"));
    tools->addWidget(profileSelector(true, "sourceProfile"));
    tools->addSpacing(3);
    tools->addWidget(new QLabel("Destination"));
    tools->addWidget(profileSelector(false, "destinationProfile"));
    auto *consoleToggle = ui::button({}, Symbol::Terminal, "toggleConsole");
    m_consoleToggle = consoleToggle;
    consoleToggle->setCheckable(true);
    consoleToggle->setToolTip("Show or hide diagnostic console");
    consoleToggle->setAccessibleName("Diagnostic console");
    tools->addWidget(consoleToggle);
    auto *settings = ui::button({}, Symbol::Settings, "settingsButton");
    settings->setToolTip("DingoBench settings");
    settings->setAccessibleName("Settings");
    tools->addWidget(settings);
    connect(settings, &QPushButton::clicked, this, &MainWindow::showSettings);
    main->addWidget(toolbar);

    m_splitter = new QSplitter(Qt::Vertical);
    m_splitter->setObjectName("workspaceSplitter");
    m_splitter->setChildrenCollapsible(false);
    m_pages = new QStackedWidget;
    m_pages->setObjectName("workspaces");
    m_pages->setMinimumHeight(185);
    for (auto builder : {&MainWindow::dashboard, &MainWindow::verification, &MainWindow::storageSend,
                         &MainWindow::storageReceive, &MainWindow::queryRetrieve, &MainWindow::worklist,
                         &MainWindow::dicomFiles, &MainWindow::logs, &MainWindow::profiles})
        m_pages->addWidget((this->*builder)());
    m_splitter->addWidget(m_pages);
    m_console = logExplorer(true);
    m_console->setMinimumHeight(108);
    m_splitter->addWidget(m_console);
    m_splitter->setStretchFactor(0, 1);
    main->addWidget(m_splitter, 1);

    m_stateLabel = new QLabel;
    m_stateLabel->setObjectName("operationState");
    m_statusIcon = new QLabel;
    statusBar()->addWidget(m_statusIcon);
    statusBar()->addWidget(m_stateLabel, 1);
    connect(consoleToggle, &QPushButton::toggled, this, &MainWindow::toggleConsole);

    refreshProfiles();
    QSettings preferences;
    m_splitter->setSizes({500, 150});
    if (m_rememberLayout) {
        const auto geometry = preferences.value("workspace/geometry").toByteArray();
        const auto splitter = preferences.value("workspace/splitter").toByteArray();
        if (!geometry.isEmpty() && !restoreGeometry(geometry))
            m_logger.log(Severity::Warning, "Workspace", "Stored window geometry could not be restored.");
        if (!splitter.isEmpty() && !m_splitter->restoreState(splitter))
            m_logger.log(Severity::Warning, "Workspace", "Stored splitter layout could not be restored.");
        m_consoleHeight = preferences.value("workspace/consoleHeight", 150).toInt();
        if (preferences.status() != QSettings::NoError)
            m_logger.log(Severity::Error, "Workspace", "Workspace preferences could not be read.");
    }
    toggleConsole(m_rememberLayout ? preferences.value("workspace/console", height() >= 660).toBool()
                                   : height() >= 660);
    selectPage(m_rememberLayout ? qBound(0, preferences.value("workspace/page", 0).toInt(),
                                         int(PageCount) - 1) : Dashboard);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (!m_verificationId.isNull() || !m_storageId.isNull() || !m_queryId.isNull() ||
        !m_moveId.isNull() || !m_getId.isNull() || !m_worklistId.isNull() || !m_fileId.isNull() || m_receiver.active() || m_worklistServer.busy()) {
        m_closeAfterOperations = true;
        if (!m_verificationId.isNull())
            findChild<QPushButton *>("cancelEcho")->click();
        if (!m_storageId.isNull())
            findChild<QPushButton *>("cancelStorage")->click();
        if (!m_queryId.isNull())
            findChild<QPushButton *>("cancelFind")->click();
        if (!m_worklistId.isNull())
            findChild<QPushButton *>("cancelWorklist")->click();
        if (!m_moveId.isNull())
            findChild<QPushButton *>("cancelMove")->click();
        if (!m_getId.isNull())
            findChild<QPushButton *>("cancelGet")->click();
        if (!m_fileId.isNull())
            findChild<QPushButton *>("cancelDicomInspection")->click();
        m_receiver.stop();
        m_worklistServer.stop();
        event->ignore();
        return;
    }
    if (!m_rememberLayout) {
        QMainWindow::closeEvent(event);
        return;
    }
    QSettings preferences;
    preferences.setValue("workspace/geometry", saveGeometry());
    preferences.setValue("workspace/splitter", m_splitter->saveState());
    preferences.setValue("workspace/console", !m_console->isHidden());
    preferences.setValue("workspace/consoleHeight", m_console->isHidden() ? m_consoleHeight
                                                                         : m_splitter->sizes().value(1));
    preferences.setValue("workspace/page", m_pages->currentIndex());
    preferences.sync();
    if (preferences.status() != QSettings::NoError)
        m_logger.log(Severity::Error, "Workspace", "Workspace preferences could not be saved.");
    QMainWindow::closeEvent(event);
}

void MainWindow::closeWhenOperationsFinish()
{
    if (m_closeAfterOperations && m_verificationId.isNull() && m_storageId.isNull() && m_queryId.isNull() &&
        m_moveId.isNull() && m_getId.isNull() && m_worklistId.isNull() && m_fileId.isNull() && !m_receiver.active() && m_receiver.state() != dicom::StorageReceiver::State::Starting &&
        !m_worklistServer.busy())
        QTimer::singleShot(0, this, [this] { close(); });
}

void MainWindow::selectPage(int page)
{
    m_pages->setCurrentIndex(page);
    findChild<QPushButton *>(QString("nav%1").arg(page))->setChecked(true);
    m_title->setText(workspaces.at(size_t(page)).name);
    refreshOperationStatus();
}

void MainWindow::refreshOperationStatus()
{
    if (!m_stateLabel)
        return;
    const auto &workspace = workspaces.at(size_t(m_pages->currentIndex()));
    ui::status(m_stateLabel, workspace.name, "info");
    m_statusIcon->setPixmap(ui::icon(workspace.symbol, QColor("#a9d3ff")).pixmap(16, 16));
}

void MainWindow::toggleConsole(bool visible)
{
    const bool wasHidden = m_console->isHidden();
    if (!visible && !wasHidden)
        m_consoleHeight = qMax(108, m_splitter->sizes().value(1));
    m_console->setVisible(visible);
    if (visible && wasHidden)
        m_splitter->setSizes({qMax(185, m_splitter->height() - m_consoleHeight), m_consoleHeight});
    const QSignalBlocker guard(m_consoleToggle);
    m_consoleToggle->setChecked(visible);
    m_consoleToggle->setIcon(ui::icon(visible ? Symbol::Hide : Symbol::Show));
}

void MainWindow::watch(std::function<void()> refresh)
{
    m_refresh.append(std::move(refresh));
}

QWidget *MainWindow::page(const QString &title, const QString &description, QVBoxLayout **body)
{
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    auto *surface = new QWidget;
    surface->setObjectName("page");
    auto *layout = new QVBoxLayout(surface);
    layout->setContentsMargins(18, 14, 18, 15);
    layout->setSpacing(10);
    auto *heading = new QLabel(title);
    heading->setProperty("heading", true);
    layout->addWidget(heading);
    layout->addWidget(ui::note(description));
    scroll->setWidget(surface);
    *body = layout;
    return scroll;
}

QComboBox *MainWindow::profileSelector(bool source, const char *objectName)
{
    auto *combo = new QComboBox;
    if (objectName)
        combo->setObjectName(objectName);
    combo->setAccessibleName(source ? "Source Profile" : "Destination Profile");
    combo->setPlaceholderText(source ? "Select a source profile" : "No destination profile");
    combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    combo->setMinimumContentsLength(13);
    (source ? m_sourceSelectors : m_destinationSelectors).append(combo);
    connect(combo, &QComboBox::currentIndexChanged, this, [this, source, combo](int index) {
        if (index >= 0)
            selectProfile(source, combo->itemData(index).toInt());
    });
    return combo;
}

QWidget *MainWindow::connectionRow(bool showSource, bool showDestination)
{
    auto *line = new QWidget;
    auto *layout = new QHBoxLayout(line);
    layout->setContentsMargins(0, 0, 0, 0);
    for (const bool source : {true, false}) {
        if (source ? !showSource : !showDestination)
            continue;
        auto *label = new QLabel(source ? "Source profile" : "Destination profile");
        auto *combo = profileSelector(source);
        label->setBuddy(combo);
        layout->addWidget(label);
        layout->addWidget(combo, 1);
    }
    if (showDestination)
        layout->addWidget(createDestinationButton());
    return line;
}

QPushButton *MainWindow::createDestinationButton()
{
    auto *create = ui::button("Create a destination profile", Symbol::Add, "createDestinationProfile");
    connect(create, &QPushButton::clicked, this, [this] { editProfile(false, 0); });
    const auto refresh = [this, create] { create->setVisible(!selectedProfile(false).id); };
    watch(refresh);
    refresh();
    return create;
}

const profiles::Profile &MainWindow::selectedProfile(bool source) const
{
    return m_profiles.active(profiles::kind(source));
}

void MainWindow::selectProfile(bool source, int id)
{
    const int previous = selectedProfile(source).id;
    const auto error = m_profiles.select(profiles::kind(source), id);
    if (!error.isEmpty())
        reportProfileError(error);
    else if (previous != id)
        m_logger.log(Severity::Info, "Profiles", source ? "Active source profile changed."
                                                      : "Active destination profile changed.");
    refreshProfiles();
}

void MainWindow::reportProfileError(const QString &error)
{
    statusBar()->showMessage(error, 9000);
    m_logger.log(Severity::Warning, "Profiles", "A profile change could not be applied.");
}

void MainWindow::refreshProfiles()
{
    for (const bool source : {true, false}) {
        const auto &profiles = m_profiles.entries(profiles::kind(source));
        const auto &selectors = source ? m_sourceSelectors : m_destinationSelectors;
        for (auto *combo : selectors) {
            const QSignalBlocker guard(combo);
            combo->clear();
            for (const auto &entry : profiles)
                combo->addItem(entry.name, entry.id);
            combo->setCurrentIndex(combo->findData(selectedProfile(source).id));
        }
    }
    for (const auto &refresh : std::as_const(m_refresh))
        refresh();
}

void MainWindow::showSettings()
{
    QDialog dialog(this);
    dialog.setWindowTitle("DingoBench settings");
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(ui::note("Window size, console height and the last workspace are saved locally. "
                               "Connection profiles and their active selections are saved locally."));
    auto *capture = new QCheckBox("Detailed Dataset Capture");
    capture->setChecked(QSettings().value("protocolInspector/detailedCapture", false).toBool());
    layout->addWidget(capture);
    layout->addWidget(ui::note("Capture attribute values for supported operations in memory. "
                              "C-ECHO carries no dataset. Binary payloads are omitted."));
    connect(capture, &QCheckBox::toggled, &dialog, [](bool enabled) {
        QSettings().setValue("protocolInspector/detailedCapture", enabled);
    });
    auto *clearProtocols = ui::button("Clear retained protocol sessions", Symbol::Clear, "clearProtocolHistory");
    clearProtocols->setEnabled(!m_protocolHistory.isEmpty());
    layout->addWidget(clearProtocols);
    connect(clearProtocols, &QPushButton::clicked, &dialog, [this, clearProtocols] {
        m_protocolHistory.clear();
        refreshProtocolControls();
        clearProtocols->setEnabled(false);
    });
    auto *reset = ui::button("Reset workspace layout", Symbol::Refresh);
    layout->addWidget(reset);
    connect(reset, &QPushButton::clicked, this, [this, &dialog] {
        resize(1240, 760);
        toggleConsole(true);
        m_splitter->setSizes({500, 150});
        selectPage(Dashboard);
        dialog.accept();
    });
    auto *actions = new QDialogButtonBox(QDialogButtonBox::Close);
    actions->button(QDialogButtonBox::Close)->setIcon(ui::icon(Symbol::Close));
    connect(actions, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(actions);
    dialog.setMinimumWidth(350);
    dialog.exec();
}

void MainWindow::addProtocolControls(QHBoxLayout *layout, QString operation, const char *comboName,
                                     const char *buttonName)
{
    auto *sessions = new QComboBox;
    sessions->setObjectName(QLatin1String(comboName));
    sessions->setAccessibleName(operation.isEmpty() ? "Retained diagnostic sessions" : "Retained " + operation + " sessions");
    sessions->setPlaceholderText("No captured sessions");
    layout->addWidget(sessions, 1);
    auto *details = ui::button("Protocol Details", Symbol::Search, buttonName);
    details->setEnabled(false);
    layout->addWidget(details);
    connect(sessions, &QComboBox::currentIndexChanged, details, [details](int index) {
        details->setEnabled(index >= 0);
    });
    connect(details, &QPushButton::clicked, this, [this, sessions] {
        const auto id = sessions->currentData().toUuid();
        const auto match = std::ranges::find(m_protocolHistory, id, &dicom::DiagnosticSession::id);
        if (match == m_protocolHistory.end())
            return;
        auto session = *match;
        for (const auto &event : m_logger.snapshot())
            session.addLog(event);
        ui::ProtocolInspector inspector(session, this);
        inspector.exec();
    });
    m_protocolControls.append({std::move(operation), sessions, details});
}

void MainWindow::retainProtocol(const dicom::DiagnosticSession &session)
{
    m_protocolHistory.prepend(session);
    if (m_protocolHistory.size() > 8)
        m_protocolHistory.removeLast();
    refreshProtocolControls(session.id);
}

void MainWindow::refreshProtocolControls(QUuid newest)
{
    refreshDashboardHistory();
    for (const auto &controls : m_protocolControls) {
        auto *combo = controls.sessions;
        const auto previous = combo->currentData().toUuid();
        const QSignalBlocker blocker(combo);
        combo->clear();
        for (const auto &session : m_protocolHistory) {
            if (controls.operation.isEmpty() || session.operation == controls.operation)
                combo->addItem(QString("%1 · %2 · %3 · %4").arg(
                    session.started.toLocalTime().toString("HH:mm:ss.zzz"), session.operation,
                    session.association.calledAe.isEmpty() ? QString("Unavailable") : session.association.calledAe,
                    session.result), session.id);
        }
        const int latest = combo->findData(newest);
        const int retained = combo->findData(previous);
        combo->setCurrentIndex(latest >= 0 ? latest : retained >= 0 ? retained : combo->count() ? 0 : -1);
        controls.details->setEnabled(combo->currentIndex() >= 0);
    }
}

void MainWindow::showAbout()
{
    QDialog about(this);
    about.setObjectName(QStringLiteral("aboutDialog"));
    about.setWindowTitle(tr("About DingoBench"));
    about.setFixedWidth(360);
    auto *layout = new QVBoxLayout(&about);
    layout->setContentsMargins(20, 18, 20, 16);
    layout->setSpacing(12);

    auto *wordmark = new QLabel;
    wordmark->setObjectName(QStringLiteral("aboutWordmark"));
    wordmark->setAccessibleName(tr("DingoBench wordmark"));
    wordmark->setPixmap(QIcon(QStringLiteral(":/branding/wordmark.png"))
                            .pixmap(QSize(270, 36), about.devicePixelRatioF()));
    layout->addWidget(wordmark, 0, Qt::AlignHCenter);
    auto *mascot = new QLabel;
    mascot->setObjectName(QStringLiteral("aboutMascot"));
    mascot->setAccessibleName(tr("DingoBench mascot holding a wrench"));
    mascot->setPixmap(QIcon(QStringLiteral(":/branding/mascot.png"))
                          .pixmap(QSize(106, 106), about.devicePixelRatioF()));
    layout->addWidget(mascot, 0, Qt::AlignHCenter);

    auto *text = new QLabel(tr("DingoBench %1\n\n"
                              "A DINGO family desktop tool for DICOM testing and troubleshooting.\n\n"
                              "Licensed under GNU GPL version 3 only (GPL-3.0-only).")
                                .arg(QCoreApplication::applicationVersion()));
    text->setTextFormat(Qt::PlainText);
    text->setAlignment(Qt::AlignCenter);
    text->setWordWrap(true);
    layout->addWidget(text);

    auto *links = new QLabel;
    links->setObjectName(QStringLiteral("aboutLinks"));
    links->setTextFormat(Qt::RichText);
    links->setAlignment(Qt::AlignCenter);
    links->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
    links->setOpenExternalLinks(true);
    links->setText(QStringLiteral("<a style='color:#9bcdf5' href='https://dingodicom.com/'>%1</a>"
                                  " &nbsp;·&nbsp; "
                                  "<a style='color:#9bcdf5' href='https://github.com/jpdevoto/dingo-bench'>%2</a>")
                       .arg(tr("DINGO DICOM").toHtmlEscaped(), tr("DingoBench on GitHub").toHtmlEscaped()));
    layout->addWidget(links);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok);
    buttons->setCenterButtons(true);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &about, &QDialog::accept);
    auto *dependencies = buttons->addButton(tr("Dependencies"), QDialogButtonBox::ActionRole);
    dependencies->setObjectName(QStringLiteral("showDependencies"));
    connect(dependencies, &QPushButton::clicked, &about, [this, &about] {
        const auto info = dicom::dependencies();
        QMessageBox diagnostics(&about);
        diagnostics.setObjectName(QStringLiteral("dependencyDiagnostics"));
        diagnostics.setWindowTitle(tr("Dependency diagnostics"));
        diagnostics.setTextFormat(Qt::PlainText);
        diagnostics.setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        diagnostics.setText(tr("Qt runtime %1\nDCMTK %2\nModules: %3\nData dictionary: %4\n"
                               "Association parameter probe: %5")
                                .arg(QString::fromLatin1(qVersion()), info.version,
                                     info.modules.join(QStringLiteral(", ")),
                                     info.dictionaryLoaded ? tr("loaded") : tr("unavailable"),
                                     info.associationParametersAvailable ? tr("passed") : tr("failed")));
        diagnostics.setStandardButtons(QMessageBox::Close);
        diagnostics.exec();
    });
    about.exec();
}

}  // namespace dingobench
