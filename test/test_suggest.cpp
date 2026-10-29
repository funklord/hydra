// What the address bar suggests while somebody types, from the tree.
//
// Pure: the ranking is `address_suggest::suggest`, and this checks it
// without a window. The window half -- the popup, and choosing one going
// there -- is `try_navigate`'s.
#include "address_suggest.h"

#include <QCoreApplication>
#include <cstdio>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void section(const char *n) { std::printf("\n== %s ==\n", n); }

using namespace address_suggest;

static QStringList urls(const QList<suggestion> &s) {
	QStringList out;
	for (const suggestion &x : s)
		out << x.url;
	return out;
}

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);
	const QDateTime now = QDateTime::fromString("2026-10-29T12:00:00", Qt::ISODate);
	const QList<page> tree = {
		{ "Rust documentation", "https://doc.rust-lang.org/std/", now.addDays(-3) },
		{ "Weather in Stockholm", "https://www.yr.no/en/forecast/stockholm", now.addDays(-1) },
		{ "Hacker News", "https://news.ycombinator.com/", now.addDays(-10) },
		{ "Hacker News", "https://news.ycombinator.com/", now },
		{ "news from home", "https://example.org/news", now.addDays(-2) },
		{ "", "about:blank", now },
		{ "Tracker issue 4231", "https://github.com/x/y/issues/4231", now.addDays(-5) },
		{ "GitHub", "https://github.com/", now.addDays(-30) },
	};

	section("what is offered, and in what order");
	{
		check(urls(suggest("git", tree)) ==
		        QStringList{ "https://github.com/x/y/issues/4231", "https://github.com/" },
		      QString("an address that starts with it, the recent first (%1)")
		          .arg(urls(suggest("git", tree)).join(' ')));
		check(urls(suggest("www.yr", tree)) == QStringList{ "https://www.yr.no/en/forecast/stockholm" } &&
		        urls(suggest("yr.no", tree)) == urls(suggest("https://yr.no", tree)),
		      "the scheme and www. are nobody's to type");
		check(urls(suggest("weather sto", tree)) ==
		        QStringList{ "https://www.yr.no/en/forecast/stockholm" },
		      "every typed word starting a word of the title");
		const QList<suggestion> news = suggest("news", tree);
		check(news.size() == 2 && news[0].url == "https://news.ycombinator.com/" &&
		        news[0].rank == 3 && news[1].url == "https://example.org/news" &&
		        news[1].rank == 2,
		      QString("an address match outranks a title match (%1)")
		          .arg(urls(news).join(' ')));
		check(urls(suggest("4231", tree)) == QStringList{ "https://github.com/x/y/issues/4231" },
		      "a word of the title, which is a number");
		check(urls(suggest("issues/42", tree)) == QStringList{ "https://github.com/x/y/issues/4231" },
		      "and failing both, anything the address contains");
	}

	section("what is not offered");
	{
		check(suggest("news", tree).size() == 2,
		      "one address once, though the tree holds it twice");
		check(suggest("blank", tree).isEmpty(), "never an empty tab's about:blank");
		check(suggest("", tree).isEmpty() && suggest("   ", tree).isEmpty(),
		      "nothing for nothing typed");
		check(suggest("zzz", tree).isEmpty(), "nothing that matches nothing");
		check(suggest("h", tree, 2).size() == 2, "and no more than asked for");
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
