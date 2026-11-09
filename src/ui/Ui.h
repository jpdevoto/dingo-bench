// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QIcon>
#include <QStringList>

class QApplication;
class QLabel;
class QPushButton;
class QTableWidget;
class QVBoxLayout;

namespace dingobench::ui {

enum class Symbol {
    Dashboard, Verification, Send, Receive, Query, Worklist, Files, Logs, Profiles,
    Settings, Terminal, Run, Stop, Add, Folder, Delete, Clear, Search, Filter,
    Export, Down, Right, Success, Warning, Error, Busy, Info, Edit, Copy, Save,
    Show, Hide, Refresh, Link, Expand, Collapse, Help, Check, Close, Checked, Unchecked,
    Pause, Back, Forward, ParentFolder, NewFolder, FileList, FileDetails,
    Count
};

QIcon icon(Symbol symbol, const QColor &color = {});
bool validateIcons(QString *failure = nullptr);
bool installTheme(QApplication &application);
QPushButton *button(const QString &text, Symbol symbol, const char *name = nullptr,
                    bool primary = false);
QLabel *note(const QString &text);
QVBoxLayout *section(const QString &title, QVBoxLayout *parent);
QTableWidget *table(const QStringList &headings);
void rows(QTableWidget *table, const QList<QStringList> &data);
void status(QLabel *label, const QString &message, const char *tone);

}  // namespace dingobench::ui
