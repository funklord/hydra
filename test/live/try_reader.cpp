// Reader mode's extractor against real pages in a real engine, offline.
//
//     ./test/build-make/try_reader
//     HYDRA_READER_URL=https://... ./test/build-make/try_reader
//
// The second form is observational: it loads a real page over the network
// and prints what the extractor kept, for judging it on sites nobody here
// controls. It asserts nothing.
//
// Each fixture is shown with `show_html` at an address of its own, so nothing
// is fetched, and the extractor is asked through `run_probe` exactly as the
// window asks it. What it keeps and what it leaves out are both checked: an
// extractor that returned the whole page would find every paragraph.
#include "policy_engine.h"
#include "qtwebengine_factory.h"
#include "reader_extract.h"
#include "reader_page.h"
#include "request_filter.h"
#include "web_view_backend.h"

#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QWidget>
#include <cstdio>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void section(const char *n) { std::printf("\n== %s ==\n", n); }
static void spin(int ms) {
	QEventLoop l;
	QTimer::singleShot(ms, &l, &QEventLoop::quit);
	l.exec();
}

// A news page the way news pages are: navigation, a sidebar of links, an ad
// slot, a hidden block, a script, comments and a footer around six paragraphs.
static const char k_news[] = R"HTML(<!doctype html>
<html lang="en"><head><title>Rivers rise in the north | Daily Example</title>
<meta property="og:title" content="Rivers rise in the north">
<meta property="og:site_name" content="Daily Example">
<meta name="author" content="Ann Writer">
<script>window.trackMe = 1;</script></head>
<body>
<header class="masthead"><a href="/">Daily Example</a>
<nav><a href="/world">World</a> <a href="/sport">Sport</a> <a href="/culture">Culture</a></nav></header>
<div class="layout">
<aside class="sidebar"><h3>Most read</h3><ul>
<li><a href="/a">Something everyone is reading today about other things</a></li>
<li><a href="/b">Another popular story that is not this one at all</a></li>
<li><a href="/c">A third headline in the list of most read stories</a></li></ul></aside>
<div class="ad-slot">ADVERTISEMENT buy the thing now while stocks last, limited offer</div>
<article class="story">
<h1>Rivers rise in the north</h1>
<p>Rivers across the northern provinces rose to their highest level in a decade on Tuesday, after three days of rain that forecasters had warned about since the weekend.</p>
<p>In the town of Exampleby, water reached the steps of the old church by noon, and the council opened the school gymnasium to families from the lowest streets, who arrived with blankets, pets and photographs.</p>
<p>"We have seen high water before, but never this quickly," said the harbour master, who has watched the river for thirty years. "By the time we had the barriers out, the quay was already gone."</p>
<figure><img src="/img/river.jpg" width="800" alt="The flooded quay"><figcaption>The quay at noon.</figcaption></figure>
<p>Forecasters expect the rain to ease on Thursday, but warned that the rivers would keep rising for a day after it stops, as water from the hills reaches the valleys.</p>
<p>Readers can follow the <a href="https://example.org/levels" onclick="track()">live river levels</a>, updated every fifteen minutes by the national agency, and the <a href="javascript:openMap()">flood map</a>.</p>
<div style="display:none">HIDDEN PROMOTIONAL TEXT that a person never sees on the page, and neither should a reader.</div>
<p>Schools in the three worst-hit districts will stay closed until Friday, the regional government said in a statement on Tuesday evening.</p>
</article>
<section class="comments"><h3>Comments</h3>
<p>COMMENT: I live by the river and it has never been this bad, truly, never in my life.</p>
<p>COMMENT: Thoughts with everyone affected, stay safe out there, everyone, please.</p></section>
</div>
<footer><p>FOOTER copyright Daily Example, all rights reserved, terms and privacy apply.</p></footer>
</body></html>)HTML";

// A blog that writes its text into divs separated by <br>, with no <p> at all.
static const char k_divs[] = R"HTML(<!doctype html>
<html><head><title>Notes on bread | Blog Example</title></head><body>
<div id="menu"><a href="/">Home</a> <a href="/about">About</a></div>
<div class="post-body">Bread wants time more than it wants work, and most of the failures I have had came from hurrying the first rise, not from kneading too little.<br><br>
A cold proof overnight in the refrigerator changes the crumb more than any flour I have tried, and it moves the baking to a morning when there is time for it.<br><br>
Salt goes in after the autolyse, not before, because it tightens the dough and makes the flour harder to hydrate evenly, which shows up as dense patches later.<br><br>
Steam for the first fifteen minutes, then open the oven door once to let it out, and the crust will colour properly instead of staying pale and soft.</div>
</body></html>)HTML";

