// What a YouTube watch page can see of this browser's patches, with the
// person's own subscribed lists. Live: it needs a display and the network.
//
//     HYDRA_FILTER_DEBUG=1 xvfb-run -a ./test/build-make/try_youtube_wall [watch url]
//
// **Why this exists**: the holder, 2026-10-25, after uBlock's JSON query
// language was ported: YouTube still says "Ad blockers are not allowed".
// The catalog's own tests prove each scriptlet against stubs and a local
// page; none of them can say what YouTube's player finds when it looks.
// This loads a real watch page through the real window and loader, with a
// copy of the subscribed lists and nothing else of the profile, and reports:
//
//   - what the scriptlet runner said it ran and what threw;
//   - which patched functions give their source away to a page asking;
//   - the request identity YouTube's own config holds, marker included;
//   - whether the player data still carries ads, and whether the wall is up.
//
// **Reported, never asserted.** What YouTube checks is theirs and changes
// on their schedule; a failure here would be news about somebody else's
// product. The output is the evidence for deciding what to change next.
#include "main_window.h"
#include "policy_engine.h"
#include "qtwebengine_factory.h"
#include "request_filter.h"
#include "theme.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QLineEdit>
#include <QMap>
#include <QTemporaryDir>
#include <QTimer>
#include <QWebEnginePage>
#include <QWebEngineView>
#include <cstdio>

static void spin(int ms) {
	QEventLoop l;
	QTimer::singleShot(ms, &l, &QEventLoop::quit);
	l.exec();
}

static QStringList g_reports;
static QtMessageHandler g_prior = nullptr;
static void listen(QtMsgType t, const QMessageLogContext &c, const QString &m) {
	if (m.startsWith(QStringLiteral("scriptlets: ")))
		g_reports << m;
	g_prior(t, c, m);
}

// Every request the filter refused while the page loaded, by address up to
// the query: a request YouTube expects to succeed and sees fail is the
// oldest way a page notices an ad blocker, and nothing on the page side
// can show it.
class blocked_log : public request_observer {
public:
	QMap<QString, int> seen;
	QMap<QString, int> hosts;   // every request, so a zero above means something
	void on_request(const request_context &ctx, const request_decision &d) override {
		hosts[ctx.request_host]++;
		if (!d.block)
			return;
		QUrl u = ctx.url;
		u.setQuery(QString());
		u.setFragment(QString());
		seen[ctx.site_host + "  " + u.toString()]++;
	}
};

