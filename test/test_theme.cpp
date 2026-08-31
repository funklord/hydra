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
#include <QPalette>
#include <cstdio>
#include <cstdlib>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void section(const char *n) { std::printf("\n== %s ==\n", n); }

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
		bool has_system = false;
		for (const QString &p : QStandardPaths::locateAll(
		         QStandardPaths::GenericDataLocation, "icons",
		         QStandardPaths::LocateDirectory))
			if (after.contains(p))
				has_system = true;
		check(has_system,
		      QString("the system icon directories are searchable (%1)")
		          .arg(after.join(", ").left(90)));

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

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