// A page that is all links and buttons: nothing a person would call an article.
static const char k_index[] = R"HTML(<!doctype html>
<html><head><title>Example portal</title></head><body>
<nav><a href="/1">One</a> <a href="/2">Two</a> <a href="/3">Three</a></nav>
<ul><li><a href="/x">Weather</a></li><li><a href="/y">Mail</a></li><li><a href="/z">Maps</a></li></ul>
<form><input name="q"><button>Search</button></form>
</body></html>)HTML";

// Right to left, and named in its own language.
static const char k_rtl[] = R"HTML(<!doctype html>
<html lang="ar" dir="rtl"><head><title>RTL</title></head><body><main>
<p>This paragraph stands in for Arabic text written right to left, long enough to be an article on its own merits, with commas, clauses, and length.</p>
<p>And a second paragraph of the same kind, so that the container holding both is plainly the article, scored by its length, its commas, and its place.</p>
</main></body></html>)HTML";

static bool wait_for_load(web_view_backend *v, int ms = 15000) {
	bool done = false;
	QMetaObject::Connection c =
	  QObject::connect(v, &web_view_backend::load_finished, v, [&](bool) { done = true; });
	for (int i = 0; i < ms / 50 && !done; ++i)
		spin(50);
	QObject::disconnect(c);
	return done;
}

static QString ask(web_view_backend *v, const QString &js, bool page_world = false) {
	QString out;
	bool got = false;
	auto take = [&](const QString &s) { out = s; got = true; };
	if (page_world)
		v->run_page_probe(js, take);
	else
		v->run_probe(js, take);
	for (int i = 0; i < 200 && !got; ++i)
		spin(50);
	return got ? out : QStringLiteral("(no answer)");
}

static bool extract(web_view_backend *v, const char *html, const QUrl &at,
                    reader_extract::article *a, QString *why) {
	v->show_html(QString::fromUtf8(html), at);
	if (!wait_for_load(v)) {
		*why = "the fixture never finished loading";
		return false;
	}
	return reader_extract::parse(ask(v, reader_extract::source()), a, why);
}

