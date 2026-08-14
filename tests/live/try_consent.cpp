// The cookie-consent blocker (sec 7.1's `cookie_notices`), driven end to end.
//
// Fixture banners rather than live sites, for the reason sec 11.5 gives about
// extractors: a real CMP changes shape every few weeks, so a test pinned to one
// measures that vendor's current markup and nothing else. These are the
// *shapes* that recur -- a reject button, an accept-only banner, a banner with
// nothing consent-shaped about it, and one that locks scrolling -- and each one
// is here because it is a different decision.
//
// What is asserted is what a user would see: the banner gone, the page usable,
// and the right button clicked. The page reports which button it was by
// fetching /clicked?..., so the claim comes from the page rather than from our
// own bookkeeping about what we intended to click.
#include "main_window.h"
#include "policy_engine.h"
#include "request_filter.h"
#include "qtwebengine_factory.h"
#include "consent_blocker.h"
#include "site_rules.h"
#include "consent_dialog.h"

#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QLineEdit>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QTreeView>
#include <QTreeWidget>
#include <QPushButton>
#include <QUrlQuery>
#include <QWebEnginePage>
#include <QWebEngineView>
#include <cstdio>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void spin(int ms) {
	QEventLoop l;
	QTimer::singleShot(ms, &l, &QEventLoop::quit);
	l.exec();
}

// Each fixture reports the label of whatever was clicked, then removes itself
// the way a real banner does once answered.
static const char *k_common = R"JS(
function say(what) {
  fetch('/clicked?what=' + encodeURIComponent(what));
}
function wire(id) {
  var box = document.getElementById(id);
  Array.prototype.forEach.call(box.querySelectorAll('button'), function (b) {
    b.addEventListener('click', function () {
      say(b.textContent.trim());
      box.remove();
      document.documentElement.style.overflow = 'auto';
      document.body.style.overflow = 'auto';
    });
  });
}
)JS";

struct fixture { const char *path; const char *html; };

// A banner offering a reject, which is the option to take.
static const char *k_reject = R"HTML(
<div id="cc" style="position:fixed;bottom:0;width:100%;height:120px;background:#eee">
  <p>We use cookies and similar technologies. Manage your privacy choices.</p>
  <button>Accept all</button>
  <button>Reject all</button>
  <button>Manage settings</button>
</div>
<script>wire('cc');</script>)HTML";

// Accept is the only way out. Refusing to click it leaves the page unusable,
// which is the thing the option exists to fix.
static const char *k_accept_only = R"HTML(
<div id="cc" style="position:fixed;bottom:0;width:100%;height:120px;background:#eee">
  <p>This site uses cookies to function.</p>
  <button>OK</button>
</div>
<script>wire('cc');</script>)HTML";

// Not a consent banner at all, though it is a fixed bar with buttons and the
// word cookie appears elsewhere on the page. Nothing here may be clicked.
static const char *k_decoy = R"HTML(
<p>Our cookie recipe is famous. Consent to nothing.</p>
<div id="cc" style="position:fixed;bottom:0;width:100%;height:120px;background:#eee">
  <button>Subscribe</button>
  <button>Delete account</button>
</div>
<script>wire('cc');</script>)HTML";

// A banner that locks scrolling, which is how these make a page unreadable
// even when the overlay itself is small.
static const char *k_scrolllock = R"HTML(
<div id="cc" style="position:fixed;inset:0;background:rgba(0,0,0,.6)">
  <p>Cookie consent required to continue.</p>
  <button>Decline</button>
  <button>Accept</button>
</div>
<script>document.documentElement.style.overflow='hidden';
document.body.style.overflow='hidden'; wire('cc');</script>)HTML";

// A real banner whose buttons say nothing the built-in patterns know. This is
// the case the discovery signal exists for, and it is the normal case outside
// English rather than an exotic one.
static const char *k_foreign = R"HTML(
<div id="cc" style="position:fixed;bottom:0;width:100%;height:120px;background:#eee">
  <p>Vi bruker informasjonskapsler (cookies) på dette nettstedet.</p>
  <button>Godta alle</button>
  <button>Avvis alle</button>
</div>
<script>wire('cc');</script>)HTML";

// The same banner, but inside a cross-origin iframe -- which is where a great
// many real consent dialogs live, since that is how a CMP vendor ships one.
static const char *k_framed = R"HTML(
<p>article text</p>
<iframe src="__OTHER__/reject" width="700" height="200"></iframe>)HTML";

