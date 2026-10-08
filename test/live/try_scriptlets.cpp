// Do the scriptlets a subscribed list carries actually prune a page?
//
// **The question the catalog could not answer.** `test_scriptlets` runs the
// generated source in QJSEngine against stubbed pages, 290 checks of it, and
// `try_blob_cost` aside nothing has ever put a scriptlet into a real engine
// through the real subscription path. That is the shape this tree calls a
// correct function and no working feature: the catalog is proven, the
// pipeline from a fetched list to a pruned page is not.
//
// This drives it end to end -- an index on disk, a cached body with a rule in
// it, the shell's own loader, the real injector, Qt WebEngine -- and asks the
// page what it got.
//
// **No DNS and no YouTube**, which is the same trick `try_filters` uses for
// the network half and the reason this is deterministic. A scriptlet is scoped
// by hostname, and `127.0.0.1` is a hostname like any other: a rule naming it
// is injected into a page served from it. What is being tested is ours --
// does a rule from a subscribed list reach a real page and change what the
// page sees -- and asserting against YouTube's current internals would test
// somebody else's product on somebody else's schedule.
//
// So the YouTube half is REPORTED rather than asserted, from the real list if
// one is at hand:
//
//   QT_QPA_PLATFORM=offscreen ./test/build-make/try_scriptlets
//   HYDRA_UBO_LIST=~/filters.txt QT_QPA_PLATFORM=offscreen ./try_scriptlets
//
// The page reports by fetching a url the server records, which is
// `try_filters`' channel and needs no javascript seam in the shell -- the
// shell deliberately has none, pages being spoken to through injected scripts
// and bridges rather than evaluated into.
#include "main_window.h"
#include "node.h"
#include "policy_engine.h"
#include "qtwebengine_factory.h"
#include "request_filter.h"
#include "scriptlets.h"
#include "settings_dialog.h"
#include "tab_tree_model.h"
#include "theme.h"
#include "tree_sort_proxy.h"

#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QLineEdit>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeView>
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

// The site, the player endpoint it fetches, and the channel it answers on.
class origin : public QTcpServer {
public:
	quint16     port = 0;
	QStringList asked;

