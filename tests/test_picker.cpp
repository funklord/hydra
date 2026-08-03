// What "zap this" hands the shell (architecture doc §12.1).
//
// The C++ half is thin on purpose — the page script does the picking — which
// means everything arriving here is a string a page composed, and the only
// defences are in this file. It had no test.
//
// The property that matters most is not parsing. It is that a page cannot say
// *which site* it is: the picked element becomes a filter-rule proposal scoped
// to a domain, so a page able to set that field could propose a rule against
// somebody else's site and have the user review it under the wrong name.
#include "element_picker.h"

#include <QCoreApplication>
#include <QSignalSpy>
#include <cstdio>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void section(const char *n) { std::printf("\n== %s ==\n", n); }

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);

	section("an ordinary pick");
	{
		element_picker p;
		QSignalSpy picked(&p, &element_picker::picked);
		QSignalSpy aborted(&p, &element_picker::aborted);
		QSignalSpy asked(&p, &element_picker::pick_requested);

		check(!p.active(), "a fresh picker is not picking");
		p.begin("https://news.example/story");
		check(p.active(), "begin() starts a pick");
		check(asked.count() == 1, "and asks the page to enter pick mode");

		p.element_picked(R"({"tag":"DIV","id":"promo",
		                      "classes":["ad","banner",""],
		                      "selector":"div#promo",
		                      "snippet":"<div id=\"promo\"></div>"})");
		check(picked.count() == 1, "a valid element is reported once");
		check(!p.active(), "and the pick is over");

		const picked_element &e = p.last();
		check(e.tag == "div", QString("the tag is lowercased (%1)").arg(e.tag));
		check(e.id == "promo", "the id comes through");
		check(e.selector == "div#promo", "and the selector the script derived");
		check(e.classes == QStringList({"ad", "banner"}),
		      QString("classes come through with empties dropped (%1)")
		          .arg(e.classes.join(",")));
		check(e.page_url == "https://news.example/story",
		      "and the page url is the one the shell set");
	}

	section("the page does not get to say which site it is");
	{
		// The shell sets the url from the view it is showing. A page that could
		// override it would have its proposal reviewed under another site's name
		// — and a cosmetic rule accepted under the wrong scope applies where
		// nobody chose it.
		element_picker p;
		p.begin("https://real.example/page");
		p.element_picked(R"({"tag":"div","page_url":"https://evil.example/",
		                      "selector":"div"})");
		check(p.last().page_url == "https://real.example/page",
		      QString("a page_url in the payload is ignored (%1)")
		          .arg(p.last().page_url));
	}

	section("what a broken or hostile payload gets");
	{
		element_picker p;
		QSignalSpy picked(&p, &element_picker::picked);
		QSignalSpy aborted(&p, &element_picker::aborted);

		p.begin("https://x.example/");
		p.element_picked("this is not json");
		check(aborted.count() == 1, "junk aborts rather than throwing");
		check(picked.count() == 0, "and reports no pick");
		check(!p.active(), "and the pick is over either way");

		p.begin("https://x.example/");
		p.element_picked(R"({"id":"promo","selector":"div#promo"})");
		check(aborted.count() == 2,
		      "an element with no tag is not an element, whatever else it carries");

		p.begin("https://x.example/");
		p.element_picked("[]");
		check(aborted.count() == 3, "a JSON array is not an element either");

		p.begin("https://x.example/");
		p.element_picked("");
		check(aborted.count() == 4, "and neither is nothing");
		check(picked.count() == 0, "none of which produced a pick");
	}

	section("a picker that starts again forgets the last one");
	{
		element_picker p;
		p.begin("https://a.example/");
		p.element_picked(R"({"tag":"div","selector":"div"})");
		check(p.last().is_valid(), "there is a pick");
		p.begin("https://b.example/");
		check(!p.last().is_valid(),
		      "and starting a new pick clears it, so a cancelled pick cannot be "
		      "mistaken for the previous element");
		check(p.active(), "while the new pick is running");
	}

	section("the snippet is capped");
	{
		element_picker p;
		p.begin("https://x.example/");
		const QString huge = QString("<div>").repeated(2000);
		p.element_picked(QString(R"({"tag":"div","selector":"div","snippet":"%1"})")
		                     .arg(huge));
		check(p.last().snippet.size() <= 1200,
		      QString("a page cannot make the payload as large as it likes (%1)")
		          .arg(p.last().snippet.size()));
	}

	section("cancelling");
	{
		element_picker p;
		QSignalSpy aborted(&p, &element_picker::aborted);
		p.begin("https://x.example/");
		p.cancelled();
		check(!p.active(), "escape ends the pick");
		check(aborted.count() == 1, "and says so once");
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
