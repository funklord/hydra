// Which colour scheme the desktop is in (architecture doc sec 6.1).
//
// The reason this has a test file of its own is that **Qt's answer is not
// enough**. On the KDE desktop this was written on, `QStyleHints::colorScheme()`
// returns `Unknown` while the XDG portal answers "prefer dark" and gsettings
// agrees. Trusting Qt would have come up light on a dark desktop, confidently.
//
// So `decide()` takes what each source said and is tested on the combinations,
// including the one that machine actually produces. The sources themselves need
// a desktop; the decision does not.
#include "theme.h"

#include "node.h"
#include "tab_tree_model.h"
#include <QFile>
#include <QDir>

#include <QApplication>
#include <QIcon>
#include <QStandardPaths>
#include <QBrush>
#include <QPixmap>
#include <QToolButton>
#include <cmath>
#include <algorithm>
#include <QPalette>
#include <cstdio>
#include <cstdlib>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void section(const char *n) { std::printf("\n== %s ==\n", n); }

// Gamma-correct relative luminance and the contrast ratio between two
// colours -- the WCAG form, and the same one `theme.cpp` uses. **One copy for
// the whole file**, because two sections need it and a second copy is how two
// luminances stop agreeing.
static double lum(const QColor &c) {
	auto ch = [](double v) {
		v /= 255.0;
		return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
	};
	return 0.2126 * ch(c.red()) + 0.7152 * ch(c.green()) + 0.0722 * ch(c.blue());
}

static double contrast(const QColor &a, const QColor &b) {
	const double x = lum(a), y = lum(b);
	return (std::max(x, y) + 0.05) / (std::min(x, y) + 0.05);
}

