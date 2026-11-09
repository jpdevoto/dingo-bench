// SPDX-License-Identifier: GPL-3.0-only
#include "Ui.h"

#include <QApplication>
#include <QFile>
#include <QHeaderView>
#include <QIconEngine>
#include <QLabel>
#include <QPainter>
#include <QPixmapCache>
#include <QProxyStyle>
#include <QPushButton>
#include <QStyleOption>
#include <QStyleFactory>
#include <QSvgRenderer>
#include <QTableWidget>
#include <QVBoxLayout>

#include <array>

namespace dingobench::ui {
namespace {

constexpr std::array names{
    "dashboard", "network_check", "upload_file", "move_to_inbox", "manage_search",
    "event_note", "description", "list_alt", "tune", "settings", "terminal",
    "play_arrow", "stop", "add", "folder_open", "delete", "clear", "search",
    "filter_alt", "download", "keyboard_arrow_down", "keyboard_arrow_right",
    "check_circle", "warning", "error", "hourglass_top", "info", "edit", "content_copy",
    "save", "visibility", "visibility_off", "refresh", "link", "expand_more",
    "expand_less", "help", "check", "close", "check_box", "check_box_outline_blank",
    "pause", "arrow_back", "arrow_forward", "drive_folder_upload", "create_new_folder",
    "list", "table_chart",
};
static_assert(names.size() == static_cast<size_t>(Symbol::Count));

QString resource(Symbol symbol)
{
    return QStringLiteral(":/material/%1.svg").arg(QLatin1String(names.at(size_t(symbol))));
}

class SymbolEngine final : public QIconEngine {
public:
    SymbolEngine(Symbol symbol, QColor color) : m_symbol(symbol), m_color(std::move(color)) {}
    QIconEngine *clone() const override { return new SymbolEngine(m_symbol, m_color); }

    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override
    {
        if (size.isEmpty())
            return {};
        const QColor ink = mode == QIcon::Disabled ? QColor("#788492")
            : state == QIcon::On ? QColor("#ffad71")
            : m_color.isValid() ? m_color : QColor("#b8c9da");
        const QString key = QStringLiteral("dingo-symbol/%1/%2/%3/%4")
            .arg(int(m_symbol)).arg(ink.name()).arg(size.width()).arg(size.height());
        QPixmap result;
        if (QPixmapCache::find(key, &result))
            return result;
        QSvgRenderer renderer(resource(m_symbol));
        if (!renderer.isValid())
            return {};
        result = QPixmap(size);
        result.fill(Qt::transparent);
        QPainter painter(&result);
        painter.setRenderHint(QPainter::Antialiasing);
        renderer.render(&painter);
        painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        painter.fillRect(result.rect(), ink);
        painter.end();
        QPixmapCache::insert(key, result);
        return result;
    }

    QPixmap scaledPixmap(const QSize &size, QIcon::Mode mode, QIcon::State state,
                         qreal scale) override
    {
#if QT_VERSION < QT_VERSION_CHECK(6, 8, 0)
        QPixmap result = pixmap(size, mode, state);
#else
        QPixmap result = pixmap(size * scale, mode, state);
#endif
        result.setDevicePixelRatio(scale);
        return result;
    }

    void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode, QIcon::State state) override
    {
        const qreal scale = painter->device()->devicePixelRatioF();
        QPixmap result = pixmap(rect.size() * scale, mode, state);
        result.setDevicePixelRatio(scale);
        painter->drawPixmap(rect.topLeft(), result);
    }

private:
    Symbol m_symbol;
    QColor m_color;
};

class WorkspaceStyle final : public QProxyStyle {
public:
    WorkspaceStyle() : QProxyStyle(QStyleFactory::create(QStringLiteral("Fusion"))) {}

    QIcon standardIcon(StandardPixmap kind, const QStyleOption *option,
                       const QWidget *widget) const override
    {
        switch (kind) {
        case SP_ArrowBack:
        case SP_ArrowForward: {
            const auto direction = option ? option->direction
                : widget ? widget->layoutDirection() : QApplication::layoutDirection();
            const bool pointsLeft = (kind == SP_ArrowBack) != (direction == Qt::RightToLeft);
            return icon(pointsLeft ? Symbol::Back : Symbol::Forward);
        }
        case SP_DirIcon:
        case SP_DirOpenIcon: return icon(Symbol::Folder);
        case SP_FileIcon: return icon(Symbol::Files);
        case SP_FileDialogToParent: return icon(Symbol::ParentFolder);
        case SP_FileDialogNewFolder: return icon(Symbol::NewFolder);
        case SP_FileDialogListView: return icon(Symbol::FileList);
        case SP_FileDialogDetailedView:
        case SP_FileDialogContentsView: return icon(Symbol::FileDetails);
        case SP_DialogOpenButton: return icon(Symbol::Folder);
        case SP_DialogSaveButton: return icon(Symbol::Save);
        case SP_DialogOkButton:
        case SP_DialogYesButton: return icon(Symbol::Check);
        case SP_DialogCloseButton:
        case SP_DialogNoButton:
        case SP_DialogCancelButton: return icon(Symbol::Close);
        default: return QProxyStyle::standardIcon(kind, option, widget);
        }
    }

