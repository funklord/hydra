// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#ifdef HYDRA_HAVE_DBUS
#include <QDBusVariant>
#else
// So the slot's signature exists whether or not DBus does; nothing calls it
// without the portal, and a header that changes shape by build is worse.
class QDBusVariant {};
#endif
#include <QPalette>
#include <QStringList>
#include <Qt>

// Light, dark, or whatever the desktop is set to (architecture doc sec 6.1).
//
// **The detection is the hard part, and Qt alone is not enough.** On the machine
// this was written on, KDE with a dark Breeze setup, `QStyleHints::colorScheme()`
// returns `Unknown` while the XDG desktop portal answers "prefer dark" and
// gsettings agrees. An app that trusted Qt would come up light on a dark desktop
// and have no idea it had got it wrong.
//
// So there is a ladder, and each rung is asked only when the one above has no
// opinion:
//
//   1. `QStyleHints::colorScheme()` -- the platform's own answer, and the right
//      one on Windows and macOS where Qt does read it.
//   2. The **XDG desktop portal**, `org.freedesktop.appearance/color-scheme`:
//      1 means prefer dark, 2 prefer light, 0 no preference. This is the
//      freedesktop standard, it is what a Linux desktop actually sets, and it is
//      the rung that works here.
//   3. **The palette Qt already has.** If the platform theme handed us a window
//      darker than its own text, we are running dark whatever anyone says about
//      it. This catches a dark GTK or Qt theme with no portal at all.
//   4. Light, because something has to be chosen and a wrong light guess is
//      merely plain, while a wrong dark guess is unreadable text on a pale
//      window.
namespace theme {

enum class choice {
	system = 0,   // follow the desktop, and keep following it as it changes
	light,
	dark,
};

// The rung-by-rung decision, as a function of what each source said, so it can
// be tested without a desktop. `portal` is -1 when the portal did not answer.
Qt::ColorScheme decide(Qt::ColorScheme qt_hint, int portal, const QPalette &current);

// Ask the sources and run `decide` on the answers.
Qt::ColorScheme detect_system();

// The desktop's icon theme, by name, because Qt does not know it here either.
//
// Qt6 ships platform-theme plugins for Plasma and for GTK, and picks an icon
// theme through whichever one loads. On this desktop -- Trinity, which reports
// `XDG_CURRENT_DESKTOP=TDE` -- neither loads, `QIcon::themeName()` comes back
// empty, and every `QIcon::fromTheme` call returns a null icon. A toolbar built
// on those calls comes up with no icons at all and nothing says why. That is
// the same shape as the colour-scheme problem above and it has the same answer:
// read what the desktop wrote down.
//
// `sources` are files to consult in order, most authoritative first, so the
// parsing can be tested without a desktop. The default list is the real one.
QString icon_theme_from(const QStringList &sources);

// Ask the real sources, and confirm the answer names a theme that is installed
// -- an icon theme that is configured but absent leaves every lookup null just
// as surely as no configuration at all.
QString detect_icon_theme();

// Set it on QIcon, unless Qt already found one that works. Call once, before any
// icon is built. Returns what it settled on, empty if nothing usable was found.
//
// `scheme` is the scheme *the application will paint in*, which is not always
// the desktop's: someone who chose Light on a dark desktop gets a light window,
// and `breeze-dark`'s icons are drawn pale to sit on dark chrome, so following
// the desktop's icon setting there would put pale icons on a pale toolbar. The
// variant is matched to the window rather than to the desktop.
QString apply_icon_theme(Qt::ColorScheme scheme);

// What a choice resolves to right now.
Qt::ColorScheme resolve(choice c);

// Paint the application. Applies the palette and, on Qt 6.8 and later, tells Qt
// which scheme it is in so its own dialogs and the web engine agree.
void apply(choice c);

// Tell the web engine which scheme pages should see, by adding Chromium's
// `preferredColorScheme` to the flags it reads at startup.
//
// **This has to happen before the engine starts, and cannot be undone while it
// runs.** Qt propagates the application's colour scheme to Chromium by watching
// `QStyleHints::colorSchemeChanged` -- but on a desktop where Qt reports
// `Unknown`, `setColorScheme()` is a request the platform is free to ignore, and
// this one does: the value reads back unchanged and the signal never fires, so
// the window goes dark and every page stays white. Measured, not assumed.
//
// So the scheme is passed as a flag instead. The cost is that changing it while
// running repaints the browser but not the pages, which the settings page says
// out loud rather than leaving somebody to notice.
void set_web_engine_scheme(Qt::ColorScheme scheme);

QString name_of(choice c);
choice  from_name(const QString &name);

// Watches the desktop while the app runs, so a system that switches at sunset
// takes the window with it. Only acts while the choice is `system`.
class watcher : public QObject {
	Q_OBJECT
public:
	explicit watcher(QObject *parent = nullptr);
	void set_choice(choice c);
	choice current() const { return m_choice; }

signals:
	// Emitted after the application has been repainted, so the shell can pass
	// the scheme to anything that does not read a QPalette -- the web engine's
	// `prefers-color-scheme`, for one.
	void applied(Qt::ColorScheme scheme);

private slots:
	// The portal's own notification. A real slot rather than a lambda because
	// QDBusConnection::connect matches by signature.
	void portal_changed(const QString &space, const QString &key,
		                   const QDBusVariant &value);

private:
	void reapply();

	choice          m_choice = choice::system;
	Qt::ColorScheme m_last   = Qt::ColorScheme::Unknown;
};

}  // namespace theme
