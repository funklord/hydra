// Do procedural cosmetic rules act on a real page?
//
// `procedural_cosmetic` reads a rule into steps and `test_subscription`
// checks that reading; this checks the other half, the page script in
// `cosmetic_filters::script_source` that runs the steps. One fixture, a rule
// per operator, each with a twin the rule must leave alone -- an element
// hidden is only evidence when its neighbour that does not match is still
// shown. One element is added after load, because what these rules select
// is usually rendered late.
//
// The fixture is shown with `show_html` and the rules are handed over through
// the real bridge, the way `main_window` wires a view.
#include "cosmetic_filters.h"
#include "filter_list.h"
#include "policy_engine.h"
#include "procedural_cosmetic.h"
#include "qtwebengine_factory.h"
#include "request_filter.h"
#include "web_view_backend.h"

#include <QApplication>
#include <QEventLoop>
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

static const char k_page[] = R"HTML(<!doctype html>
<html><body class="no-scroll keep">
<div class="post" id="p1">Sponsored content</div>
<div class="post" id="p2">Real news</div>
<div id="w"><div><span class="lbl">Annons</span></div></div>
<div id="w2"><div><span class="lbl">Annonsera hos oss</span></div></div>
<article id="a1"><h2>Annons here</h2></article>
<article id="a2"><h2>News</h2></article>
<div class="wall" id="wall">Subscribe to read</div>
<div class="m" id="fx" style="position:fixed;top:0">fixed</div>
<div class="m" id="st">static</div>
<div class="n" id="n1">keep me</div>
<div class="n" id="n2">drop me</div>
<div data-ad="1" id="xp">xpath</div>
<div id="xq">not xpath</div>
<div class="ma" id="ma1" data-kind="promo">attr</div>
<div class="ma" id="ma2" data-kind="story">attr</div>
<img class="r" id="img" srcset="a.png 1x" alt="">
<div id="pg1">path</div><div id="pg2">path</div><div id="pg3">path</div>
<h3 class="s">Promoted</h3><div id="after1">after promoted</div>
<h3 class="s">Story</h3><div id="after2">after story</div>
<script>
setTimeout(function () {
  var d = document.createElement('div');
  d.className = 'post'; d.id = 'late'; d.textContent = 'Sponsored, late';
  document.body.appendChild(d);
}, 600);
</script>
</body></html>)HTML";

static const char *const k_rules[] = {
	"div.post:has-text(Sponsored)",
	"span.lbl:has-text(/^Annons$/):upward(2)",
	"article:has(h2:has-text(Annons))",
	"div.wall:remove()",
	"body:remove-class(no-scroll)",
	"div.m:matches-css(position: fixed)",
	"div.n:not(:has-text(keep))",
	":xpath(//div[@data-ad])",
	"div.ma:matches-attr(data-kind=\"promo\")",
	"img.r:remove-attr(srcset)",
	"h3.s:has-text(Promoted) + div",
	":matches-path(/story) div#pg1",
	":matches-path(/other) div#pg2",
	":not(:matches-path(/story)) div#pg3",
};

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

	section("every rule reads");
	generic_cosmetics generic;
	for (const char *sel : k_rules) {
		procedural_cosmetic::rule r;
		QString why;
		const bool ok = procedural_cosmetic::parse(QString::fromLatin1(sel), &r, &why);
		check(ok, QString("%1 %2").arg(QString::fromLatin1(sel), why));
		generic.procedural << procedural_cosmetic::scoped_rule{
			"news.example", QString::fromLatin1(sel), procedural_cosmetic::to_json(r) };
	}

	policy_engine policy;
	request_filter filter(&policy);
	qtwebengine_factory factory(&filter);
	QWidget host;
	host.resize(1000, 800);
	web_view_backend *view = factory.create_view(&host);
	view->widget()->resize(1000, 800);
	host.show();
	view->apply_settings(view_settings());

	filter_list own;
	auto *cosmetic = new cosmetic_filters(&own, &policy, view);
	cosmetic->set_generic(&generic);
	cosmetic->set_page_host("news.example");
	view->set_script_bridge(cosmetic, cosmetic_filters::bridge_name());
	view->inject_script("hydra-cosmetic", cosmetic_filters::script_source());

	bool loaded = false;
	QObject::connect(view, &web_view_backend::load_finished, view, [&](bool) { loaded = true; });
	view->show_html(QString::fromLatin1(k_page), QUrl("https://news.example/story"));
	for (int i = 0; i < 300 && !loaded; ++i)
		spin(50);
	spin(1500);   // past the late element and the 150 ms pass after it

	// One probe, every answer: an id is "hidden", "shown" or "gone".
	QString out;
	bool got = false;
	view->run_probe(QStringLiteral(R"JS(
(function () {
  var ids = ['p1','p2','late','w','w2','a1','a2','wall','fx','st','n1','n2','xp','xq',
             'ma1','ma2','after1','after2','pg1','pg2','pg3'];
  var r = {};
  ids.forEach(function (id) {
    var e = document.getElementById(id);
    r[id] = !e ? 'gone' : getComputedStyle(e).display === 'none' ? 'hidden' : 'shown';
  });
  r.body = document.body.className;
  r.srcset = document.getElementById('img').hasAttribute('srcset') ? 'kept' : 'removed';
  return JSON.stringify(r);
})()
)JS"), [&](const QString &s) { out = s; got = true; });
	for (int i = 0; i < 200 && !got; ++i)
		spin(50);
	std::printf("  page: %s\n", qPrintable(out));

	auto is = [&](const char *id, const char *state) {
		return out.contains(QString("\"%1\":\"%2\"").arg(id, state));
	};
	section("each operator acts on what it names, and not on its twin");
	check(is("p1", "hidden") && is("p2", "shown"), "has-text");
	check(is("late", "hidden"), "has-text on an element added after load");
	check(is("w", "hidden") && is("w2", "shown"), "a regex has-text, then upward(2)");
	check(is("a1", "hidden") && is("a2", "shown"), "has() holding has-text");
	check(is("wall", "gone"), "remove()");
	check(is("fx", "hidden") && is("st", "shown"), "matches-css");
	check(is("n2", "hidden") && is("n1", "shown"), "not() holding has-text");
	check(is("xp", "hidden") && is("xq", "shown"), "xpath on its own");
	check(is("ma1", "hidden") && is("ma2", "shown"), "matches-attr");
	check(is("after1", "hidden") && is("after2", "shown"), "a sibling selector after an operator");
	check(is("pg1", "hidden") && is("pg2", "shown") && is("pg3", "shown"),
	      "a path gate runs the rule on its path only, and its negation the reverse");
	check(out.contains("\"body\":\"keep\""), "remove-class takes one class and leaves the other");
	check(is("srcset", "removed"), "remove-attr");

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
