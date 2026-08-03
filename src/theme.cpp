// SPDX-License-Identifier: GPL-3.0-or-later
#include "theme.h"

#include <QApplication>
#include <QStyle>
#include <QStyleHints>

#ifdef HYDRA_HAVE_DBUS
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDBusVariant>
#endif

namespace theme {

namespace {

// The dark palette, written out rather than derived.
//
// Qt 6.8 can be *told* the scheme and will darken its own widgets, but the
// platform style still hands out whatever colours the desktop theme has, and on
// a light desktop those are light. Someone who picks Dark on a light desktop
// means the window, not a hint about it.
QPalette dark_palette() {
	QPalette p;
	const QColor window(0x2b, 0x2b, 0x2e);
	const QColor base(0x1f, 0x1f, 0x22);
	const QColor alt(0x26, 0x26, 0x2a);
	const QColor text(0xe6, 0xe6, 0xe6);
	const QColor dim(0x9a, 0x9a, 0xa0);
	const QColor accent(0x3d, 0x7e, 0xb8);

	p.setColor(QPalette::Window, window);
	p.setColor(QPalette::WindowText, text);
	p.setColor(QPalette::Base, base);
	p.setColor(QPalette::AlternateBase, alt);
	p.setColor(QPalette::Text, text);
	p.setColor(QPalette::Button, window);
	p.setColor(QPalette::ButtonText, text);
	p.setColor(QPalette::ToolTipBase, base);
	p.setColor(QPalette::ToolTipText, text);
	p.setColor(QPalette::Highlight, accent);
	p.setColor(QPalette::HighlightedText, Qt::white);
	p.setColor(QPalette::Link, QColor(0x6f, 0xb2, 0xe8));
	p.setColor(QPalette::LinkVisited, QColor(0xa9, 0x8f, 0xd6));
	p.setColor(QPalette::PlaceholderText, dim);

	// Disabled has to be set for every group that uses it or a greyed control
	// keeps full-contrast text and stops looking greyed at all -- the same
	// mistake the settings page made with its descriptions.
	p.setColor(QPalette::Disabled, QPalette::WindowText, dim);
	p.setColor(QPalette::Disabled, QPalette::Text, dim);
	p.setColor(QPalette::Disabled, QPalette::ButtonText, dim);
	p.setColor(QPalette::Disabled, QPalette::Highlight, QColor(0x3a, 0x3a, 0x40));
	p.setColor(QPalette::Disabled, QPalette::HighlightedText, dim);
	return p;
}

#ifdef HYDRA_HAVE_DBUS
// 1 = prefer dark, 2 = prefer light, 0 = no preference, -1 = did not answer.
int portal_scheme() {
	QDBusInterface iface(QStringLiteral("org.freedesktop.portal.Desktop"),
	                      QStringLiteral("/org/freedesktop/portal/desktop"),
	                      QStringLiteral("org.freedesktop.portal.Settings"),
	                      QDBusConnection::sessionBus());
	if (!iface.isValid())
		return -1;
	const QDBusReply<QDBusVariant> reply =
		iface.call(QStringLiteral("Read"), QStringLiteral("org.freedesktop.appearance"),
		            QStringLiteral("color-scheme"));
	if (!reply.isValid())
		return -1;
	// The portal wraps the value twice: a variant holding a variant.
	QVariant v = reply.value().variant();
	if (v.canConvert<QDBusVariant>())
		v = v.value<QDBusVariant>().variant();
	bool ok = false;
	const int n = v.toInt(&ok);
	return ok ? n : -1;
}
#else
int portal_scheme() { return -1; }
#endif

}  // namespace

Qt::ColorScheme decide(Qt::ColorScheme qt_hint, int portal, const QPalette &current) {
	if (qt_hint != Qt::ColorScheme::Unknown)
		return qt_hint;
	if (portal == 1)
		return Qt::ColorScheme::Dark;
	if (portal == 2)
		return Qt::ColorScheme::Light;

	// Portal said "no preference" (0) or nothing at all. The palette we were
	// given is evidence in its own right: a window darker than the text on it is
	// a dark theme, whoever failed to say so.
	const int window = current.color(QPalette::Window).lightness();
	const int text   = current.color(QPalette::WindowText).lightness();
	if (window < text)
		return Qt::ColorScheme::Dark;
	return Qt::ColorScheme::Light;
}

Qt::ColorScheme detect_system() {
	const Qt::ColorScheme hint = QGuiApplication::styleHints()
	                                 ? QGuiApplication::styleHints()->colorScheme()
	                                 : Qt::ColorScheme::Unknown;
	return decide(hint, portal_scheme(), QGuiApplication::palette());
}

Qt::ColorScheme resolve(choice c) {
	switch (c) {
		case choice::light: return Qt::ColorScheme::Light;
		case choice::dark:  return Qt::ColorScheme::Dark;
		case choice::system: break;
	}
	return detect_system();
}

void apply(choice c) {
	const Qt::ColorScheme want = resolve(c);

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
	// Telling Qt as well as painting: its own dialogs, the style's own drawing
	// and the web engine's `prefers-color-scheme` all read this rather than the
	// palette, so setting one without the other gives a dark window with light
	// menus in it.
	QGuiApplication::styleHints()->setColorScheme(want);
#endif

	if (want == Qt::ColorScheme::Dark) {
		QApplication::setPalette(dark_palette());
	} else {
		// Back to whatever the style would have chosen on its own, rather than a
		// hand-written light palette: a desktop's light theme is the user's, and
		// replacing it with ours would make "Light" mean "our light".
		if (QStyle *s = QApplication::style())
			QApplication::setPalette(s->standardPalette());
	}
}

void set_web_engine_scheme(Qt::ColorScheme scheme) {
	// Blink's enum: 0 is dark, 1 is light. Appended rather than assigned, so a
	// flag somebody set in the environment for their own reasons survives.
	const QByteArray want =
		"--blink-settings=preferredColorScheme=" +
		QByteArray(scheme == Qt::ColorScheme::Dark ? "0" : "1");
	QByteArray flags = qgetenv("QTWEBENGINE_CHROMIUM_FLAGS");
	if (flags.contains("preferredColorScheme"))
		return;   // somebody asked for something specific; leave it alone
	if (!flags.isEmpty())
		flags += ' ';
	flags += want;
	qputenv("QTWEBENGINE_CHROMIUM_FLAGS", flags);
}

QString name_of(choice c) {
	switch (c) {
		case choice::light: return QStringLiteral("light");
		case choice::dark:  return QStringLiteral("dark");
		case choice::system: break;
	}
	return QStringLiteral("system");
}

choice from_name(const QString &name) {
	const QString n = name.trimmed().toLower();
	if (n == "light") return choice::light;
	if (n == "dark")  return choice::dark;
	return choice::system;
}

watcher::watcher(QObject *parent) : QObject(parent) {
	// Qt's own signal, for the platforms where Qt does the detecting.
	if (QStyleHints *h = QGuiApplication::styleHints())
		connect(h, &QStyleHints::colorSchemeChanged, this,
		         [this](Qt::ColorScheme) { reapply(); });

#ifdef HYDRA_HAVE_DBUS
	// And the portal's, for the desktop where Qt says Unknown -- which is the
	// case this whole file exists for. Without this, choosing "system" would
	// follow the desktop once, at startup, and then stop.
	QDBusConnection::sessionBus().connect(
		QStringLiteral("org.freedesktop.portal.Desktop"),
		QStringLiteral("/org/freedesktop/portal/desktop"),
		QStringLiteral("org.freedesktop.portal.Settings"),
		QStringLiteral("SettingChanged"), this,
		SLOT(portal_changed(QString, QString, QDBusVariant)));
#endif
}

void watcher::portal_changed(const QString &space, const QString &key,
                              const QDBusVariant &value) {
	Q_UNUSED(value)
	// Only the one setting; the portal reports every change on this desktop.
	if (space == QLatin1String("org.freedesktop.appearance") &&
	    key == QLatin1String("color-scheme"))
		reapply();
}

void watcher::set_choice(choice c) {
	m_choice = c;
	reapply();
}

void watcher::reapply() {
	const Qt::ColorScheme want = resolve(m_choice);
	// Repainting every widget is not free, and a desktop that emits several
	// notifications for one change is ordinary.
	if (want == m_last)
		return;
	m_last = want;
	apply(m_choice);
	emit applied(want);
}

}  // namespace theme
