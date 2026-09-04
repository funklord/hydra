#include "theme.h"
#include <QTextStream>
#include <QRegularExpression>
#include <QIcon>
#include <QFile>
#include <QProxyStyle>
#include <QStyleOption>
#include <QPixmap>
#include <QPainter>
#include <QImage>
#include <cmath>
#include <algorithm>
#include <QDir>
#include <QStandardPaths>

#include <QApplication>
#include <QStyle>
#include <QStyleHints>

/// @pkg_optional Qt6DBus defines HYDRA_HAVE_DBUS
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

	// **The five roles a style shades with, which this palette used to leave
	// behind.** `QPalette p;` copies the application's current palette and
	// only the roles named above were overridden, so `Light`, `Midlight`,
	// `Mid`, `Dark` and `Shadow` stayed at the *light* desktop's values --
	// measured, and identical in both schemes:
	//
	//     Light #ffffff  Midlight #cacaca  Mid #b8b8b8  Dark #9f9f9f
	//
	// A style fills frames, group boxes, headers, splitter handles and tab
	// bars with those. In a dark window that is a near-white ground with
	// near-white `WindowText` drawn on it, which is the "impossible to read"
	// this was reported as.
	//
	// Derived from the window rather than written out, so the five stay in
	// step with it if it ever moves, and ordered the way Qt expects:
	// Light > Midlight > Button > Mid > Dark > Shadow.
	p.setColor(QPalette::Light,    window.lighter(160));
	p.setColor(QPalette::Midlight, window.lighter(130));
	p.setColor(QPalette::Mid,      window.darker(130));
	p.setColor(QPalette::Dark,     window.darker(160));
	p.setColor(QPalette::Shadow,   window.darker(300));

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
	// The portal first, then what the desktop wrote down. -1 is "the portal did
	// not answer" and 0 is "it answered no preference"; neither is a preference,
	// so both fall through to tier 4 rather than only the first.
	int scheme = portal_scheme();
	if (scheme < 1) {
		const QString home = QDir::homePath();
		const QString cfg = qEnvironmentVariableIsSet("XDG_CONFIG_HOME")
		                        ? QString::fromLocal8Bit(qgetenv("XDG_CONFIG_HOME"))
		                        : home + "/.config";
		scheme = color_scheme_from({
			cfg + "/kdeglobals",
			home + "/.trinity/share/config/kdeglobals",
			home + "/.kde/share/config/kdeglobals",
			// LXQt writes the APPLIED palette into its own config, under
			// [Palette]. The files under <data>/lxqt/palettes/ are the
			// library its "Load Palette" dialog reads from, and nothing
			// records which one is active -- so consulting them answers
			// about a palette that may not be in use. A fixed path, like
			// the three above it.
			cfg + "/lxqt/lxqt.conf",
		});
	}
	return decide(hint, scheme, QGuiApplication::palette());
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

// --- The icon theme -------------------------------------------------------

