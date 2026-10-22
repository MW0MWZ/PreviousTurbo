/*
  PreviousTurbo - menu_model.h

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  The window's menus, described once: drawn in the window on Linux and
  Windows, and as the system menu bar on macOS.
*/

#ifndef PREV_MENU_MODEL_H
#define PREV_MENU_MODEL_H

#include <functional>
#include <string>
#include <vector>

namespace ptgui {

struct MenuEntry {
    enum class Kind { Item, Separator, Heading, Submenu };
    // Items macOS keeps in its application menu instead.
    enum class Role { None, About, Settings, Quit };
    Kind kind = Kind::Item;
    std::string label;
    std::string keys;    // the shortcut as shown ("Ctrl+Alt+F", "F12")
    char mac_key = 0;    // Ctrl+Option + this letter on macOS, lower case (0: none)
    bool checked = false, enabled = true;
    std::string tooltip;
    Role role = Role::None;
    std::function<void()> action;  // looks up what it acts on when run
    std::vector<MenuEntry> children;

    static MenuEntry item(std::string label, std::function<void()> action, bool enabled = true, bool checked = false) {
        MenuEntry e;
        e.label = std::move(label);
        e.action = std::move(action);
        e.enabled = enabled;
        e.checked = checked;
        return e;
    }
    static MenuEntry separator() {
        MenuEntry e;
        e.kind = Kind::Separator;
        return e;
    }
    static MenuEntry heading(std::string label) {
        MenuEntry e;
        e.kind = Kind::Heading;
        e.label = std::move(label);
        return e;
    }
    static MenuEntry submenu(std::string label, std::vector<MenuEntry> children) {
        MenuEntry e;
        e.kind = Kind::Submenu;
        e.label = std::move(label);
        e.children = std::move(children);
        return e;
    }
    MenuEntry& key(std::string shown, char mac = 0) {
        keys = std::move(shown);
        mac_key = mac;
        return *this;
    }
    MenuEntry& tip(std::string t) {
        tooltip = std::move(t);
        return *this;
    }
    MenuEntry& as(Role r) {
        role = r;
        return *this;
    }
};

// macOS: puts the menus in the system menu bar (false elsewhere). `menus`
// describes them when one opens; `post` runs a chosen item's action from
// the main loop.
bool install_native_menus(std::function<std::vector<MenuEntry>()> menus,
                          std::function<void(std::function<void()>)> post);

}  // namespace ptgui

#endif /* PREV_MENU_MODEL_H */
