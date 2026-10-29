// Reader mode's two pure halves: reading the extractor's answer, and the
// document built from it.
//
// The extractor itself runs in a page and is driven through a real engine by
// `test/live/try_reader.cpp`; the window's toggle is `test_rotation`'s.
#include "reader_extract.h"
#include "reader_page.h"

#include <QCoreApplication>
#include <cstdio>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void section(const char *n) { std::printf("\n== %s ==\n", n); }

using reader_extract::article;

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);

	section("reading the extractor's answer");
	{
		article a;
		QString why;
		check(reader_extract::parse(
		        R"({"ok":true,"title":"A story","byline":"Ann Writer","site":"News",)"
		        R"("lang":"sv","dir":"rtl","html":"<p>Text.</p>","words":460})", &a, &why) &&
		        a.title == "A story" && a.byline == "Ann Writer" && a.site == "News" &&
		        a.lang == "sv" && a.dir == "rtl" && a.html == "<p>Text.</p>" && a.words == 460,
		      "every field of an answer arrives");
		check(!reader_extract::parse(R"({"ok":false,"why":"No article was found on this page."})",
		                             &a, &why) &&
		        why == "No article was found on this page.",
		      "a page with no article says so, in the extractor's words");
		check(!reader_extract::parse(QString(), &a, &why) && why == "This page could not be read.",
		      "an empty answer -- a view that cannot run scripts -- is not an article");
		check(!reader_extract::parse("not json", &a, &why) && !why.isEmpty(),
		      "nor is an answer that does not parse");
		check(!reader_extract::parse(R"({"ok":true,"title":"T","html":"  "})", &a, &why),
		      "nor one whose article is empty");
		check(reader_extract::parse(R"({"ok":true,"html":"<p>x</p>","dir":"sideways"})",
		                            &a, &why) && a.dir == "ltr",
		      "a direction other than rtl reads as ltr");
		check(!reader_extract::source().isEmpty() &&
		        reader_extract::source().trimmed().startsWith("(function"),
		      "the extractor is one expression the probe can evaluate");
	}

	section("the document");
	{
		article a;
		a.title = "Fifty %5 off <script>alert(1)</script>";
		a.byline = "Ann & Bo";
		a.site = "The <News>";
		a.html = "<p>The article, kept as the extractor rebuilt it.</p>";
		a.words = 690;
		const QUrl page("https://news.example/2026/story?id=7&x=1");
		const QString light = reader_page::build(a, page, false);
		const QString dark = reader_page::build(a, page, true);

		check(light.contains("Content-Security-Policy") &&
		        light.contains("default-src 'none'") && !light.contains("script-src"),
		      "it forbids every script: no script source is allowed at all");
		check(light.contains("<h1>Fifty %5 off &lt;script&gt;alert(1)&lt;/script&gt;</h1>") &&
		        light.contains("<title>Fifty %5 off &lt;script&gt;"),
		      "the title is escaped, and a %5 in it stays text");
		check(light.contains("Ann &amp; Bo") && light.contains("The &lt;News&gt;"),
		      "byline and site are escaped too");
		check(light.contains("3 min read"), "690 words read as three minutes");
		check(light.contains("<article><p>The article, kept as the extractor rebuilt it.</p>"
		                     "</article>"),
		      "the article goes in as the extractor rebuilt it");
		check(light.contains("href=\"https://news.example/2026/story?id=7&amp;x=1\""),
		      "the site line links back to the page");
		check(light.contains("content=\"light\"") && light.contains("--bg:#fbfbf8") &&
		        dark.contains("content=\"dark\"") && dark.contains("--bg:#1b1b1d"),
		      "light and dark each get their own colours");
		check(light.contains(" dir=\"ltr\"") && !light.contains(" lang="),
		      "no language claimed when the page gave none");

		a.dir = "rtl";
		a.lang = "ar";
		a.byline.clear();
		a.words = 0;
		const QString rtl = reader_page::build(a, page, false);
		check(rtl.contains("<html lang=\"ar\" dir=\"rtl\">"), "a right-to-left page stays one");
		check(!rtl.contains("class=\"byline\""), "and no byline line when there is nothing to say");

		article bare;
		bare.html = "<p>x</p>";
		check(reader_page::build(bare, page, false).contains("<h1>news.example</h1>"),
		      "an article with no title is named after its site");
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