// Both spellings that matter: an INI-ish `key=value` as GTK 3 and kdeglobals
// write it, and GTK 2's quoted `gtk-icon-theme-name="Breeze"`. The value is
// taken from the first file that names one, so order is authority.
namespace {

// Rec.709 relative luminance, which is the comparison `harmonization.md`
// specifies -- deliberately the same question the palette rung asks, pointed at
// the source that has the answer, rather than a second heuristic to trust.
double luminance_709(int r, int g, int b) {
	return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

// "#232323" -> a colour, or false. LXQt writes hex; TDE writes decimal
// triples. Two spellings of the same statement, so both are read rather
// than one being preferred -- see `harmonization.md`'s dialect table.
bool parse_hex(const QString &value, int *r, int *g, int *b) {
	QString s = value.trimmed();
	if (!s.startsWith(QLatin1Char('#')))
		return false;
	s = s.mid(1);
	if (s.size() != 6)
		return false;
	bool ok = false;
	const uint n = s.toUInt(&ok, 16);
	if (!ok)
		return false;
	*r = int((n >> 16) & 0xff);
	*g = int((n >> 8) & 0xff);
	*b = int(n & 0xff);
	return true;
}

// "0,42,78" -> a colour, or false. TDE writes plain decimal triples.
bool parse_triple(const QString &value, int *r, int *g, int *b) {
	const QStringList parts = value.split(QLatin1Char(','));
	if (parts.size() != 3)
		return false;
	bool ok0 = false, ok1 = false, ok2 = false;
	*r = parts.at(0).trimmed().toInt(&ok0);
	*g = parts.at(1).trimmed().toInt(&ok1);
	*b = parts.at(2).trimmed().toInt(&ok2);
	return ok0 && ok1 && ok2;
}

}  // namespace

int theme::color_scheme_from(const QStringList &sources) {
	for (const QString &path : sources) {
		QFile f(path);
		if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
			continue;
		QTextStream in(&f);
		QString section;
		int br = -1, bg = -1, bb = -1, fr = -1, fg = -1, fb = -1;
		while (!in.atEnd()) {
			const QString line = in.readLine().trimmed();
			if (line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']'))) {
				section = line.mid(1, line.size() - 2);
				continue;
			}
			// Sections matter in both dialects, and for the same reason: the
			// same key names appear in per-application sections, and taking
			// whichever came last would answer about some other program's
			// colours.
			const bool general =
			  section.compare(QLatin1String("General"), Qt::CaseInsensitive) == 0;
			const bool palette =
			  section.compare(QLatin1String("Palette"), Qt::CaseInsensitive) == 0;
			if (!general && !palette)
				continue;
			const int eq = line.indexOf(QLatin1Char('='));
			if (eq < 0)
				continue;
			const QString key = line.left(eq).trimmed();
			const QString val = line.mid(eq + 1).trimmed();
			// TDE and KDE 3: [General], decimal triples.
			if (general) {
				if (key.compare(QLatin1String("windowBackground"),
				                 Qt::CaseInsensitive) == 0)
					parse_triple(val, &br, &bg, &bb);
				else if (key.compare(QLatin1String("windowForeground"),
				                      Qt::CaseInsensitive) == 0)
					parse_triple(val, &fr, &fg, &fb);
				continue;
			}
			// LXQt: [Palette], #rrggbb.
			if (key.compare(QLatin1String("window_color"), Qt::CaseInsensitive) == 0)
				parse_hex(val, &br, &bg, &bb);
			else if (key.compare(QLatin1String("window_text_color"),
			                      Qt::CaseInsensitive) == 0)
				parse_hex(val, &fr, &fg, &fb);
		}
		if (br < 0 || fr < 0)
			continue;   // this file had no answer; try the next
		const double back = luminance_709(br, bg, bb);
		const double fore = luminance_709(fr, fg, fb);
		if (back < fore)
			return 1;   // prefer dark
		if (back > fore)
			return 2;   // prefer light
		return 0;       // identical, which says nothing
	}
	// **Abstain rather than guess.** The two errors are not symmetric: a wrong
	// light answer is merely plain, while a wrong dark one is unreadable text on
	// a pale background. No readable file means no opinion.
	return 0;
}