    void drawPrimitive(PrimitiveElement element, const QStyleOption *option, QPainter *painter,
                       const QWidget *widget) const override
    {
        Symbol symbol;
        switch (element) {
        case PE_IndicatorArrowDown: symbol = Symbol::Down; break;
        case PE_IndicatorArrowUp: symbol = Symbol::Collapse; break;
        case PE_IndicatorArrowRight: symbol = Symbol::Right; break;
        case PE_IndicatorCheckBox:
            symbol = option->state & State_On ? Symbol::Checked : Symbol::Unchecked;
            break;
        case PE_IndicatorBranch:
            if (!(option->state & State_Children))
                return;
            symbol = option->state & State_Open ? Symbol::Down : Symbol::Right;
            break;
        default:
            QProxyStyle::drawPrimitive(element, option, painter, widget);
            return;
        }
        const int extent = qMin(16, qMin(option->rect.width(), option->rect.height()));
        const QRect bounds(option->rect.center() - QPoint(extent / 2, extent / 2),
                           QSize(extent, extent));
        icon(symbol).paint(painter, bounds, Qt::AlignCenter,
                           option->state & State_Enabled ? QIcon::Normal : QIcon::Disabled);
    }
};

}  // namespace

QIcon icon(Symbol symbol, const QColor &color)
{
    return QIcon(new SymbolEngine(symbol, color));
}

bool validateIcons(QString *failure)
{
    for (int n = 0; n < int(Symbol::Count); ++n) {
        const auto symbol = Symbol(n);
        for (qreal scale : {1.0, 1.5, 2.0}) {
            for (const auto [mode, state] : {std::pair{QIcon::Normal, QIcon::Off},
                    std::pair{QIcon::Disabled, QIcon::Off}, std::pair{QIcon::Active, QIcon::Off},
                    std::pair{QIcon::Normal, QIcon::On}}) {
                const auto image = icon(symbol).pixmap({20, 20}, scale, mode, state).toImage();
                if (!image.isNull() && image.size() == QSize(20, 20) * scale &&
                    image.hasAlphaChannel()) {
                    bool visible = false;
                    for (int y = 0; y < image.height() && !visible; ++y)
                        for (int x = 0; x < image.width() && !visible; ++x)
                            visible = qAlpha(image.pixel(x, y)) > 0;
                    if (visible)
                        continue;
                }
                if (failure)
                    *failure = resource(symbol);
                return false;
            }
        }
    }
    return true;
}

bool installTheme(QApplication &application)
{
    application.setStyle(new WorkspaceStyle);
    QPalette palette;
    palette.setColor(QPalette::Window, "#141d28");
    palette.setColor(QPalette::WindowText, "#ecf2f8");
    palette.setColor(QPalette::Base, "#101822");
    palette.setColor(QPalette::AlternateBase, "#182432");
    palette.setColor(QPalette::Text, "#ecf2f8");
    palette.setColor(QPalette::Button, "#202e3e");
    palette.setColor(QPalette::ButtonText, "#ecf2f8");
    palette.setColor(QPalette::Highlight, "#304961");
    palette.setColor(QPalette::HighlightedText, "#ffffff");
    palette.setColor(QPalette::PlaceholderText, "#9fb1c4");
    palette.setColor(QPalette::Disabled, QPalette::Text, "#788492");
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, "#788492");
    palette.setColor(QPalette::ToolTipBase, "#263749");
    palette.setColor(QPalette::ToolTipText, "#ecf2f8");
    application.setPalette(palette);
    QFile css(QStringLiteral(":/workspace/workspace.qss"));
    if (!css.open(QIODevice::ReadOnly))
        return false;
    application.setStyleSheet(QString::fromUtf8(css.readAll()));
    return css.error() == QFileDevice::NoError;
}

QPushButton *button(const QString &text, Symbol symbol, const char *name, bool primary)
{
    auto *widget = new QPushButton(icon(symbol), text);
    widget->setIconSize({18, 18});
    widget->setProperty("primary", primary);
    if (name)
        widget->setObjectName(QLatin1String(name));
    return widget;
}

QLabel *note(const QString &text)
{
    auto *label = new QLabel(text);
    label->setProperty("muted", true);
    label->setWordWrap(true);
    return label;
}

QVBoxLayout *section(const QString &title, QVBoxLayout *parent)
{
    auto *frame = new QWidget;
    frame->setProperty("section", true);
    auto *layout = new QVBoxLayout(frame);
    layout->setContentsMargins(14, 11, 14, 13);
    layout->setSpacing(9);
    auto *heading = new QLabel(title);
    heading->setProperty("sectionHeading", true);
    layout->addWidget(heading);
    if (parent)
        parent->addWidget(frame);
    return layout;
}

QTableWidget *table(const QStringList &headings)
{
    auto *widget = new QTableWidget(0, int(headings.size()));
    widget->setHorizontalHeaderLabels(headings);
    widget->setAlternatingRowColors(true);
    widget->setSelectionBehavior(QAbstractItemView::SelectRows);
    widget->setEditTriggers(QAbstractItemView::NoEditTriggers);
    widget->setShowGrid(false);
    widget->setWordWrap(false);
    widget->verticalHeader()->hide();
    widget->verticalHeader()->setDefaultSectionSize(27);
    widget->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    widget->horizontalHeader()->setStretchLastSection(true);
    widget->horizontalHeader()->setDefaultSectionSize(112);
    widget->setMinimumHeight(114);
    return widget;
}

void rows(QTableWidget *table, const QList<QStringList> &data)
{
    table->setRowCount(int(data.size()));
    for (int row = 0; row < data.size(); ++row)
        for (int column = 0; column < data[row].size(); ++column)
            table->setItem(row, column, new QTableWidgetItem(data[row][column]));
}

void status(QLabel *label, const QString &message, const char *tone)
{
    label->setText(message);
    label->setProperty("tone", QLatin1String(tone));
    label->style()->unpolish(label);
    label->style()->polish(label);
}

}  // namespace dingobench::ui