class origin : public QTcpServer {
public:
	QHash<QString, QString> clicked;    // path -> label
	QString current;
	quint16 port = 0;

	void incomingConnection(qintptr fd) override {
		auto *s = new QTcpSocket(this);
		s->setSocketDescriptor(fd);
		connect(s, &QTcpSocket::readyRead, this, [this, s] {
			const QByteArray head = s->readAll();
			const QByteArray target = head.mid(4, head.indexOf(' ', 4) - 4);

			QByteArray type = "text/html", body;
			if (target.startsWith("/clicked")) {
				const QUrlQuery q(QUrl(QString::fromUtf8(target)).query());
				clicked.insert(current, q.queryItemValue("what"));
				type = "text/plain";
				body = "ok";
			} else {
				const char *inner = k_accept_only;
				if (target.startsWith("/reject"))          inner = k_reject;
				else if (target.startsWith("/decoy"))      inner = k_decoy;
				else if (target.startsWith("/scrolllock")) inner = k_scrolllock;
				else if (target.startsWith("/foreign"))    inner = k_foreign;
				else if (target.startsWith("/framed"))     inner = k_framed;
				QByteArray page_inner(inner);
				page_inner.replace("__OTHER__",
				                    "http://127.0.0.2:" + QByteArray::number(port));
				body = QByteArray("<!doctype html><html><body><h1>page</h1>"
				                   "<script>") + k_common + "</script>" + page_inner +
				        "</body></html>";
			}

			QByteArray resp = "HTTP/1.1 200 OK\r\nContent-Type: " + type +
			                   "\r\nCache-Control: no-store\r\nContent-Length: " +
			                   QByteArray::number(body.size()) +
			                   "\r\nConnection: close\r\n\r\n" + body;
			s->write(resp);
			s->flush();
			s->disconnectFromHost();
		});
	}
};

