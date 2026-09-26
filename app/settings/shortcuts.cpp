#include "shortcuts.h"

#include <QSettings>

namespace Shortcuts {

const QVector<Action>& actions()
{
    static const QVector<Action> list {
        { "menu_button",        "Show / hide the menu button",        "Ctrl+Alt+Shift+B" },
        { "fullscreen_toggle",  "Switch fullscreen / windowed",       "Ctrl+Alt+Shift+X" },
        { "fullscreen",         "Go fullscreen",                      "Ctrl+Alt+Shift+F" },
        { "windowed",           "Go windowed",                        "Ctrl+Alt+Shift+W" },
        { "minimize",           "Minimize",                           "Ctrl+Alt+Shift+D" },
        { "metrics",            "Show / hide metrics",                "Ctrl+Alt+Shift+S" },
        { "immersive",          "Immersive mode (capture mouse)",     "Ctrl+Alt+Shift+M" },
        { "keyboard_immersive", "Keyboard immersive mode",            "Ctrl+Alt+Shift+K" },
        { "release",            "Release mouse and keyboard",         "Ctrl+Alt+Shift+Z" },
        { "cursor",             "Show local cursor",                  "Ctrl+Alt+Shift+C" },
        { "lock_cursor",        "Lock cursor to window",              "Ctrl+Alt+Shift+L" },
        { "paste",              "Paste clipboard as text",            "Ctrl+Alt+Shift+V" },
        { "ctrl_alt_del",       "Send Ctrl+Alt+Del",                  "" },
        { "disconnect",         "Disconnect",                         "Ctrl+Alt+Shift+Q" },
        { "quit_exit",          "Quit app and exit Moonlight",        "Ctrl+Alt+Shift+E" },
    };
    return list;
}

static QString key(const QString& id)
{
    return QStringLiteral("shortcuts/") + id;
}

QString binding(const QString& id)
{
    QSettings settings;
    if (settings.contains(key(id))) {
        return settings.value(key(id)).toString();
    }
    for (const Action& action : actions()) {
        if (id == action.id) {
            return action.defaultBinding;
        }
    }
    return QString();
}

void setBinding(const QString& id, const QString& value)
{
    QSettings settings;
    if (!value.isEmpty()) {
        for (const Action& action : actions()) {
            if (id != action.id && binding(action.id).compare(value, Qt::CaseInsensitive) == 0) {
                settings.setValue(key(action.id), QString());
            }
        }
    }
    settings.setValue(key(id), value);
}

void resetAll()
{
    QSettings settings;
    settings.remove(QStringLiteral("shortcuts"));
}

QString menuSuffix(const QString& id)
{
    QString b = binding(id);
    return b.isEmpty() ? QString() : QStringLiteral("\t") + b;
}

}