// Asked of the page once it has settled. Everything is read defensively:
// a page that throws on a read is a finding, not a crash.
static const char *k_probe = R"JS(
(function () {
	var out = {};
	// Native, or the first line or so of whoever's source it is -- which
	// says which script replaced it.
	var src = function (f) {
		try {
			if (typeof f !== 'function') return typeof f;
			var t = String(f);
			if (t.indexOf('[native code]') >= 0) return 'native';
			return 'SOURCE: ' + t.replace(/\s+/g, ' ').slice(0, 160);
		} catch (e) { return 'throws'; }
	};
	var X = window.XMLHttpRequest && window.XMLHttpRequest.prototype;
	var R = window.Response && window.Response.prototype;
	out.functions = {
		'fetch': src(window.fetch),
		'JSON.parse': src(JSON.parse),
		'JSON.stringify': src(JSON.stringify),
		'XMLHttpRequest.prototype.open': src(X && X.open),
		'XMLHttpRequest.prototype.send': src(X && X.send),
		'Response.prototype.json': src(R && R.json),
		'Response.prototype.text': src(R && R.text),
		'Node.prototype.appendChild': src(Node.prototype.appendChild),
		'setTimeout': src(window.setTimeout),
		'setInterval': src(window.setInterval),
		'addEventListener': src(EventTarget.prototype.addEventListener),
		'Function.prototype.toString': src(Function.prototype.toString),
		'Map.prototype.has': src(Map.prototype.has),
		'Array.prototype.push': src(Array.prototype.push),
		'Promise.prototype.then': src(Promise.prototype.then),
		'MediaSource.prototype.addSourceBuffer':
			src(window.MediaSource && MediaSource.prototype.addSourceBuffer),
		'SourceBuffer.prototype.appendBuffer':
			src(window.SourceBuffer && SourceBuffer.prototype.appendBuffer),
		'Permissions.prototype.query':
			src(window.Permissions && Permissions.prototype.query),
		'MediaDevices.prototype.enumerateDevices':
			src(window.MediaDevices && MediaDevices.prototype.enumerateDevices)
	};
	// Own properties where the engine keeps the method on the prototype.
	try {
		out.own_shims = [
			navigator.permissions && Object.prototype.hasOwnProperty.call(navigator.permissions, 'query') ? 'permissions.query' : '',
			navigator.mediaDevices && Object.prototype.hasOwnProperty.call(navigator.mediaDevices, 'enumerateDevices') ? 'mediaDevices.enumerateDevices' : ''
		].filter(Boolean).join(',') || 'none';
	} catch (e) { out.own_shims = 'throws'; }
	// Anything in the DOM that names this browser.
	out.named_elements = Array.prototype.filter.call(document.querySelectorAll('[id],[class]'),
		function (e) { return /hydra/i.test(e.id + ' ' + e.className); }).length;
	// A buffer of the page's own making, asked what it carries of its own.
	try {
		var ms = new MediaSource();
		out.source_buffer_own = Object.getOwnPropertyNames(ms).join(',') || 'none';
	} catch (e) { out.source_buffer_own = 'throws'; }
	try {
		var d = Object.getOwnPropertyDescriptor(XMLHttpRequest.prototype, 'responseText');
		out.xhr_responseText_getter = d && d.get ? src(d.get) : 'none';
	} catch (e) { out.xhr_responseText_getter = 'throws'; }
	try { out.user_agent = ytcfg.data_.INNERTUBE_CONTEXT.client.userAgent; }
	catch (e) { out.user_agent = 'unreadable'; }
	try {
		var p = window.ytInitialPlayerResponse || {};
		var what = function (v) {
			return v === undefined ? 'undefined' : Array.isArray(v) ? 'array of ' + v.length
			     : typeof v;
		};
		out.initial_ad_placements = what(p.adPlacements);
		out.initial_player_ads = what(p.playerAds);
		out.initial_ad_slots = what(p.adSlots);
		out.playability = p.playabilityStatus ? p.playabilityStatus.status : 'none';
	} catch (e) { out.initial = 'throws'; }
	try {
		var mp = document.getElementById('movie_player');
		var r = mp && mp.getPlayerResponse ? mp.getPlayerResponse() : null;
		out.player_ad_placements = r ? (r.adPlacements === undefined ? 'undefined'
		                                 : 'array of ' + (r.adPlacements || []).length)
		                             : 'no player';
	} catch (e) { out.player_ad_placements = 'throws'; }
	var wall = document.querySelector('ytd-enforcement-message-view-model');
	out.wall_element = wall ? 'present' : 'absent';
	out.wall_text = (document.body && document.body.innerText || '')
		.indexOf('Ad blockers are not allowed') >= 0 ? 'present' : 'absent';
	// Globals a page can list that name this browser or its engine.
	try {
		out.named_globals = Object.getOwnPropertyNames(window).filter(function (k) {
			return /hydra|^qt$|webchannel|__/i.test(k);
		});
		out.qt_object = typeof window.qt;
	} catch (e) { out.named_globals = 'throws'; }
	// Whether quick-fixes' `rpnt` landed: the script it rewrites carries
	// `serverContract` (or opens `if (window.ytcsi`), and once rewritten it
	// also carries the rule's own `YOUTUBE_PREMIUM_LOGO` check.
	try {
		var scripts = Array.prototype.slice.call(document.scripts);
		var has = function (t) {
			return scripts.filter(function (s) {
				return String(s.textContent).indexOf(t) >= 0;
			}).length;
		};
		out.server_contract_scripts = has('serverContract');
		out.ytcsi_scripts = scripts.filter(function (s) {
			return /^if \(window\.ytcsi/.test(String(s.textContent));
		}).length;
		out.rewritten_scripts = has('YOUTUBE_PREMIUM_LOGO');
		out.escaped_commas_left = has('\\,');
		// The rewritten script swaps `Map.prototype.has` for a Proxy, which
		// prints as a nameless native: proof the rewrite ran, not only landed.
		out.map_has_printed = String(Map.prototype.has);
		scripts.forEach(function (s) {
			var t = String(s.textContent);
			if (t.indexOf('serverContract') < 0) return;
			out.contract_has_observer = t.indexOf('MutationObserver') >= 0;
			out.contract_pattern = /(\(function serverContract\(\)|^if \(window\.ytcsi)/m.test(t);
			out.contract_head = t.slice(0, 120);
			out.contract_length = t.length;
		});
	} catch (e) { out.server_contract_scripts = 'throws'; }
	out.url = location.href;
	return JSON.stringify(out);
})()
)JS";

int main(int argc, char *argv[]) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
	qtwebengine_factory::register_resource_scheme();
	QApplication app(argc, argv);
	g_prior = qInstallMessageHandler(listen);
	const QString watch = argc > 1 ? QString::fromLocal8Bit(argv[1])
	                               : QStringLiteral("https://www.youtube.com/watch?v=jNQXAC9IVRw");

	// The person's lists, copied; nothing else of the profile comes along.
	const QString own = QDir::homePath() + "/.local/share/Hydra";
	QTemporaryDir profile;
	if (!profile.isValid())
		return 1;
	const QString dir = profile.path();
	QDir().mkpath(dir + "/filters-subscribed");
	QFile::copy(own + "/filters-subscribed.json", dir + "/filters-subscribed.json");
	for (const QString &f : QDir(own + "/filters-subscribed").entryList(QDir::Files))
		QFile::copy(own + "/filters-subscribed/" + f, dir + "/filters-subscribed/" + f);
	std::printf("lists copied: %lld\n",
	            qlonglong(QDir(dir + "/filters-subscribed").entryList(QDir::Files).size()));
	{
		QFile tf(dir + "/tree.txt");
		tf.open(QIODevice::WriteOnly);
		tf.write("- [p1] unopened | Watch | about:blank\n");
	}

	theme::apply(theme::choice::light);
	policy_engine  policy;
	request_filter filter(&policy);
	blocked_log blocked;
	filter.add_observer(&blocked);
	qtwebengine_factory factory(&filter);
	main_window window(&factory, &policy, &filter);
	window.resize(1280, 800);
	window.load_tree(dir + "/tree.txt");
	window.show();
	spin(1500);

	QLineEdit *bar = nullptr;
	for (QLineEdit *e : window.findChildren<QLineEdit *>())
		if (!e->placeholderText().isEmpty()) { bar = e; break; }
	if (!bar)
		return 1;
	bar->setText(watch);
	QMetaObject::invokeMethod(bar, "returnPressed");

	// Long enough for the player to ask for its data and decide.
	for (int waited = 0; waited < 25000; waited += 500)
		spin(500);

	QWebEngineView *view = nullptr;
	for (QWebEngineView *v : window.findChildren<QWebEngineView *>())
		if (v->isVisible()) view = v;
	if (!view) {
		std::printf("no visible view\n");
		return 1;
	}
	QString said;
	bool done = false;
	view->page()->runJavaScript(QString::fromLatin1(k_probe), [&](const QVariant &r) {
		said = r.toString();
		done = true;
	});
	for (int i = 0; i < 50 && !done; ++i)
		spin(100);

	std::printf("\n== rules with options in force: %d block, %d exception, %d redirect ==\n",
	            filter.network_rule_count(network_rule::action::block),
	            filter.network_rule_count(network_rule::action::allow),
	            filter.network_rule_count(network_rule::action::redirect));
	{
		// What a decision costs with the lists in, over addresses shaped like
		// a page's: the price of every rule above on every request.
		const QStringList urls = {
			"https://www.youtube.com/youtubei/v1/player?key=x",
			"https://i.ytimg.com/vi/abc/hqdefault.jpg",
			"https://fonts.gstatic.com/s/roboto/v30/x.woff2",
			"https://static.doubleclick.net/instream/ad_status.js",
			"https://cdn.example.com/assets/app.bundle.js?v=123",
			"https://www.google-analytics.com/analytics.js",
			"https://images.news.example/2026/10/photo-large.jpg",
			"https://api.example.org/v2/items?page=3&limit=50",
		};
		QElapsedTimer t;
		t.start();
		int blocked = 0;
		for (int i = 0; i < 250; ++i)
			for (const QString &u : urls) {
				request_context ctx;
				ctx.url = QUrl(u);
				ctx.request_host = ctx.url.host();
				ctx.site_host = "www.youtube.com";
				ctx.type = request_type::other;
				blocked += filter.decide(ctx).block ? 1 : 0;
			}
		std::printf("decide: %lld us per request over %lld (%d blocked)\n",
		            qlonglong(t.nsecsElapsed() / 1000 / (250 * urls.size())),
		            qlonglong(250 * urls.size()), blocked);
		// The same with only the plain lists, so the cost above can be told
		// apart from theirs. Taken last: the rules do not come back.
		if (qEnvironmentVariableIsSet("HYDRA_DECIDE_BASELINE")) {
			filter.set_network_rules({});
			t.restart();
			for (int i = 0; i < 250; ++i)
				for (const QString &u : urls) {
					request_context ctx;
					ctx.url = QUrl(u);
					ctx.request_host = ctx.url.host();
					ctx.site_host = "www.youtube.com";
					ctx.type = request_type::other;
					filter.decide(ctx);
				}
			std::printf("decide, plain lists only: %lld us per request\n",
			            qlonglong(t.nsecsElapsed() / 1000 / (250 * urls.size())));
		}
	}
	{
		request_context ctx;
		ctx.url = QUrl("https://static.doubleclick.net/instream/ad_status.js");
		ctx.request_host = ctx.url.host();
		ctx.site_host = "www.youtube.com";
		ctx.kind = resource_kind::script;
		ctx.type = request_type::script;
		const request_decision d = filter.decide(ctx);
		std::printf("ad_status.js on youtube: %s\n",
		            d.block ? (d.redirect.isEmpty() ? "refused"
		                                            : qPrintable("answered with " + d.redirect))
		                    : "allowed");
	}
	std::printf("\n== scriptlet reports (%lld) ==\n", qlonglong(g_reports.size()));
	for (const QString &r : g_reports)
		std::printf("%s\n", qPrintable(r.left(1500)));
	std::printf("\n== requests refused (%lld) ==\n", qlonglong(blocked.seen.size()));
	for (auto it = blocked.seen.cbegin(); it != blocked.seen.cend(); ++it)
		std::printf("%3d  %s\n", it.value(), qPrintable(it.key().left(200)));
	std::printf("\n== every host asked (%lld) ==\n", qlonglong(blocked.hosts.size()));
	for (auto it = blocked.hosts.cbegin(); it != blocked.hosts.cend(); ++it)
		std::printf("%4d  %s\n", it.value(), qPrintable(it.key()));
	std::printf("\n== what the page sees ==\n%s\n",
	            qPrintable(QString::fromUtf8(QJsonDocument::fromJson(said.toUtf8())
	                                           .toJson(QJsonDocument::Indented))));
	window.close();
	spin(300);
	return 0;
}