int main(int argc, char *argv[]) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QTemporaryDir home;
	const QByteArray base = home.path().toLocal8Bit();
	qputenv("HOME", base);
	qputenv("XDG_CONFIG_HOME", base + "/config");
	qputenv("XDG_DATA_HOME", base + "/data");
	qputenv("XDG_CACHE_HOME", base + "/cache");
	QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
	QApplication app(argc, argv);

	policy_engine policy;
	request_filter filter(&policy);
	qtwebengine_factory factory(&filter);
	QWidget host;
	host.resize(1000, 800);
	web_view_backend *view = factory.create_view(&host);
	view->widget()->resize(1000, 800);
	host.show();
	view->apply_settings(view_settings());

	const QString live = qEnvironmentVariable("HYDRA_READER_URL");
	if (!live.isEmpty()) {
		view->load(QUrl(live));
		if (!wait_for_load(view, 45000))
			std::printf("did not finish loading in 45 s; asking anyway\n");
		spin(3000);   // pages that build themselves after the load
		reader_extract::article a;
		QString why;
		if (!reader_extract::parse(ask(view, reader_extract::source()), &a, &why)) {
			std::printf("no article: %s\n", qPrintable(why));
			return 0;
		}
		QString plain = a.html;
		plain.remove(QRegularExpression("<[^>]*>"));
		std::printf("title:  %s\nbyline: %s\nsite:   %s\nwords:  %d\n\nstart:  %s\n\nend:    %s\n",
		            qPrintable(a.title), qPrintable(a.byline), qPrintable(a.site), a.words,
		            qPrintable(plain.left(500).simplified()),
		            qPrintable(plain.right(400).simplified()));
		return 0;
	}

	const QUrl news("https://news.example/2026/10/rivers");

	section("a news page: the story, and nothing around it");
	{
		reader_extract::article a;
		QString why;
		const bool ok = extract(view, k_news, news, &a, &why);
		check(ok, QString("an article was found (%1)").arg(ok ? "yes" : why));
		check(view->url() == news, QString("the fixture kept its address (%1)")
		                             .arg(view->url().toString()));
		check(a.title == "Rivers rise in the north", "the title, from og:title: " + a.title);
		check(a.site == "Daily Example" && a.byline == "Ann Writer",
		      QString("site and byline (%1 / %2)").arg(a.site, a.byline));
		check(a.lang == "en" && a.dir == "ltr", "language and direction");
		const char *kept[] = { "highest level in a decade", "steps of the old church",
		                       "harbour master", "ease on Thursday", "live river levels",
		                       "stay closed until Friday", "The quay at noon." };
		int found = 0;
		for (const char *k : kept)
			found += a.html.contains(QLatin1String(k)) ? 1 : 0;
		check(found == 7, QString("all of the story's text is there (%1 of 7)").arg(found));
		const char *gone[] = { "Most read", "Sport", "ADVERTISEMENT", "HIDDEN PROMOTIONAL",
		                       "COMMENT:", "FOOTER", "trackMe" };
		QStringList leaked;
		for (const char *g : gone)
			if (a.html.contains(QLatin1String(g)))
				leaked << g;
		check(leaked.isEmpty(), "nav, sidebar, ad, hidden text, comments, footer and "
		                        "script all left out (" + leaked.join(", ") + ")");
		check(!a.html.contains("<h2>Rivers rise") && !a.html.contains("<h1"),
		      "the headline is not repeated inside the article");
		check(!a.html.contains("onclick") && !a.html.contains("javascript:") &&
		        !a.html.contains("<script") && !a.html.contains("style="),
		      "no handler, script url, script or style survives");
		const int at = a.html.indexOf("Readers can follow");
		check(a.html.contains("<a href=\"https://example.org/levels\">live river levels</a>") &&
		        a.html.contains("the flood map") && !a.html.contains("openMap"),
		      "a real link is kept; the script link keeps its words and loses its target (" +
		        a.html.mid(at, 220) + ")");
		check(a.html.contains("src=\"https://news.example/img/river.jpg\"") &&
		        a.html.contains("alt=\"The flooded quay\""),
		      "the picture, its address made absolute");
		check(a.words > 150 && a.words < 260, QString("a word count (%1)").arg(a.words));

		// The reader document, shown the way the window shows it.
		a.html += QStringLiteral("<script>document.title = 'a script ran';</script>");
		view->show_html(reader_page::build(a, news, false), news);
		check(wait_for_load(view), "the reader document loads");
		check(view->url() == news, "and the tab's address is still the page's");
		check(ask(view, "document.title", true) == "Rivers rise in the north",
		      "a script that slipped into the article does not run");
		check(ask(view, "document.querySelector('article p') ? 'yes' : 'no'") == "yes",
		      "the article is in the document");
	}

	section("a blog written in divs");
	{
		reader_extract::article a;
		QString why;
		const bool ok = extract(view, k_divs, QUrl("https://blog.example/bread"), &a, &why);
		check(ok && a.html.contains("cold proof overnight") && a.html.contains("Steam for the"),
		      QString("text in divs separated by <br> is an article too (%1)")
		        .arg(ok ? "found" : why));
		check(!a.html.contains(">About<"), "and its menu is not");
		check(a.html.contains("<br>"), "the breaks between its paragraphs are kept");
		check(a.title == "Notes on bread",
		      "a title's trailing site name is dropped when it names the host: " + a.title);
	}

	section("a page with no article");
	{
		reader_extract::article a;
		QString why;
		const bool ok = extract(view, k_index, QUrl("https://portal.example/"), &a, &why);
		check(!ok && why == "No article was found on this page.",
		      "a page of links is refused, with the reason: " + why);
	}

	section("right to left");
	{
		reader_extract::article a;
		QString why;
		const bool ok = extract(view, k_rtl, QUrl("https://rtl.example/"), &a, &why);
		check(ok && a.dir == "rtl" && a.lang == "ar",
		      QString("direction and language carried over (%1 %2)").arg(a.dir, a.lang));
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
