#pragma once

#include <QList>
#include <QString>
#include <QUrl>

// **What a held finger on a link or an image offers**, on Android.
//
// The desktop has Qt WebEngine's own context menu, which opens a link in a
// new tab, copies its address and saves an image. The system WebView has
// nothing: a long press on a link selected its text, so on a phone there was
// no way to open a link beside the page, copy where it goes, or save a
// picture. Listed as unbuilt in project.md's "What is next" since 2026-09-18.
//
// Pure, so which entries a link earns is tested without a device: the Java
// side asks for them, shows them in the platform's own menu, and hands back
// what was chosen. A native menu rather than a QMenu, because the WebView is
// drawn over everything Qt renders and `android_view` moves it out of the
// way while a Qt popup is up -- the page would vanish behind its own menu.
namespace link_menu {

enum class action { open, open_background, copy, save };

struct entry {
	QString label;
	action  what = action::open;
	QString value;   // the address to open or save, or the text to copy
};

// `link` is the anchor's address and `image` the picture's, either of which
// may be empty. Only what a person can use is offered: a `javascript:` link
// does nothing outside its page, and a `data:` or `blob:` image is not an
// address anybody can open, copy or fetch again.
QList<entry> entries(const QUrl &link, const QUrl &image);

// The entries as one string for the Java side, a line each:
// `<action>\t<label>\t<value>`. Empty when there is nothing to offer, which
// is what tells Java to leave the long press to the page.
QString encode(const QList<entry> &entries);

// The action word `encode` wrote, back.
bool parse_action(const QString &word, action *out);

}  // namespace link_menu