static QPalette light_palette() {
	QPalette p;
	p.setColor(QPalette::Window, QColor(0xef, 0xef, 0xef));
	p.setColor(QPalette::WindowText, QColor(0x00, 0x00, 0x00));
	return p;
}
static QPalette darkish_palette() {
	QPalette p;
	p.setColor(QPalette::Window, QColor(0x2b, 0x2b, 0x2e));
	p.setColor(QPalette::WindowText, QColor(0xe6, 0xe6, 0xe6));
	return p;
}

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);

	using CS = Qt::ColorScheme;

	section("when Qt knows, Qt wins");
	{
		// It is the platform's own answer, and on Windows and macOS it is the
		// one that is right. Nothing below it should be able to overrule it.
		check(theme::decide(CS::Dark, 2, light_palette()) == CS::Dark,
		      "a definite Dark beats a portal saying light");
		check(theme::decide(CS::Light, 1, darkish_palette()) == CS::Light,
		      "and a definite Light beats a portal saying dark");
	}

	section("when Qt says Unknown, the portal answers");
	{
		// **This is the case on the machine this was written on**, and the whole
		// reason the ladder exists: Qt Unknown, portal 1, a light-looking palette
		// because the platform theme has not been applied yet.
		check(theme::decide(CS::Unknown, 1, light_palette()) == CS::Dark,
		      "portal 1 means prefer dark, whatever the palette looks like");
		check(theme::decide(CS::Unknown, 2, darkish_palette()) == CS::Light,
		      "and portal 2 means prefer light");
	}

	section("when nobody has a preference, the palette is evidence");
	{
		// Portal 0 is "no preference" -- a real answer, not a missing one -- and a
		// desktop that says that can still have handed Qt a dark theme.
		check(theme::decide(CS::Unknown, 0, darkish_palette()) == CS::Dark,
		      "a window darker than its own text is a dark theme");
		check(theme::decide(CS::Unknown, 0, light_palette()) == CS::Light,
		      "and a window lighter than its text is a light one");
		check(theme::decide(CS::Unknown, -1, darkish_palette()) == CS::Dark,
		      "the same when the portal did not answer at all");
	}

	section("and when there is nothing to go on");
	{
		// Light, deliberately: a wrong light guess is merely plain, while a wrong
		// dark guess is dark text on a dark window, which is unreadable.
		QPalette flat;
		flat.setColor(QPalette::Window, QColor(0x80, 0x80, 0x80));
		flat.setColor(QPalette::WindowText, QColor(0x80, 0x80, 0x80));
		check(theme::decide(CS::Unknown, -1, flat) == CS::Light,
		      "an ambiguous palette resolves light rather than risking unreadable");
	}

	section("the choice, and what it resolves to");
	{
		check(theme::resolve(theme::choice::light) == CS::Light,
		      "Light means light, whatever the desktop is doing");
		check(theme::resolve(theme::choice::dark) == CS::Dark,
		      "and Dark means dark");
		const Qt::ColorScheme sys = theme::resolve(theme::choice::system);
		check(sys == CS::Light || sys == CS::Dark,
		      "System always resolves to one of the two, never Unknown");
		std::printf("  --    this desktop reads as %s\n",
		            sys == CS::Dark ? "dark" : "light");
	}

	section("names, because the setting is stored as one");
	{
		check(theme::name_of(theme::choice::system) == "system", "system");
		check(theme::name_of(theme::choice::dark) == "dark", "dark");
		check(theme::from_name("Dark") == theme::choice::dark,
		      "reading is case-insensitive");
		check(theme::from_name(" light ") == theme::choice::light,
		      "and forgives the whitespace a hand-edited INI will have");
		check(theme::from_name("purple") == theme::choice::system,
		      "and anything unrecognised falls back to following the desktop, "
		      "which is the safe answer rather than a guess at a colour");
		check(theme::from_name("") == theme::choice::system, "as does nothing");
	}

	section("applying it changes the palette");
	{
		theme::apply(theme::choice::dark);
		const QPalette dark = QApplication::palette();
		check(dark.color(QPalette::Window).lightness() <
		          dark.color(QPalette::WindowText).lightness(),
		      "dark gives a window darker than its text");
		// The thing the settings page got wrong once: a disabled control has to
		// dim, which needs the Disabled group set explicitly.
		check(dark.color(QPalette::Disabled, QPalette::WindowText) !=
		          dark.color(QPalette::Active, QPalette::WindowText),
		      "and disabled text is dimmer than active, so greyed still looks it");

		theme::apply(theme::choice::light);
		const QPalette light = QApplication::palette();
		check(light.color(QPalette::Window).lightness() >
		          light.color(QPalette::WindowText).lightness(),
		      "light gives the opposite");
	}

	section("reading the desktop's icon theme out of its own files");
	{
		// Three separate bugs lived in this parser and every one of them ended
		// in the same place -- a toolbar with no icons and a startup line that
		// looked reasonable -- so it gets checked without a desktop.
		const QString dir = QDir::temp().filePath("hydra-icon-theme-test");
		QDir(dir).removeRecursively();
		QDir().mkpath(dir);
		auto write = [&](const QString &name, const QString &body) {
			QFile f(dir + "/" + name);
			if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
				f.write(body.toUtf8());
			return dir + "/" + name;
		};

		// **The one that actually bit.** GTK puts everything under [Settings],
		// and the first parser treated any section header as leaving [Icons] --
		// so it walked past the answer and reported the fallback.
		const QString gtk3 = write("settings.ini",
		  "[Settings]\n"
		  "gtk-application-prefer-dark-theme=true\n"
		  "gtk-cursor-theme-name=breeze_cursors\n"
		  "gtk-icon-theme-name=breeze-dark\n");
		check(theme::icon_theme_from({ gtk3 }) == "breeze-dark",
		      QString("a GTK settings.ini is read past its [Settings] header (%1)")
		          .arg(theme::icon_theme_from({ gtk3 })));

		// --- tier 4: the colour scheme the desktop wrote down ---------------
		//
		// **Both directions, from the same parser.** A fixture that only proves
		// "dark in, dark out" cannot tell a working comparison from a function
		// that returns 1. The light file is the same layout with the two
		// colours exchanged, so the only thing that can distinguish them is the
		// luminance test itself.
		const QString tde_dark = write("kdeglobals-dark",
		  "[General]\n"
		  "colorScheme=DarkBlue.kcsrc\n"
		  "windowBackground=0,42,78\n"
		  "windowForeground=220,220,220\n");
		check(theme::color_scheme_from({ tde_dark }) == 1,
		      QString("Trinity's real dark scheme reads as prefer-dark (%1)")
		          .arg(theme::color_scheme_from({ tde_dark })));

		const QString tde_light = write("kdeglobals-light",
		  "[General]\n"
		  "colorScheme=DarkBlue.kcsrc\n"     // the name still says Dark
		  "windowBackground=220,220,220\n"
		  "windowForeground=0,42,78\n");
		check(theme::color_scheme_from({ tde_light }) == 2,
		      QString("a light scheme reads as prefer-light even though the "
		               "scheme is still named DarkBlue (%1)")
		          .arg(theme::color_scheme_from({ tde_light })));

		// **The keys are only meaningful under [General].** kdeglobals carries
		// per-application sections with the same key names, and a parser that
		// took whichever came last would answer about somebody else's window.
		const QString tde_other = write("kdeglobals-other",
		  "[General]\n"
		  "windowBackground=220,220,220\n"
		  "windowForeground=0,0,0\n"
		  "[konqueror]\n"
		  "windowBackground=0,0,0\n"
		  "windowForeground=255,255,255\n");
		check(theme::color_scheme_from({ tde_other }) == 2,
		      QString("a later section does not override [General] (%1)")
		          .arg(theme::color_scheme_from({ tde_other })));

		// **Abstain rather than guess**: a wrong light answer is plain, a wrong
		// dark one is unreadable text on pale. No file, no opinion.
		check(theme::color_scheme_from({ dir + "/does-not-exist" }) == 0,
		      "an unreadable file abstains rather than guessing");

		// --- tier 4, second dialect: LXQt --------------------------------
		//
		// A desktop with no dark-mode status is the normal case, not the odd
		// one, and each such desktop spells the same statement differently.
		// LXQt uses [Palette] and #rrggbb where TDE uses [General] and
		// decimal triples, so a parser for one finds nothing in the other.
		// Both sessions are installed on the machine this was written on.
		const QString lxqt_dark = write("palette-dark",
		  "[Palette]\n"
		  "base_color=#282828\n"
		  "window_color=#232323\n"
		  "window_text_color=#e1e6e6\n");
		check(theme::color_scheme_from({ lxqt_dark }) == 1,
		      QString("LXQt's shipped Dark palette reads as prefer-dark (%1)")
		          .arg(theme::color_scheme_from({ lxqt_dark })));

		// The exchange control, as above: only the luminance test can tell
		// these two apart.
		const QString lxqt_light = write("palette-light",
		  "[Palette]\n"
		  "window_color=#efefef\n"
		  "window_text_color=#000000\n");
		check(theme::color_scheme_from({ lxqt_light }) == 2,
		      QString("LXQt's shipped Light palette reads as prefer-light (%1)")
		          .arg(theme::color_scheme_from({ lxqt_light })));

		// **The name is not a predicate, measured rather than argued.** Of
		// the twelve palettes LXQt ships, the luminance test classifies all
		// twelve correctly while EIGHT are named something that says nothing
		// -- Ambiance, Arch-Colors, Kvantum, Leech, Silver and Valendas are
		// dark, Silver-bright is light. Here the file is called "Light" and
		// holds Silver's colours, so a substring test gets it backwards.
		const QString lxqt_misnamed = write("Light",
		  "[Palette]\n"
		  "window_color=#636464\n"
		  "window_text_color=#f9f9f9\n");
		check(theme::color_scheme_from({ lxqt_misnamed }) == 1,
		      QString("a palette FILE named Light holding dark colours reads "
		               "dark (%1)")
		          .arg(theme::color_scheme_from({ lxqt_misnamed })));

		// **The applied palette lives in lxqt.conf, not in the library.**
		// This first read <data>/lxqt/palettes/<theme=>, which is wrong
		// twice: the library file is loaded only when palette_override is
		// on, so it can describe a palette nobody is using; and LXQt
		// title-cases the name before building that path, so seven of the
		// twelve themes installed here -- ambiance, dark, frost, kvantum,
		// light, silver, system -- did not resolve at all. The shipped
		// /etc/xdg/lxqt/lxqt.conf says theme=frost, so the DEFAULT install
		// silently found nothing, which is the failure this rung removes.
		const QString lxqt_conf = write("lxqt.conf",
		  "[General]\n"
		  "theme=frost\n"           // lowercase, and deliberately so
		  "icon_theme=oxygen\n"
		  "[Palette]\n"
		  "window_color=#232323\n"
		  "window_text_color=#e1e6e6\n"
		  "[Qt]\n"
		  "style=Fusion\n");
		check(theme::color_scheme_from({ lxqt_conf }) == 1,
		      QString("the applied palette is read from lxqt.conf itself, "
		               "whatever theme= says (%1)")
		          .arg(theme::color_scheme_from({ lxqt_conf })));


		// GTK 2 quotes its values.
		const QString gtk2 = write("gtkrc-2.0",
		  "gtk-icon-theme-name=\"Adwaita\"\n");
		check(theme::icon_theme_from({ gtk2 }) == "Adwaita",
		      "and a quoted GTK 2 value comes back unquoted");

		// A kdeglobals *does* need its sections: a bare Theme= outside [Icons]
		// is the colour scheme, and reading it names a palette as an icon set.
		const QString kde = write("kdeglobals",
		  "[General]\n"
		  "Theme=BreezeDarkColourScheme\n"
		  "[Icons]\n"
		  "Theme=oxygen\n");
		check(theme::icon_theme_from({ kde }) == "oxygen",
		      QString("a kdeglobals Theme= is taken from [Icons], not [General] (%1)")
		          .arg(theme::icon_theme_from({ kde })));

		// Order is authority, and a file that names nothing is skipped rather
		// than treated as an answer.
		const QString empty = write("empty.ini", "[Settings]\nunrelated=1\n");
		check(theme::icon_theme_from({ empty, gtk3 }) == "breeze-dark",
		      "a file naming no theme is passed over for the next one");
		check(theme::icon_theme_from({ "/nonexistent/nothing.ini" }).isEmpty(),
		      "and nothing at all gives nothing, not a guess");

		QDir(dir).removeRecursively();
	}

	section("finding a theme Qt cannot see by itself");
	{
		// **Qt6 fills `themeSearchPaths()` from a platform-theme plugin and
		// ships only Plasma's and GTK's.** On a desktop that loads neither the
		// list holds `:/icons` and nothing else, so every system directory is
		// invisible: a theme that is installed, configured and perfectly good
		// cannot be found, validated or loaded. Measured on the machine this
		// was written on -- Trinity, `Theme=crystalsvg` in its own kdeglobals,
		// the theme present with `go-previous` at four sizes -- and the
		// toolbar drew words.
		const QStringList before = QIcon::themeSearchPaths();
		theme::seed_icon_search_paths();
		const QStringList after = QIcon::themeSearchPaths();
		check(after.size() >= before.size(),
		      "seeding never takes a search path away");
		for (const QString &p : before)
			check(after.contains(p),
			      QString("and keeps what Qt had already (%1)").arg(p));
		// **Only where the system has any, which a bare container does not.**
		// The assertion is that seeding keeps the system's icon directories
		// reachable; on a machine with none there is nothing to keep, and the
		// check as written failed against correct code. Found by CI once it
		// started running again: the build job is a `debian:trixie` container
		// with no icon theme installed, so `locateAll` returns an empty list
		// and a loop over it can only leave `has_system` false.
		const QStringList system_icons = QStandardPaths::locateAll(
		  QStandardPaths::GenericDataLocation, "icons",
		  QStandardPaths::LocateDirectory);
		bool has_system = false;
		for (const QString &p : system_icons)
			if (after.contains(p))
				has_system = true;
		if (system_icons.isEmpty()) {
			std::printf("  --    no system icon directories on this machine, so "
			             "there are none to keep searchable\n");
		} else {
			check(has_system,
			      QString("the system icon directories are searchable (%1)")
			          .arg(after.join(", ").left(90)));
		}

		theme::seed_icon_search_paths();
		check(QIcon::themeSearchPaths() == after,
		      "and calling it twice changes nothing");

		// Every candidate names a theme that is really there.
		const QStringList found = theme::detect_icon_themes();
		bool all_present = true;
		for (const QString &t : found) {
			bool here = false;
			for (const QString &dir : QIcon::themeSearchPaths())
				if (QFile::exists(dir + "/" + t + "/index.theme"))
					here = true;
			if (!here)
				all_present = false;
		}
		check(all_present,
		      QString("every candidate is installed (%1)")
		          .arg(found.join(", ")));

		// **The invariant that broke, and the reason this loops rather than
		// taking the first hit.** Being installed is not being able to draw:
		// Adwaita ships an index.theme, a cursor set and symbolic icons, and
		// carries none of the ordinary action icons a toolbar asks for. It
		// passes every test but drawing, exactly as hicolor does -- so
		// whatever is settled on must answer for a real icon, or the startup
		// line reports success over an empty toolbar.
		const QString chosen = theme::apply_icon_theme(Qt::ColorScheme::Light);
		if (chosen.isEmpty()) {
			std::printf("  --    no icon theme on this machine; the choice "
			             "cannot be checked here\n");
		} else {
			const QIcon probe = QIcon::fromTheme("go-previous");
			check(!probe.isNull() && !probe.availableSizes().isEmpty(),
			      QString("what it settled on can actually draw (%1)").arg(chosen));
			check(QIcon::themeName() == chosen,
			      "and is what it left set on QIcon");

			// How many of the installed candidates would have drawn nothing.
			// On this machine the configured theme comes first and works, so
			// the loop never has to skip -- which is worth saying out loud,
			// because it means the probing is hardening for a machine with no
			// desktop configuration (where the list starts at breeze,
			// Adwaita) rather than what fixed the toolbar here.
			QStringList useless;
			for (const QString &t : found) {
				QIcon::setThemeName(t);
				const QIcon p = QIcon::fromTheme("go-previous");
				if (p.isNull() || p.availableSizes().isEmpty())
					useless << t;
			}
			QIcon::setThemeName(chosen);
			std::printf("  --    installed candidates that draw nothing: %s\n",
			             useless.isEmpty() ? "none"
			                                : qPrintable(useless.join(", ")));
			check(!useless.contains(chosen),
			      "and the one chosen is not among the ones that cannot draw");
		}
	}

	section("the two colours this tree writes by hand");
	{
		// **Everything else asks the palette; these two do not.** The tab tree
		// paints an unopened link mid-grey, and the downloads list paints the
		// "public" marker amber. Both are deliberate -- one is a shade of the
		// normal text colour, the other is a warning that must not read as
		// ordinary -- and both are frozen numbers that a colour scheme cannot
		// move.
		//
		// That is the shape of a bug this tree has had: the settings
		// descriptions dimmed by writing a colour, which froze under whichever
		// scheme was current when the widget was built, and a contrast check
		// exists there because of it. These two survive both schemes, and the
		// point of measuring is that nobody knew it -- a mid-grey happens to
		// clear a dark background and a light one, and "happens to" is the part
		// worth holding still.
		// **Asked of the model, not copied from it.** Restating the literal here
		// would test this file against itself: change the colour in
		// `tab_tree_model.cpp` and a copy sitting in the test agrees with the
		// old value forever. So the model is built and asked what it paints,
		// which is the thing that would actually change.
		tab_tree_model model;
		node *unopened = model.add_tab(nullptr, "A tab nobody opened",
		                                "https://example.invalid/");
		const QVariant ink = model.data(model.index_for_node(unopened),
		                                 Qt::ForegroundRole);
		check(ink.canConvert<QBrush>() && ink.value<QBrush>().color().isValid(),
		      "the tree paints an unopened tab in a colour of its own");

		const theme::choice both[] = { theme::choice::light, theme::choice::dark };
		for (theme::choice c : both) {
			theme::apply(c);
			const QColor bg = QApplication::palette().color(QPalette::Base);
			const QColor fg = ink.value<QBrush>().color();
			const int gap = std::abs(fg.lightness() - bg.lightness());
			check(gap >= 25,
			      QString("%1: an unopened tab stands %2 lightness levels off "
			               "the tree behind it").arg(
			          c == theme::choice::light ? "light" : "dark").arg(gap));
		}

		// The downloads list writes one too -- amber, for a transfer whose
		// address is visible to others. It is deliberately not checked here:
		// reaching it means a live download, and asserting against a second
		// copy of the literal would be this file agreeing with itself. Named
		// so the omission is a decision rather than an oversight.
	}

	// **Can you see which toolbar button is held down?** Reported from the
	// desktop, and measured here by drawing a real QToolButton in both states
	// and reading the pixels rather than by reasoning about palette roles.
	section("a held button is distinguishable from one that is not");
	{
		// Gamma-correct relative luminance, the WCAG form the workspace
		// settled on. Any correct luminance answers this comparison -- what
		// matters is that both sides use the same one.
		// **The app installs a proxy style and this suite did not**, so the
		// first run of this measurement reported the platform style's answer
		// while the running program uses another. The held fill is drawn by
		// that proxy, so without this the numbers below describe a build
		// nobody ships -- and they were identical before and after the fix,
		// which is what said so.
		theme::install_icon_style();

		// The centre of a rendered button in a given check state. Grabbed
		// rather than computed: which palette role a style reaches for to
		// draw "on" is the style's business, and the question here is what
		// the person actually sees.
		auto fill_of = [](bool on) {
			QToolButton b;
			b.setCheckable(true);
			b.setChecked(on);
			b.setText("X");
			b.resize(40, 40);
			const QPixmap shot = b.grab();
			// Two pixels in from a corner: inside the button's own panel and
			// away from the text in the middle.
			return shot.toImage().pixelColor(4, 4);
		};

		for (const auto scheme : {theme::choice::dark, theme::choice::light}) {
			theme::apply(scheme);
			const char *what = scheme == theme::choice::dark ? "dark" : "light";
			const QColor off = fill_of(false), on = fill_of(true);
			const double held = contrast(on, off);
			// **1.5, chosen to reject what was measured before the fix.**
			// The first version of this check said 1.2 -- written before the
			// numbers were in -- and the reported bug measured 1.22 in dark
			// and 1.36 in light, so it passed the very thing it was added
			// for. A floor picked to sit above whatever the code currently
			// does is not a floor.
			//
			// It is a visibility target rather than a legibility one: two
			// backgrounds are being compared, not a colour against its own
			// text, so the 3:1 the workspace settled on for that does not
			// transfer. `held_fill` aims at 1.6 and this leaves a little
			// room under it for a style that draws its own panel over ours.
			check(held >= 1.5,
			      QString("%1: held and unheld differ (%2:1, %3 vs %4)")
			          .arg(what).arg(held, 0, 'f', 2)
			          .arg(on.name(), off.name()));
			const QColor label =
			    QApplication::palette().color(QPalette::ButtonText);
			const double readable = contrast(label, on);
			check(readable >= 3.0,
			      QString("%1: and its label is still readable on it (%2:1)")
			          .arg(what).arg(readable, 0, 'f', 2));
		}
	}

	// **The five roles a style shades with have to belong to the same scheme
	// as the text drawn on them.** `dark_palette()` overrode thirteen roles
	// and left these at whatever `QPalette p;` had copied -- the light
	// desktop's -- so a dark window shaded a frame or a header near-white and
	// then drew near-white `WindowText` on it. Reported from the desktop as
	// text that was impossible to read.
	//
	// Asserted as a relationship rather than as five colours, so it cannot go
	// stale the next time the window colour moves: whatever the scheme, every
	// shading role must sit on the same side of the text as the window does.
	section("a scheme's shading roles belong to that scheme");
	{
		const QPalette::ColorRole shading[] = {
			QPalette::Light, QPalette::Midlight, QPalette::Mid,
			QPalette::Dark, QPalette::Shadow
		};
		const char *names[] = { "Light", "Midlight", "Mid", "Dark", "Shadow" };

		QList<int> dark_values, light_values;
		for (const auto scheme : { theme::choice::dark, theme::choice::light }) {
			theme::apply(scheme);
			const QPalette p = QApplication::palette();
			const char *what = scheme == theme::choice::dark ? "dark" : "light";
			QList<int> &into =
			    scheme == theme::choice::dark ? dark_values : light_values;
			for (auto r : shading)
				into << p.color(r).lightness();

			// **`Light` is the one text lands on**, and the only one of the
			// five with a legibility question. `Dark` and `Shadow` are meant
			// to be dark in a light scheme -- they are edges and shadows, not
			// grounds -- so asking all five to sit near the window fails a
			// correct light palette, which is what the first version of this
			// check did before the sabotage showed it.
			const int light_role = p.color(QPalette::Light).lightness();
			const int text = p.color(QPalette::WindowText).lightness();
			const int win  = p.color(QPalette::Window).lightness();
			check(qAbs(light_role - win) < qAbs(light_role - text),
			      QString("%1: Light (%2) is the window's (%3), not the text's "
			               "(%4)").arg(what).arg(light_role).arg(win).arg(text));

			check(p.color(QPalette::Light).lightness() >
			          p.color(QPalette::Midlight).lightness() &&
			      p.color(QPalette::Midlight).lightness() >
			          p.color(QPalette::Mid).lightness() &&
			      p.color(QPalette::Mid).lightness() >
			          p.color(QPalette::Dark).lightness() &&
			      p.color(QPalette::Dark).lightness() >
			          p.color(QPalette::Shadow).lightness(),
			      QString("%1: and they run Light > Midlight > Mid > Dark > "
			               "Shadow").arg(what));
		}

		// **And the thing itself: can the text be read on it.** The checks
		// above are about where a colour sits; this is about what a person
		// sees, and it is the one that names the reported symptom in its own
		// units. Measured with the fix reverted, `WindowText` reads on
		// `Light` at **1.25:1**, on `Midlight` at 1.31 and on `Mid` at 1.59 --
		// all of them invisible. (1.09 appeared here first, from arithmetic
		// rather than from the run; the numbers above are the run's.)
		//
		// Only the dark palette, because it is the only one this project
		// owns. `apply()` hands the light scheme back to the style on purpose
		// -- "a desktop's light theme is the user's" -- so asserting on it
		// would be asserting on Qt's choices rather than on ours.
		theme::apply(theme::choice::dark);
		{
			const QPalette p = QApplication::palette();
			const QPalette::ColorRole grounds[] = {
				QPalette::Window, QPalette::Base, QPalette::AlternateBase,
				QPalette::Button, QPalette::Light, QPalette::Midlight,
				QPalette::Mid
			};
			const char *gnames[] = { "Window", "Base", "AlternateBase",
			                          "Button", "Light", "Midlight", "Mid" };
			// 3:1 is the floor this workspace settled on for interface text,
			// and it is deliberately not 4.5: that is the body-text figure,
			// and holding a chrome palette to it would fail the highlight
			// colour every desktop ships.
			for (int i = 0; i < 7; ++i) {
				const double r = contrast(p.color(QPalette::WindowText),
				                           p.color(grounds[i]));
				check(r >= 3.0,
				      QString("dark: WindowText reads on %1 (%2:1)")
				          .arg(gnames[i]).arg(r, 0, 'f', 2));
			}
		}

		// **The defect stated exactly.** `dark_palette()` overrode thirteen
		// roles and left these five at whatever `QPalette p;` had copied, so
		// they were the light desktop's -- byte for byte the same list in both
		// schemes. Nothing about either list alone says that; the equality
		// does.
		check(dark_values != light_values,
		      QString("the two schemes do not share one set of shading roles "
		               "(dark %1,%2,%3,%4,%5)")
		          .arg(dark_values.value(0)).arg(dark_values.value(1))
		          .arg(dark_values.value(2)).arg(dark_values.value(3))
		          .arg(dark_values.value(4)));
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
