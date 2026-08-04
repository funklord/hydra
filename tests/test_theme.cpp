// Which colour scheme the desktop is in (architecture doc §6.1).
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
#include <QFile>
#include <QDir>

#include <QApplication>
#include <QPalette>
#include <cstdio>

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
		// Portal 0 is "no preference" — a real answer, not a missing one — and a
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

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