	void incomingConnection(qintptr fd) override {
		auto *s = new QTcpSocket(this);
		s->setSocketDescriptor(fd);
		connect(s, &QTcpSocket::readyRead, this, [this, s] {
			const QByteArray head = s->readAll();
			const int sp = head.indexOf(' ', 4);
			const QByteArray target = sp > 4 ? head.mid(4, sp - 4) : QByteArray();
			asked << QString::fromUtf8(target);

			QByteArray body, type = "text/html";
			if (target.startsWith("/player")) {
				// The shape a player response has: the ad slots beside the
				// thing the page actually needs, so a prune that took the
				// whole body would be indistinguishable from one that worked.
				type = "application/json";
				body = "{\"adPlacements\":[{\"slot\":1},{\"slot\":2}],"
				        "\"streamingData\":{\"ok\":1}}";
			} else if (target.startsWith("/report")) {
				type = "image/gif";
				body = QByteArray::fromHex("4749463839610100010080000000000000"
				                            "ffffff21f90401000000002c0000000001"
				                            "0001000002024401003b");
			} else {
				// **Reports both halves.** `ads` says whether the ad slots
				// survived; `kept` says whether the body the page needs did.
				// One without the other cannot tell a working prune from a
				// scriptlet that threw the response away.
				body = "<!doctype html><html><body>player"
				        "<script>"
				        "fetch('/player').then(function (r) { return r.json(); })"
				        ".then(function (d) {"
				        "  var ads = (d && d.adPlacements) ? 'present' : 'gone';"
				        "  var kept = (d && d.streamingData && d.streamingData.ok)"
				        "             ? 'yes' : 'no';"
				        "  new Image().src = '/report?ads=' + ads + '&kept=' + kept;"
				        "}).catch(function () {"
				        "  new Image().src = '/report?ads=error&kept=error';"
				        "});"
				        "</script></body></html>";
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

	// What the page said, or empty if it has not said yet.
	QString report() const {
		for (const QString &a : asked)
			if (a.startsWith("/report?"))
				return a;
		return QString();
	}
	bool wait_for_report(int ms = 15000) {
		for (int waited = 0; waited < ms; waited += 200) {
			spin(200);
			if (!report().isEmpty())
				return true;
		}
		return false;
	}
};

int main(int argc, char *argv[]) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
	QApplication app(argc, argv);

	origin server;
	if (!server.listen(QHostAddress::AnyIPv4, 0)) {
		std::printf("could not listen\n");
		return 1;
	}
	server.port = server.serverPort();

	QTemporaryDir profile;
	if (!profile.isValid()) {
		std::printf("no scratch directory\n");
		return 1;
	}
	const QString tree = QDir(profile.path()).filePath("tree.txt");
	const QString subs_dir = QDir(profile.path()).filePath("filters-subscribed");
	const QString subs_index =
	  QDir(profile.path()).filePath("filters-subscribed.json");

	// **A subscription the way one arrives, not a scriptlet handed to the
	// injector.** The whole point is the path: a body on disk, an index naming
	// it, the shell's own `read()` classifying its lines, and
	// `inject_scriptlets` deciding what a view gets.
	QDir().mkpath(subs_dir);
	{
		QFile body(QDir(subs_dir).filePath("local.txt"));
		if (body.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
			body.write("! Title: a local list\n");
			// The rule under test, scoped to the host the page is served from.
			body.write("127.0.0.1##+js(json-prune, adPlacements)\n");
			// A network rule as well, so the list is not scriptlets alone and
			// `read()` is exercised on a mixed body.
			body.write("||never-requested.invalid^\n");
		}
	}
	{
		QFile idx(subs_index);
		if (idx.open(QIODevice::WriteOnly | QIODevice::Truncate))
			idx.write("[\n    {\n"
			           "        \"name\": \"a local list\",\n"
			           "        \"url\": \"https://local.invalid/list.txt\",\n"
			           "        \"enabled\": true,\n"
			           "        \"trusted\": false,\n"
			           "        \"file\": \"local.txt\"\n"
			           "    }\n]\n");
	}
	{
		QFile tf(tree);
		if (tf.open(QIODevice::WriteOnly | QIODevice::Truncate))
			tf.write("- [p1] unopened | Player | about:blank\n");
	}

	theme::apply(theme::choice::light);
	policy_engine  policy;
	request_filter filter(&policy);
	qtwebengine_factory factory(&filter);
	main_window window(&factory, &policy, &filter);
	window.resize(900, 650);
	window.load_tree(tree);
	window.show();
	spin(800);

	QLineEdit *bar = nullptr;
	for (QLineEdit *e : window.findChildren<QLineEdit *>())
		if (!e->placeholderText().isEmpty()) { bar = e; break; }
	if (!bar) {
		std::printf("no address bar\n");
		return 1;
	}

	const QString page = QString("http://127.0.0.1:%1/page").arg(server.port);

	section("a subscribed scriptlet reaches a real page");
	{
		// The catalog read the rule at all, which is the cheapest thing to get
		// wrong and the one that makes every assertion below vacuous.
		check(scriptlets::vetted("json-prune"),
		       "json-prune is in the catalog this build ships");

		server.asked.clear();
		bar->setText(page);
		QMetaObject::invokeMethod(bar, "returnPressed");
		const bool answered = server.wait_for_report();
		check(answered, QString("the page reported back (%1)")
		                     .arg(server.report()));
		check(server.report().contains("ads=gone"),
		       QString("and the ad slots were pruned out of its player "
		                "response (%1)").arg(server.report()));
		// **The half that separates a prune from a wrecked response.** A
		// scriptlet that replaced the body, or threw, would also report the
		// ads gone.
		check(server.report().contains("kept=yes"),
		       QString("while the part the page needs survived (%1)")
		           .arg(server.report()));
	}

	section("and the ads setting takes it off the tab in front of you");
	{
		// **The half that was broken when this driver was written.** The other
		// two halves of the ads switch answer per request and per page, so
		// they stop the moment the setting changes. A scriptlet is injected at
		// DocumentCreation out of a collection fixed when the view was made,
		// and nothing re-read it when a rule changed -- so an existing tab
		// went on being patched until it was recreated, and the switch looked
		// like it worked on a new tab and not on the one in front of you.
		//
		// Delete the `policy_engine::changed` connection in `main_window` and
		// this check goes red while the one below stays green, which is what
		// makes the pair worth having rather than either alone.
		policy.set_setting("127.0.0.1", policy::feature::ads,
		                    policy::setting::allow);
		spin(300);
		server.asked.clear();
		bar->setText(page + "-again");
		QMetaObject::invokeMethod(bar, "returnPressed");
		const bool answered = server.wait_for_report();
		check(answered, QString("the original tab reported back (%1)")
		                     .arg(server.report()));
		check(server.report().contains("ads=present"),
		       QString("and it stopped pruning without being recreated (%1)")
		           .arg(server.report()));
		// **What this check cannot see on its own**, found by sabotaging the
		// other end: drop a subscribed list's scriptlets entirely and it still
		// passes, because "no longer pruning" is trivially true of a page that
		// was never patched. It means something only while the first section
		// passes, which is why the two are in one driver and not two.
	}

	section("and never installs it on a tab made while the switch is off");
	{
		// **The gate where it is read.** `inject_scriptlets` filters
		// `m_scriptlets` by the `ads` setting for each call's own scope and
		// hands the survivors to the view, so a tab created while the switch
		// is off should never receive the patch at all -- a different claim
		// from the one above, which is about a tab that already had it.
		window.new_tab();
		spin(300);
		server.asked.clear();
		bar->setText(page + "-fresh");
		QMetaObject::invokeMethod(bar, "returnPressed");
		const bool answered = server.wait_for_report();
		check(answered, QString("the fresh tab reported back (%1)")
		                     .arg(server.report()));
		check(server.report().contains("ads=present"),
		       QString("and it was never given the scriptlet (%1)")
		           .arg(server.report()));
		policy.set_setting("127.0.0.1", policy::feature::ads,
		                    policy::setting::ask);
	}

	// --- what the real list says, reported rather than asserted -----------
	section("what uBlock's own list asks for on youtube.com");
	if (qEnvironmentVariableIsSet("HYDRA_UBO_LIST")) {
		const QString path = qgetenv("HYDRA_UBO_LIST");
		QFile f(path);
		if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
			std::printf("  %s could not be read\n", qPrintable(path));
		} else {
			int lines = 0, kept = 0, needs_trust = 0;
			const QStringList all =
			  QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'));
			for (const QString &line : all) {
				if (!line.contains(QStringLiteral("youtube")) ||
				    !line.contains(QStringLiteral("##+js(")))
					continue;
				++lines;
				QString inside =
				  line.mid(line.indexOf(QStringLiteral("##+js(")) + 6).trimmed();
				if (inside.endsWith(QLatin1Char(')')))
					inside.chop(1);
				scriptlet_call c;
				if (!scriptlets::parse_call(inside, &c, nullptr)) {
					std::printf("        refused: %s\n", qPrintable(inside));
					continue;
				}
				++kept;
				if (scriptlets::requires_trust(c.name))
					++needs_trust;
			}
			std::printf("        %d youtube scriptlet rule(s): %d this build "
			             "runs, %d of those need the list trusted\n",
			             lines, kept, needs_trust);
			std::printf("\n  Reported, not asserted: which rules YouTube needs "
			             "is uBlock's\n  business and changes on their "
			             "schedule, so a failure here would\n  be news about "
			             "somebody else's product. What this driver asserts is "
			             "ours.\n");
		}
	} else {
		std::printf("        set HYDRA_UBO_LIST=<filters.txt> to have this "
		             "read the real list\n");
	}

	window.close();
	spin(200);
	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
