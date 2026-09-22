// Every piece of JavaScript this browser injects into a page, parsed.
//
// **Nothing parsed any of it.** Eleven scripts live in `src/` as C++ raw
// string literals -- the element picker, the autofill filler, the consent
// blocker, the MSE tap and its subframe relay, the cosmetic-filter
// stylesheet, the permissions shim and its device-label half, the client-
// hints shim, and the two channel bootstraps. The compiler sees them as text.
// A missing brace or a stray character in any of them is a program that
// builds, ships, and then does nothing on a page, with no error anywhere the
// browser can see: `runJavaScript` reports a syntax error to the engine's own
// console, which is not this process.
//
// **Read out of the tree rather than listed here**, and that is the point
// rather than a convenience. Seven of the eleven are `const char *` in an
// anonymous namespace inside a `.cpp`, so no test can name them, and a test
// that listed the four it *could* reach would be a check whose name says
// "every script" over a population its author wrote down by hand. Reading
// `src/` means a script added next year is covered the day it is added.
//
// **Parsed, not run.** Each body goes into a function expression that is
// never called, so the whole text is parsed and none of it executes. That
// also means no watchdog: `site_extractor` needs one because it *calls* what
// it compiles, and a tight loop in JS never yields. Nothing here is called,
// so nothing here can loop.
//
// The one thing a wrapper permits that a page would not is a `return` at the
// top level. Every blob in the tree opens `(function`, which is asserted
// below, so none of them is in that shape -- and a new one that is would fail
// that check rather than slipping through this one.
#include "autofill_script.h"
#include "permissions_shim.h"
#include "picker_script.h"
#include "user_agent.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJSEngine>
#include <QJSValue>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <cstdio>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void section(const char *n) { std::printf("\n== %s ==\n", n); }

// One blob, as found.
struct blob {
	QString file;
	int     line = 0;
	QString body;
};

// `%1`-style placeholders are QString::arg's, not JavaScript, so the template
// as written is not a program. A bare identifier is what stands in: it parses
// wherever a value may appear, including inside a string literal, and it is
// the only substitution that is safe in both positions.
//
// **The first version used `"x"` and produced a failure that looked like a
// finding.** `permissions_shim` writes `states = { camera: "%1" }` -- the
// quotes are already in the template -- so a quoted substitution made `""x""`
// and the checker reported a SyntaxError in a file that is perfectly good.
// The section below parses what the real call produces, which is the answer
// to what the template becomes; this one only has to keep the template
// parseable enough to see a real typo.
static QString fill_placeholders(const QString &js) {
	QString out = js;
	for (int i = 9; i >= 1; --i)
		out.replace(QString("%%1").arg(i), QStringLiteral("x"));
	return out;
}

// Parsed without being run: the body becomes a function expression that
// nothing calls. Returns an empty string when it parses, or the message.
static QString syntax_error(QJSEngine &engine, const QString &js) {
	const QJSValue v =
	  engine.evaluate("(function () {\n" + fill_placeholders(js) + "\n})");
	if (v.isError() && v.errorType() == QJSValue::SyntaxError)
		return v.toString() + QString(" (line %1)")
		                        .arg(v.property("lineNumber").toInt());
	return QString();
}

// Where `src/` is. `HYDRA_SRC_DIR` comes from `test/Makefile` and is the
// answer that always works; the search after it is for a build that does not
// set it -- fmake compiles this file to compute the link sets and knows
// nothing of that Makefile's defines, so a hard dependency on the macro would
// be a suite that cannot be built by the tool the tree also builds with.
//
// An empty answer is a failure rather than a skip: a script checker that
// cannot find the scripts has checked nothing, and saying so is the whole
// difference between that and a clean run.
static QString source_dir() {
#ifdef HYDRA_SRC_DIR
	if (QFileInfo(QStringLiteral(HYDRA_SRC_DIR)).isDir())
		return QStringLiteral(HYDRA_SRC_DIR);
#endif
	for (const char *guess : { "src", "../src", "../../src" })
		if (QFileInfo(QString::fromLatin1(guess)).isDir() &&
		    QFileInfo(QString::fromLatin1(guess) + "/main.cpp").isFile())
			return QString::fromLatin1(guess);
	return QString();
}