QString theme::icon_theme_from(const QStringList &sources) {
	static const QRegularExpression re(
	    // Custom delimiter: the pattern contains `)"`, which closes a plain
	    // `R"( )"` early and turns the rest of the regex into stray tokens.
	    R"RX(^\s*(?:gtk-icon-theme-name|Theme)\s*=\s*"?([^"\r\n]+?)"?\s*$)RX",
	    QRegularExpression::CaseInsensitiveOption);
	for (const QString &path : sources) {
		QFile f(path);
		if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
			continue;
		QTextStream in(&f);
		// `Theme=` is only meaningful under `[Icons]` in a kdeglobals, where a
		// bare `Theme=` elsewhere means the *colour* scheme. So sections are
		// tracked -- but only for those files.
		//
		// **Only for those files**, because GTK's settings.ini puts everything
		// under `[Settings]`, and gating on `[Icons]` there skips the whole
		// file. That was the first version, and it is why this came up with
		// `hicolor` on a desktop whose GTK config plainly says `breeze-dark`:
		// the answer was in the file, one line past a section header the parser
		// had decided to stop at.
		const bool sectioned = path.contains(QLatin1String("kdeglobals"));
		bool in_icons = !sectioned;
		while (!in.atEnd()) {
			const QString line = in.readLine();
			const QString t = line.trimmed();
			if (sectioned && t.startsWith('[')) {
				in_icons = (t.compare(QLatin1String("[Icons]"),
				                       Qt::CaseInsensitive) == 0);
				continue;
			}
			if (!in_icons)
				continue;
			const QRegularExpressionMatch m = re.match(line);
			if (m.hasMatch() && !m.captured(1).trimmed().isEmpty())
				return m.captured(1).trimmed();
		}
	}
	return QString();
}

namespace {

// Gamma-correct relative luminance, and the contrast ratio between two
// colours. Written here rather than reached for from elsewhere because this
// file is the only thing in the tree that needs it; if a second caller
// appears, that is the moment to share it rather than to keep a second copy.
double luminance(const QColor &c) {
	auto channel = [](double v) {
		v /= 255.0;
		return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
	};
	return 0.2126 * channel(c.red()) + 0.7152 * channel(c.green()) +
	        0.0722 * channel(c.blue());
}

double contrast_of(const QColor &a, const QColor &b) {
	const double x = luminance(a), y = luminance(b);
	return (std::max(x, y) + 0.05) / (std::min(x, y) + 0.05);
}

// **What a held button is filled with, so that it plainly looks held.**
//
// Reported from the desktop as "the contrast of held buttons is poor and hard
// to see", and measured before it was touched: a checked toolbar button came
// out at **1.22:1** against an unchecked one in dark and 1.36:1 in light,
// which is a shade rather than a state.
//
// Derived from the button's own colour rather than from an accent, because
// the ground is not always ours. In dark the palette is hydra's; in light it
// is whatever the desktop's style handed over, and `apply()` says why that is
// deliberate. A fixed accent chosen against one of those is a guess against
// the other -- the workspace has the measurement for that, thirteen real
// schemes and no accent clearing the floor on all of them.
//
// So walk away from the base colour until the ratio is met, in whichever
// direction has the room: lighter on a dark button, darker on a light one.
// That cannot fail for want of headroom, because the direction is chosen by
// where the headroom is.
QColor held_fill(const QColor &base) {
	// **1.6, and it is a visibility target rather than a legibility one.**
	// Two backgrounds are being compared, not a colour against its own text,
	// so the 3:1 the workspace settled on for that does not transfer. What is
	// wanted is a difference nobody has to look for.
	constexpr double want = 1.6;
	const bool lighter = luminance(base) < 0.5;

	// **Measured against what the style DRAWS, not against the palette role.**
	// Aiming at `Button` alone was not enough in dark: Fusion paints an
	// unheld button lighter than the colour the palette gives -- #2b2b2e in
	// the role, #3e3e41 on screen -- so a fill that cleared 1.6 against the
	// role cleared only 1.21 against the button beside it, which is the
	// comparison a person actually makes. The fix moved the number by 0.01
	// and the test said so.
	//
	// The style is not going to tell us what it will draw, so clear the bar
	// against a deliberately pessimistic stand-in as well: the base lightened
	// by a third, which is more than Fusion moves it. Overshooting costs a
	// slightly stronger held state; undershooting costs the whole point.
	const QColor drawn = base.lighter(133);
	QColor c = base;
	for (int step = 0; step < 96; ++step) {
		if (contrast_of(c, base) >= want && contrast_of(c, drawn) >= want)
			break;
		c = lighter ? c.lighter(105) : c.darker(105);
	}
	return c;
}

// Qt's disabled rendering, with the lift halved. See theme.h.
class icon_style : public QProxyStyle {
public:
	// **Drawn here rather than left to the style**, because which palette role
	// a style reaches for to say "on" is the style's business and the answers
	// differ: measured offscreen it is a shade, and the desktop this was
	// reported from runs a style this machine's test never loads. Filling it
	// ourselves is the one answer that does not depend on which style is
	// installed.
	//
	// The base style still draws afterwards, so the frame, the focus ring and
	// everything else stay the platform's. Only the panel underneath is ours.
	void drawPrimitive(PrimitiveElement el, const QStyleOption *opt,
	                    QPainter *p, const QWidget *w) const override {
		const bool held = opt && (opt->state & (State_On | State_Sunken));
		if (!held || (el != PE_PanelButtonTool && el != PE_PanelButtonCommand)) {
			QProxyStyle::drawPrimitive(el, opt, p, w);
			return;
		}
		// **The base first, then ours on top of it.** Drawn the other way
		// round the base repaints its own panel over the fill and nothing
		// changes at all -- which is exactly what happened, and the
		// measurement reporting the identical numbers before and after is
		// what said so rather than any reading of the code.
		QProxyStyle::drawPrimitive(el, opt, p, w);
		// Inset by a pixel so whatever edge the platform style drew survives
		// and only the interior is ours. A held button still looks like this
		// desktop's button; it just plainly looks held.
		const QRect inner = opt->rect.adjusted(1, 1, -1, -1);
		if (inner.isEmpty())
			return;
		p->save();
		p->setPen(Qt::NoPen);
		p->setBrush(held_fill(opt->palette.color(QPalette::Button)));
		p->drawRect(inner);
		p->restore();
	}

	QPixmap generatedIconPixmap(QIcon::Mode mode, const QPixmap &pixmap,
	                               const QStyleOption *opt) const override {
		if (mode != QIcon::Disabled || pixmap.isNull())
			return QProxyStyle::generatedIconPixmap(mode, pixmap, opt);

		// Toward the background the icon actually sits on, taken from the
		// option where there is one: a toolbar and a menu are not always
		// painted in the same colour.
		const QColor bg = opt ? opt->palette.color(QPalette::Window)
		                         : QApplication::palette().color(QPalette::Window);
		const double target = qGray(bg.rgb());
		// Half of the 0.40 Qt applies. Enough that a disabled control is
		// plainly paler than a live one, not so much that the shape goes.
		constexpr double lift = 0.20;

		QImage img = pixmap.toImage().convertToFormat(QImage::Format_ARGB32);
		for (int y = 0; y < img.height(); ++y) {
			QRgb *row = reinterpret_cast<QRgb *>(img.scanLine(y));
			for (int x = 0; x < img.width(); ++x) {
				const int a = qAlpha(row[x]);
				if (!a)
					continue;
				// Fully desaturated, which is the part that says "off".
				const double g = qGray(row[x]);
				const int v = int(qBound(0.0, g + (target - g) * lift, 255.0));
				row[x] = qRgba(v, v, v, a);
			}
		}
		QPixmap out = QPixmap::fromImage(img);
		// Or the icon is drawn at the wrong size on a scaled display.
		out.setDevicePixelRatio(pixmap.devicePixelRatio());
		return out;
	}
};

}  // namespace

void theme::install_icon_style() {
	// Owned by QApplication once set, and set before any widget exists so
	// nothing is left holding the previous style.
	QApplication::setStyle(new icon_style);
}

static QStringList detect_icon_theme_list();

void theme::seed_icon_search_paths() {
	QStringList paths = QIcon::themeSearchPaths();
	// `locateAll` is the same set `XDG_DATA_DIRS` describes, already filtered
	// to directories that exist, and it includes the user's own
	// `~/.local/share/icons` ahead of the system ones.
	for (const QString &dir : QStandardPaths::locateAll(
	         QStandardPaths::GenericDataLocation, QStringLiteral("icons"),
	         QStandardPaths::LocateDirectory))
		if (!paths.contains(dir))
			paths << dir;
	// The old per-user location, which predates XDG and is still where several
	// theme installers put things.
	const QString dot_icons = QDir::homePath() + QStringLiteral("/.icons");
	if (!paths.contains(dot_icons) && QFile::exists(dot_icons))
		paths << dot_icons;
	if (paths != QIcon::themeSearchPaths())
		QIcon::setThemeSearchPaths(paths);
}

QStringList theme::detect_icon_themes() {
	// Before anything is looked for, or the search below has nowhere to look.
	seed_icon_search_paths();
	return detect_icon_theme_list();
}

QString theme::detect_icon_theme() {
	return detect_icon_themes().value(0);
}

// Every candidate that is actually present, best first. Kept as a list
// because being installed does not mean being able to draw: the caller loads
// each in turn and asks for an icon.
static QStringList detect_icon_theme_list() {
	const QString home = QDir::homePath();
	const QString cfg = qEnvironmentVariableIsSet("XDG_CONFIG_HOME")
	                        ? QString::fromLocal8Bit(qgetenv("XDG_CONFIG_HOME"))
	                        : home + "/.config";
	const QString named = theme::icon_theme_from({
		cfg + "/gtk-4.0/settings.ini",
		cfg + "/gtk-3.0/settings.ini",
		home + "/.gtkrc-2.0",
		cfg + "/kdeglobals",
		// Trinity keeps its own, and it is the desktop actually running here.
		home + "/.trinity/share/config/kdeglobals",
		home + "/.kde/share/config/kdeglobals",
	});

	QStringList tries;
	if (!named.isEmpty())
		tries << named;
	// Ordinary fallbacks, in the order most desktops would rank them. They are
	// tried after whatever the desktop asked for, and each is still only a
	// candidate: `Adwaita` in particular is present on most machines and
	// carries none of the icons wanted here.
	tries << "breeze" << "Adwaita" << "oxygen" << "hicolor";

	// Named or not, a candidate has to exist on disk. A theme that is
	// configured but not installed leaves every lookup null exactly as an unset
	// one does, and that is a failure worth stepping past rather than
	// reporting.
	QStringList present;
	for (const QString &t : tries) {
		if (present.contains(t))
			continue;
		for (const QString &dir : QIcon::themeSearchPaths()) {
			if (QFile::exists(dir + "/" + t + "/index.theme")) {
				present << t;
				break;
			}
		}
	}
	return present;
}

// Can the current theme draw the sort of icon a toolbar asks for? `hicolor` is
// the freedesktop fallback and carries almost no application icons, so it is a
// name without a theme behind it for this purpose.
static bool theme_is_usable() {
	if (QIcon::themeName().isEmpty())
		return false;
	const QIcon probe = QIcon::fromTheme(QStringLiteral("go-previous"));
	return !probe.isNull() && !probe.availableSizes().isEmpty();
}

// `breeze` and `breeze-dark` are the same icons drawn for opposite backgrounds,
// and most themes that ship a dark variant name it this way. Given a name and
// the scheme the window will actually use, return whichever variant exists.
static QString variant_for(const QString &name, Qt::ColorScheme scheme) {
	const bool want_dark = (scheme == Qt::ColorScheme::Dark);
	const bool is_dark = name.endsWith(QLatin1String("-dark"));
	if (want_dark == is_dark)
		return name;
	const QString other = want_dark ? name + "-dark"
	                                 : name.chopped(5);   // drop "-dark"
	for (const QString &dir : QIcon::themeSearchPaths())
		if (QFile::exists(dir + "/" + other + "/index.theme"))
			return other;
	return name;
}

QString theme::apply_icon_theme(Qt::ColorScheme scheme) {
	// **First, so that anything below can see the disk at all.** Without this
	// the search list holds only `:/icons` on a desktop Qt has no plugin for,
	// and every check downstream is asking about directories it cannot look in.
	seed_icon_search_paths();

	// If a platform theme already answered *with something that works*, it
	// knows better than this does.
	//
	// The "that works" half was missing and cost an hour. Qt reports a theme
	// name of `hicolor` when nothing else is set -- not empty, so an
	// `isEmpty()` guard returns happily, having chosen a theme that contains
	// none of the icons about to be asked for. The startup line then says
	// `icon theme: hicolor` and everything downstream draws nothing, which
	// reads as the icon *names* being wrong rather than the theme.
	if (theme_is_usable())
		return QIcon::themeName();

	// So a theme that inherits (breeze-dark -> breeze) still resolves anything
	// it does not carry itself. Set before the probing below, since a
	// candidate's usability depends on what it can fall back to.
	QIcon::setFallbackThemeName(QStringLiteral("hicolor"));

	// **Present is not usable, and only loading it can tell the two apart.**
	// Each candidate is set and then asked for an ordinary toolbar icon; the
	// first that answers is the one. Adwaita is why this loops rather than
	// taking the first that exists: it is installed nearly everywhere, ships
	// an `index.theme`, and carries cursors and symbolic icons only -- so the
	// old code chose it over the `oxygen` sitting beside it and drew nothing.
	for (const QString &candidate : detect_icon_theme_list()) {
		const QString t = variant_for(candidate, scheme);
		QIcon::setThemeName(t);
		if (theme_is_usable())
			return t;
	}

	// Nothing worked. Leave no half-chosen theme behind: a name that draws
	// nothing is worse than no name, because the next thing to ask
	// `themeName()` would believe it.
	QIcon::setThemeName(QString());
	return QString();
}

