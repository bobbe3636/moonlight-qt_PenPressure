#pragma once

#include <QString>
#include <QVector>

// Rebindable stream shortcuts. A binding is text like "Ctrl+Alt+Shift+X": the modifiers
// Ctrl, Alt, Shift, Win in that order, then an SDL key name ("A", "F5", "Space", "PageUp").
// An empty binding means the action has no shortcut. Stored under "shortcuts/<id>".
namespace Shortcuts {

struct Action {
    const char* id;
    const char* label;
    const char* defaultBinding;
};

const QVector<Action>& actions();

// The current binding (the default until the user changes it)
QString binding(const QString& id);

// Set a binding; another action that had the same binding loses it
void setBinding(const QString& id, const QString& binding);

void resetAll();

// "\t<binding>" for a menu item, or nothing when the action has no shortcut
QString menuSuffix(const QString& id);

}