static QList<blob> scripts_in_tree(const QString &dir) {
	QList<blob> found;
	const QStringList files =
	  QDir(dir).entryList({ "*.cpp", "*.h" }, QDir::Files, QDir::Name);
	for (const QString &name : files) {
		QFile f(QDir(dir).filePath(name));
		if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
			continue;
		const QString text = QString::fromUtf8(f.readAll());
		int at = 0;
		while ((at = text.indexOf(QStringLiteral("R\"JS("), at)) >= 0) {
			const int from = at + 5;
			const int end  = text.indexOf(QStringLiteral(")JS\""), from);
			if (end < 0)
				break;
			blob b;
			b.file = name;
			b.line = int(QStringView(text).left(at).count(u'\n')) + 1;
			b.body = text.mid(from, end - from);
			found.append(b);
			at = end + 4;
		}
	}
	return found;
}

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);
	QJSEngine engine;

	const QString src = source_dir();

	section("the checker can tell a broken script from a whole one");
	{
		// A positive control, first, because everything below is a silence:
		// a checker that never reports would pass every script in the tree
		// exactly as loudly as one that reads them.
		check(syntax_error(engine, "(function () { if (1 { } })()")
		        .contains("SyntaxError"),
		      "a missing parenthesis is reported as a SyntaxError");
		check(syntax_error(engine, "(function () { var x = 1; })()").isEmpty(),
		      "and a whole script is not");
		// The failure a page would show is a *reference* error, not a syntax
		// one, and this check must not confuse them: these scripts all touch
		// `window` on their first line and none of it runs here.
		check(syntax_error(engine, "(function () { window.x = 1; })()").isEmpty(),
		      "while a script naming things a page has is not an error here");
	}

	section("every injected script in the tree parses");
	{
		check(!src.isEmpty(),
		      QString("the source directory was found (%1)")
		        .arg(src.isEmpty() ? QStringLiteral("(none)") : src));
		const QList<blob> found = scripts_in_tree(src);

		// A floor, because this list is computed: a rename, a move, or a
		// changed raw-string tag would leave it empty, and a loop over
		// nothing reports success exactly as loudly as a real pass.
		check(found.size() >= 8,
		      QString("%1 injected script(s) found; expected at least 8")
		        .arg(found.size()));

		for (const blob &b : found) {
			const QString err = syntax_error(engine, b.body);
			check(err.isEmpty(),
			      QString("%1:%2 (%3 chars) parses%4")
			        .arg(b.file).arg(b.line).arg(b.body.size())
			        .arg(err.isEmpty() ? QString() : " -- " + err));
		}

		// The wrapper above would accept a top-level `return`, which a page
		// would refuse. Every blob is an immediately-invoked function, so
		// none is in that shape -- asserted rather than assumed, because it
		// is the one hole the parse check has.
		int iife = 0;
		for (const blob &b : found)
			if (b.body.trimmed().startsWith(QStringLiteral("(function")))
				++iife;
		check(iife == found.size(),
		      QString("all %1 open with (function, so the wrapper changes "
		               "nothing about how they parse (%2)")
		        .arg(found.size()).arg(iife));
	}

	section("and so does what the substituted ones actually send");
	{
		// The four reachable by name, called the way the browser calls them,
		// so the text under test is the text a page receives rather than the
		// template it came from. `permissions_shim::source` is the only one
		// that substitutes anything.
		struct named { const char *what; QString js; };
		const QList<named> live = {
			{ "picker_script::source",      QString::fromUtf8(picker_script::source()) },
			{ "autofill_script::source",    QString::fromUtf8(autofill_script::source()) },
			{ "permissions_shim::source",   permissions_shim::source("granted", "prompt") },
			{ "permissions_shim::device_labels", permissions_shim::device_labels() },
			{ "user_agent::client_hints_shim",   user_agent::client_hints_shim() },
		};
		for (const named &n : live) {
			const QString err = syntax_error(engine, n.js);
			check(err.isEmpty(),
			      QString("%1 parses%2").arg(n.what)
			        .arg(err.isEmpty() ? QString() : " -- " + err));
		}
		// The substitution is the reason this section exists, so say it
		// happened: a shim still carrying %1 would parse as a string here and
		// be a syntax error in a page.
		const QString filled = permissions_shim::source("granted", "prompt");
		check(!filled.contains(QStringLiteral("%1")) &&
		         filled.contains(QStringLiteral("granted")),
		      "and the permission shim really was filled in");
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