int main(int argc, char *argv[]) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
	QApplication app(argc, argv);

	const QString out = qEnvironmentVariableIsSet("HYDRA_TEST_OUT")
	                        ? qgetenv("HYDRA_TEST_OUT") : QString("/tmp/hydra-test");
	QDir().mkpath(out);
	QDir(out + "/state").removeRecursively();
	QFile::remove(out + "/policy.ini");
	QFile::remove(out + "/policy.json");   // and the file it migrates from
	// The shell loads its rules from this directory, and the round-trip phase
	// below writes a rule file. Left in place, a run starts already knowing what
	// the previous run learned -- and the "a banner nothing matches" case quietly
	// stops testing anything. Same shape as the tree and state contamination
	// `try_extract` had: an artefact of the last run, indistinguishable from a
	// real result.
	QFile::remove(out + "/site-rules.ini");
	QFile::remove(out + "/site-rules.ini");
	const QString tree = out + "/tree.txt";
	QFile tf(tree);
	if (!tf.open(QIODevice::WriteOnly | QIODevice::Truncate)) return 1;
	tf.write("- [f0] folder | Capture\n"
	          "  - [a1] unopened | Blank | about:blank | "
	          "created=2026-01-01T00:00:00 | seen=2026-01-01T00:00:00\n"
	          // A second tab, for the tab-switching check at the end. It is
	          // loaded once and then left alone, which is exactly the state that
	          // used to go unnoticed.
	          "  - [a2] unopened | Second | about:blank | "
	          "created=2026-01-01T00:00:00 | seen=2026-01-01T00:00:00\n");
	tf.close();

	// Bound on all loopback addresses so the same server can be the page's host
	// and a *different* host for the iframe, which is what makes the framed case
	// genuinely cross-origin rather than a same-origin one wearing a costume.
	origin server;
	if (!server.listen(QHostAddress::AnyIPv4, 0)) return 1;
	const quint16 port = server.serverPort();
	server.port = port;

	policy_engine       policy;
	request_filter      filter(&policy);
	qtwebengine_factory factory(&filter);
	main_window w(&factory, &policy, &filter);
	w.load_tree(tree);
	w.resize(1000, 700);
	w.show();
	spin(1500);

	auto *blocker = w.findChild<consent_blocker *>();
	check(blocker != nullptr, "the shell has a consent blocker");
	if (!blocker) return 1;

	auto *tree_view = w.findChild<QTreeView *>();
	emit tree_view->activated(
	  tree_view->model()->index(0, 0, tree_view->model()->index(0, 0)));
	spin(1500);

	QLineEdit *bar = nullptr;
	for (QLineEdit *e : w.findChildren<QLineEdit *>())
		if (e->placeholderText() == "Address") bar = e;
	if (!bar) { std::printf("NO ADDRESS BAR\n"); return 1; }

	int visit = 0;
	// Wait for the click rather than a fixed window: the script watches for a
	// banner that may arrive late, and a fixed wait is the instrument that
	// invented a mechanism once already in this project.
	auto load = [&](const char *path, int max_ms = 12000) {
		server.current = QString::fromUtf8(path);
		server.clicked.remove(server.current);
		bar->setText(QString("http://127.0.0.1:%1%2?v=%3")
		                 .arg(port).arg(path).arg(++visit));
		QMetaObject::invokeMethod(bar, "returnPressed");
		for (int waited = 0; waited < max_ms &&
		                      !server.clicked.contains(server.current); waited += 250)
			spin(250);
		spin(600);   // let the hide pass and the bridge call land
	};
	auto clicked_on = [&](const char *path) {
		return server.clicked.value(QString::fromUtf8(path));
	};
	// Whether the page ended up usable, asked of the page itself -- the overlay
	// gone and scrolling not left locked. Nothing else can answer that honestly:
	// our own record says what we meant to do, not what the document looks like
	// afterwards.
	auto page_usable = [&]() {
		const auto views = w.findChildren<QWebEngineView *>();
		if (views.isEmpty()) return false;
		bool out = false;
		QEventLoop loop;
		static const char *js =
		  "(function(){var b=document.getElementById('cc');"
		  "var lock=getComputedStyle(document.documentElement).overflow==='hidden'"
		  "||getComputedStyle(document.body).overflow==='hidden';"
		  "return (!b||getComputedStyle(b).display==='none')&&!lock;})()";
		views.last()->page()->runJavaScript(QString::fromLatin1(js),
		                                     [&](const QVariant &v) {
			out = v.toBool();
			loop.quit();
		});
		QTimer::singleShot(3000, &loop, &QEventLoop::quit);
		loop.exec();
		return out;
	};

	std::printf("\n== a banner offering reject ==\n");
	load("/reject");
	std::printf("  page clicked: %s\n", qPrintable(clicked_on("/reject")));
	check(clicked_on("/reject") == "Reject all",
	      "the least permissive option is taken, not the first one in the DOM");
	check(page_usable(), "and the banner is gone afterwards");

	std::printf("\n== a banner whose only way out is accept ==\n");
	load("/accept");
	std::printf("  page clicked: %s\n", qPrintable(clicked_on("/accept")));
	check(clicked_on("/accept") == "OK",
	      "accept is taken rather than leaving the page unusable");

	std::printf("\n== a fixed bar that is not a consent banner ==\n");
	load("/decoy", 6000);
	std::printf("  page clicked: %s\n",
	             clicked_on("/decoy").isEmpty() ? "(nothing)"
	                                             : qPrintable(clicked_on("/decoy")));
	check(clicked_on("/decoy").isEmpty(),
	      "nothing is clicked on a bar that only mentions cookies in passing");

	std::printf("\n== a banner that locks scrolling ==\n");
	load("/scrolllock");
	std::printf("  page clicked: %s\n", qPrintable(clicked_on("/scrolllock")));
	check(clicked_on("/scrolllock") == "Decline",
	      "the reject-shaped option is taken there too");
	check(page_usable(),
	      "and the scroll lock it left behind is released, which is the part a "
	      "banner that merely hides would not fix");

	// The policy side, and it has to start from cookies actually blocked or the
	// relaxation has nothing to do and the check passes for the wrong reason --
	// which it did, the first time this was written.
	std::printf("\n== what it does to policy when cookies are blocked ==\n");
	policy.set_setting("127.0.0.1", policy::feature::cookies,
	                    policy::setting::block);
	check(!policy.is_allowed(policy::feature::cookies, "127.0.0.1"),
	      "cookies start blocked for the host");
	load("/reject");
	const bool first = policy.is_allowed(policy::feature::cookies, "127.0.0.1");
	const bool third =
	  policy.is_allowed(policy::feature::third_party_cookies, "127.0.0.1");
	std::printf("  cookies=%d thirdParty=%d\n", int(first), int(third));
	check(first, "answering the banner allows first-party cookies, so the choice "
	              "can be recorded and the banner does not return next load");
	check(!third, "and third-party cookies are still blocked — that is what the "
	               "dialog was bargaining for");
	check(policy.setting_for("127.0.0.1", policy::feature::cookies) ==
	          policy::setting::allow,
	      "written as an ordinary per-site rule, so the shield shows it and can "
	      "undo it");

	// And the option is an option: set to allow, the banner is left alone.
	policy.set_setting("127.0.0.1", policy::feature::cookie_notices,
	                    policy::setting::allow);
	std::printf("\n== with the option turned off for this site ==\n");
	load("/reject", 6000);
	std::printf("  page clicked: %s\n",
	             clicked_on("/reject").isEmpty() ? "(nothing)"
	                                              : qPrintable(clicked_on("/reject")));
	check(clicked_on("/reject").isEmpty(),
	      "the banner is left for the user to answer");

	// A banner it cannot answer is the discovery signal, not a silent failure.
	policy.set_setting("127.0.0.1", policy::feature::cookie_notices,
	                    policy::setting::unset);
	std::printf("\n== a banner in a language the built-ins do not cover ==\n");
	load("/foreign", 8000);
	check(clicked_on("/foreign").isEmpty(),
	      "nothing is clicked, because nothing matched");
	const QStringList seen = blocker->unhandled();
	std::printf("  unhandled: %s\n",
	             seen.isEmpty() ? "(none)" : qPrintable(seen.first()));
	check(!seen.isEmpty(), "and the banner is recorded as unanswerable rather "
	                        "than silently skipped");
	check(!seen.isEmpty() && seen.first().contains("Avvis alle"),
	      "with the labels it offered, which is most of the rule already");

	// Turning one into a rule, and what scope it gets.
	const site_rule learned = blocker->rule_from_label("Avvis alle", "reject");
	check(learned.kind == "reject", "a label becomes a rule of the right kind");
	check(learned.generic(),
	      "with no host — a button label describes a shape, not a site");
	site_rules store = site_rules::defaults();
	store.add(learned);
	check(store.promotable().size() == 1,
	      "so it is flagged for the built-in defaults at the next release");
	check(store.promotable().first().value.contains("Avvis"),
	      "and the flagged rule is the one just learned");

	// The rule file is the unit that would eventually be shared, so it has to
	// survive a round trip carrying that flag.
	// Deliberately not the name the shell loads: this is a round-trip check, not
	// a rule set for the next run to inherit.
	const QString rules_path = out + "/round-trip-rules.json";
	check(store.save(rules_path), "the rule set saves");
	site_rules reloaded;
	check(reloaded.load(rules_path), "and loads again");
	check(reloaded.promotable().size() == 1,
	      "with the promote flag surviving the round trip, which is what makes "
	      "it findable by whoever folds it into the binary");
	check(reloaded.all().size() > reloaded.promotable().size(),
	      "and the built-ins present but not written into the file");

	// An escaped label: a banner reading like a regex must not become one.
	const site_rule hostile = blocker->rule_from_label("(.*)", "accept");
	check(hostile.value.contains("\\("),
	      "a label that looks like a regex is escaped, not compiled");

	// The review surface, driven rather than admired. A dialog that is correct
	// and never clicked is this project's most common defect.
	std::printf("\n== teaching a rule through the dialog ==\n");
	{
		const QString path = out + "/dialog-rules.json";
		QFile::remove(path);
		consent_dialog dlg(blocker, path);
		dlg.show();
		spin(400);

		auto *list = dlg.findChild<QTreeWidget *>("banners");
		check(list && list->topLevelItemCount() >= 1,
		      "the unanswered banner is listed");
		QPushButton *refuses = nullptr, *accepts = nullptr;
		for (QPushButton *b : dlg.findChildren<QPushButton *>()) {
			if (b->text().contains("refuses")) refuses = b;
			if (b->text().contains("accepts")) accepts = b;
		}
		check(refuses && !refuses->isEnabled(),
		      "and nothing can be accepted until a button is chosen");

		// The site row is not a rule; only a button is.
		if (list && list->topLevelItemCount()) {
			list->setCurrentItem(list->topLevelItem(0));
			spin(150);
			check(refuses && !refuses->isEnabled(),
			      "selecting the site row alone still offers nothing");

			QTreeWidgetItem *top = list->topLevelItem(0);
			QTreeWidgetItem *kid = nullptr;
			for (int i = 0; i < top->childCount(); ++i)
				if (top->child(i)->text(0) == "Avvis alle") kid = top->child(i);
			check(kid != nullptr, "the labels the banner offered are the choices");
			if (kid) {
				list->setCurrentItem(kid);
				spin(150);
				check(refuses && refuses->isEnabled(),
				      "choosing one enables it");
				refuses->click();
				spin(300);
			}
		}

		// A label too generic to apply everywhere is refused, with a reason,
		// rather than quietly learned. The rule would carry no host.
		{
			const site_rule risky = blocker->rule_from_label("Yes", "accept");
			check(!site_rules::why_unsafe(risky).isEmpty(),
			      "a label like \"Yes\" is refused as a site-wide rule");
			const site_rule fine = blocker->rule_from_label("Avvis alle", "reject");
			check(site_rules::why_unsafe(fine).isEmpty(),
			      "while a banner-specific one is fine");
		}

		const QList<site_rule> flagged = blocker->rules().promotable();
		check(flagged.size() == 1,
		      "accepting adds exactly one rule, flagged for the built-ins");
		check(!flagged.isEmpty() && flagged.first().kind == "reject",
		      "of the kind the button was said to be");
		site_rules from_disk;
		check(from_disk.load(path) && from_disk.promotable().size() == 1,
		      "and it is on disk, so it survives the session");
		Q_UNUSED(accepts)
	}

	// And the point of fetching rules per page rather than baking them in: the
	// rule just learned applies to the next load, with no new view built.
	std::printf("\n== the learned rule takes effect ==\n");
	load("/foreign");
	std::printf("  page clicked: %s\n",
	             clicked_on("/foreign").isEmpty() ? "(nothing)"
	                                              : qPrintable(clicked_on("/foreign")));
	check(clicked_on("/foreign") == "Avvis alle",
	      "the banner that could not be answered a moment ago now is");

	std::printf("\n== a banner inside a cross-origin iframe ==\n");
	load("/framed", 10000);
	std::printf("  page clicked: %s\n",
	             clicked_on("/framed").isEmpty() ? "(nothing)"
	                                              : qPrintable(clicked_on("/framed")));
	check(clicked_on("/framed") == "Reject all",
	      "a CMP shipped as an iframe is answered like any other banner");

	// Does the consent bridge follow the *visible* tab?
	//
	// It did not, and nothing here noticed. The page host was set in one place --
	// the current view's url_changed -- so switching to an already-loaded tab
	// navigated nothing, sent no signal, and left the blocker answering
	// active_now() and rules_json() for the site of the tab before it. A bridge
	// deliberately built so a page cannot name its own host is worth little if
	// the shell then names the wrong one.
	//
	// Found on Android, where the same gap was total rather than partial: that
	// backend emits url_changed from inside load(), before the stack switches, so
	// the host was never set even once.
	std::printf("\n== the page context follows the tab, not the last navigation ==\n");
	{
		auto *model = tree_view->model();
		const QModelIndex folder = model->index(0, 0);
		const QModelIndex first  = model->index(0, 0, folder);
		const QModelIndex second = model->index(1, 0, folder);
		check(second.isValid(), "the fixture has a second tab to switch to");

		if (second.isValid()) {
			// Give each tab a page on a host of its own. Two loopback addresses,
			// as everywhere else here, so the hosts differ by name.
			emit tree_view->activated(first);
			spin(800);
			bar->setText(QString("http://127.0.0.1:%1/plain").arg(port));
			QMetaObject::invokeMethod(bar, "returnPressed");
			spin(2500);
			const QString host_one = blocker->page_host();

			emit tree_view->activated(second);
			spin(800);
			bar->setText(QString("http://127.0.0.2:%1/plain").arg(port));
			QMetaObject::invokeMethod(bar, "returnPressed");
			spin(2500);
			check(blocker->page_host() == "127.0.0.2",
			      QString("the second tab's host is current while it is (%1)")
			          .arg(blocker->page_host()));

			// The whole point: back to the first tab, which navigates nothing.
			emit tree_view->activated(first);
			spin(2000);
			check(blocker->page_host() == host_one,
			      QString("switching back restores the first tab's host without a "
			               "navigation (%1, wanted %2)")
			          .arg(blocker->page_host(), host_one));
			check(blocker->page_host() != "127.0.0.2",
			      "and does not leave the other tab's site in force");
		}
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
