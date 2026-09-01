// The feature-permission callbacks: geolocation, camera, microphone and
// notifications. Wired since step 3.5 and never once exercised, which in this
// project is the same sentence as "probably broken".
//
// Driven end to end and observed from the *page's* side. The page asks for each
// permission and reports what it got by fetching `/report?...`, so what is
// measured is what a site would actually experience -- not what our decider
// returned, which is the thing that could be right while the wiring under it is
// not. `main_window` installs the decider, the backend maps Qt's feature enum
// onto ours, and the policy engine answers; a break anywhere in that chain shows
// up here as the wrong answer reaching the page.
//
// Served from `127.0.0.1`, which Chromium counts as a secure context -- without
// that, geolocation and getUserMedia are refused before any permission is asked
// for and the whole run would measure nothing.
//
// **Reading the results needs one distinction.** "Denied" and "granted but
// unavailable" are different outcomes and this machine produces both: with
// geolocation *granted*, Chromium has no location provider configured and
// answers POSITION_UNAVAILABLE (code 2), while a refusal is PERMISSION_DENIED
// (code 1). Likewise getUserMedia says `NotAllowedError` when refused and
// `NotFoundError` when allowed with no device present. So the codes are what is
// checked, never "did it fail".
#include "main_window.h"
#include "policy_engine.h"
#include <QToolBar>
#include "annoyance_log.h"
#include "request_filter.h"
#include "qtwebengine_factory.h"

#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QLineEdit>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QDialog>
#include <QPushButton>
#include <QTreeView>
#include <QUrlQuery>
#include <cstdio>

// The decider's own answers, captured in process. The page-visible outcome is
// not always enough: geolocation comes back PERMISSION_DENIED on this build
// even when we grant it, because the engine has no location provider and says
// so in the only vocabulary the API gives it. What we control is what was asked
// and what we answered, so that is what is asserted.
static QStringList g_perm_log;
static void collect(QtMsgType, const QMessageLogContext &, const QString &m) {
	if (m.startsWith("permission:")) g_perm_log << m;
	else std::fprintf(stderr, "%s\n", qPrintable(m));
}

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
// Not a pass and not a failure. Counted as neither, so a run that could not
// exercise something says so rather than quietly reporting a smaller number of
// passes -- which reads as everything being fine.
static void skipped(const QString &w) {
	std::printf("  --    skipped: %s\n", qPrintable(w));
}
static void spin(int ms) {
	QEventLoop l;
	QTimer::singleShot(ms, &l, &QEventLoop::quit);
	l.exec();
}

static const char *k_page = R"HTML(<!doctype html><html><body>
<script>
function say(what, result) {
  fetch('/report?what=' + encodeURIComponent(what) +
        '&result=' + encodeURIComponent(result));
}
navigator.geolocation.getCurrentPosition(
  function () { say('geo', 'position'); },
  function (e) { say('geo', 'error-' + e.code); });
// The tracks are stopped the instant they arrive. A granted case opens a real
// microphone and a real camera on somebody's machine, and a test that leaves
// either running past the assertion it needed them for is a test that turns a
// light on and walks away. Nothing is read from the stream; only the number of
// tracks is reported, never a device label -- a label names somebody's
// hardware and the assertion does not need it.
function media(kind, want) {
  navigator.mediaDevices.getUserMedia(want)
    .then(function (s) {
      var n = s.getTracks().length;
      s.getTracks().forEach(function (t) { t.stop(); });
      say(kind, 'stream-' + n);
    })
    .catch(function (e) { say(kind, e.name); });
}
media('mic', { audio: true });
media('cam', { video: true });
Notification.requestPermission().then(function (p) { say('notify', p); });
</script></body></html>)HTML";

// Serves the page and collects what it reported.
class origin : public QTcpServer {
public:
	QHash<QString, QString> reported;

	void incomingConnection(qintptr fd) override {
		auto *s = new QTcpSocket(this);
		s->setSocketDescriptor(fd);
		connect(s, &QTcpSocket::readyRead, this, [this, s] {
			const QByteArray head = s->readAll();
			const QByteArray target = head.mid(4, head.indexOf(' ', 4) - 4);

			QByteArray type = "text/html", body;
			if (target.startsWith("/report")) {
				const QUrlQuery q(QUrl(QString::fromUtf8(target)).query());
				reported.insert(q.queryItemValue("what"), q.queryItemValue("result"));
				type = "text/plain";
				body = "ok";
			} else if (target.startsWith("/page")) {
				body = k_page;
			} else {
				body = "";
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
	qputenv("HYDRA_PERM_DEBUG", "1");
	qInstallMessageHandler(collect);
	QApplication app(argc, argv);

	const QString out = qEnvironmentVariableIsSet("HYDRA_TEST_OUT")
	                        ? qgetenv("HYDRA_TEST_OUT") : QString("/tmp/hydra-test");
	QDir().mkpath(out);
	QDir(out + "/state").removeRecursively();
	QFile::remove(out + "/policy.ini");
	QFile::remove(out + "/policy.json");   // and the file it migrates from
	const QString tree = out + "/tree.txt";
	QFile tf(tree);
	if (!tf.open(QIODevice::WriteOnly | QIODevice::Truncate)) return 1;
	tf.write("- [f0] folder | Capture\n"
	          "  - [a1] unopened | Blank | about:blank | "
	          "created=2026-01-01T00:00:00 | seen=2026-01-01T00:00:00\n");
	tf.close();

	// One origin per case, on its own port. Chromium remembers a permission
	// answer per *origin* for the session, and an origin is host **and port** --
	// so re-asking on the same port would be answered from that memory and the
	// page would never reach our decider a second time. Same host throughout, so
	// the per-host policy still applies to all three.
	origin servers[6];
	quint16 ports[6];
	for (int i = 0; i < 6; ++i) {
		if (!servers[i].listen(QHostAddress::LocalHost, 0)) {
			std::printf("could not listen\n");
			return 1;
		}
		ports[i] = servers[i].serverPort();
	}
	std::printf("origins on 127.0.0.1: %u %u %u %u %u %u\n",
	             ports[0], ports[1], ports[2], ports[3], ports[4], ports[5]);

	policy_engine       policy;
	request_filter      filter(&policy);
	qtwebengine_factory factory(&filter);
	main_window w(&factory, &policy, &filter);
	w.load_tree(tree);
	w.resize(1000, 700);
	w.show();
	spin(1500);

	auto *tree_view = w.findChild<QTreeView *>();
	emit tree_view->activated(
	  tree_view->model()->index(0, 0, tree_view->model()->index(0, 0)));
	spin(1500);

	QLineEdit *bar = nullptr;
	for (QLineEdit *e : w.findChildren<QLineEdit *>())
		if (e->placeholderText() == "Address") bar = e;
	if (!bar) { std::printf("NO ADDRESS BAR\n"); return 1; }

	int visit = 0;
	origin *server = &servers[0];
	auto run_case = [&](int which, const char *title) {
		server = &servers[which];
		server->reported.clear();
		g_perm_log.clear();
		bar->setText(QString("http://127.0.0.1:%1/page?v=%2")
		                 .arg(ports[which]).arg(++visit));
		QMetaObject::invokeMethod(bar, "returnPressed");
		spin(5000);
		std::printf("\n== %s ==\n", title);
		// What the decider would answer, printed beside what the page saw. The
		// two disagreeing is the whole diagnosis: policy saying allow while the
		// page still sees a refusal means the answer never reached the engine.
		std::printf("  policy geo=%d cam=%d mic=%d notify=%d\n",
		             int(policy.is_allowed(policy::feature::geolocation, "127.0.0.1")),
		             int(policy.is_allowed(policy::feature::camera, "127.0.0.1")),
		             int(policy.is_allowed(policy::feature::microphone, "127.0.0.1")),
		             int(policy.is_allowed(policy::feature::notifications, "127.0.0.1")));
		for (auto it = server->reported.cbegin(); it != server->reported.cend(); ++it)
			std::printf("  page  %s -> %s\n", qPrintable(it.key()),
			             qPrintable(it.value()));
		if (server->reported.isEmpty())
			std::printf("  page  (reported nothing — did it load?)\n");
		for (const QString &l : std::as_const(g_perm_log))
			std::printf("  ours  %s\n", qPrintable(l));
	};
	auto got = [&](const char *what) { return server->reported.value(what); };
	// Our own feature names, as the debug line prints them. It used to print
	// Qt's raw enum number, and this matched on that -- until 6.8 introduced
	// QWebEnginePermission with a differently ordered enum, at which point every
	// number here silently meant a different permission. Names do not renumber.
	auto answered = [&](const char *feature, const char *verdict) {
		const QString want = QString("asked for %1 -> %2").arg(feature).arg(verdict);
		for (const QString &l : std::as_const(g_perm_log))
			if (l.contains(want)) return true;
		return false;
	};

	// "Refused", as the page can tell.
	//
	// Chromium says `NotAllowedError` when a permission is denied and
	// `AbortError` when it cannot open the device at all -- and which of the two
	// getUserMedia reports for a denied camera changed between the Chromium in
	// Qt 6.8.2 and the one in 6.11: same machine, same decision, same run,
	// different word. Neither is a grant, and the thing under test is that the
	// page did not get the device. Matching one spelling of a refusal made a Qt
	// upgrade look like a permissions regression.
	auto refused = [&](const char *what) {
		const QString v = got(what);
		return v == "NotAllowedError" || v == "AbortError";
	};

	// Somebody to press the button, since `ask` puts a modal dialog in the way
	// and nothing in a sweep is holding a mouse.
	//
	// **A watcher rather than a one-shot.** The prompt appears an unpredictable
	// time after the navigation -- the page has to load and call `getUserMedia`
	// first -- and a single shot timed to guess that is a flake waiting to
	// happen, in the one test that hangs for ever when it misses. This polls
	// instead. It is bounded by the driver: it spawns nothing, it acts at most
	// once per dialog, and it dies with the run.
	//
	// It is armed for every case, not only the two that expect a prompt, and
	// `prompts_seen` is asserted to be zero for the ones that do not. A dialog
	// nobody expected would otherwise hang the sweep unattended, which is the
	// exact failure this driver is now written to make impossible.
	int prompts_seen = 0;
	const char *press_button = nullptr;   // null means dismiss, which refuses
	QTimer watcher;
	watcher.setInterval(150);
	QObject::connect(&watcher, &QTimer::timeout, [&] {
		for (QWidget *tl : QApplication::topLevelWidgets()) {
			if (tl->objectName() != "permission_dialog" || !tl->isVisible())
				continue;
			++prompts_seen;
			if (press_button) {
				if (auto *b = tl->findChild<QPushButton *>(press_button))
					b->click();
				else
					qobject_cast<QDialog *>(tl)->reject();
			} else {
				qobject_cast<QDialog *>(tl)->reject();
			}
			return;
		}
	});
	watcher.start();

	// 1. Explicitly blocked, which is no longer the same thing as the defaults.
	//
	// **This case used to say "the sec 7.2 defaults" and run without setting
	// anything.** Three of these four now default to `ask`, so leaving it that
	// way would have put four modal dialogs on the screen and hung the driver
	// for ever -- in the sweep, unattended, with nothing to say why. Set here,
	// so the case keeps testing what it was written to test: that a refusal
	// travels from the decider to the page.
	policy.set_setting("127.0.0.1", policy::feature::geolocation,
	                    policy::setting::block);
	policy.set_setting("127.0.0.1", policy::feature::camera,
	                    policy::setting::block);
	policy.set_setting("127.0.0.1", policy::feature::microphone,
	                    policy::setting::block);
	policy.set_setting("127.0.0.1", policy::feature::notifications,
	                    policy::setting::block);
	run_case(0, "explicitly blocked: geo, camera, microphone, notifications");
	// **Whether this machine can exercise the camera at all**, decided once and
	// consulted by every camera assertion below.
	//
	// With no `/dev/video*` the page fails at device enumeration and Chromium
	// never requests the permission -- so the decider is never consulted, the
	// page reports `NotFoundError`, and every check about the camera fails for a
	// reason that has nothing to do with this browser. Four of them did exactly
	// that, and nothing in the output distinguished "no camera here" from "the
	// permission plumbing is broken". Now it does.
	//
	// Derived from the decider's own log rather than by looking for a device:
	// the question is not "is there a webcam" but "does a camera request reach
	// our code here", and the log is what answers that.
	const bool camera_reachable = answered("camera", "denied");

	check(answered("notifications", "denied") && answered("geolocation", "denied") &&
	       answered("microphone", "denied"),
	      "geolocation, microphone and notifications are refused by our decider");
	check(got("geo") == "error-1",
	      "geolocation is refused (PERMISSION_DENIED, not merely unavailable)");
	check(refused("mic"), QString("the microphone is refused (%1)").arg(got("mic")));
	if (camera_reachable)
		check(refused("cam"), QString("the camera is refused (%1)").arg(got("cam")));
	else
		skipped(QString("every camera check -- no camera request reaches the "
		                 "decider on this machine, so the page fails at device "
		                 "enumeration (%1) before any permission is asked")
		          .arg(got("cam")));
	check(got("notify") == "denied", "notifications are refused");

	// 2. Grant geolocation for this host alone. This is the row that isolates
	//    the callback: nothing else changes, and a refusal that turns into
	//    "granted, no provider" can only have come through our decider.
	policy.set_setting("127.0.0.1", policy::feature::geolocation,
	                    policy::setting::allow);
	run_case(1, "geolocation allowed for this host");
	check(answered("geolocation", "GRANTED"), "geolocation is now granted by our decider");
	check(answered("microphone", "denied") &&
	       (!camera_reachable || answered("camera", "denied")),
	      "while microphone and camera are still refused, so the grant is "
	      "per-feature and the enum mapping is right");
	check(refused("mic"),
	      "and the page still cannot open a microphone");
	// Deliberately not asserted: what the page sees for geolocation. This build
	// has no location provider, so a granted request still ends as
	// PERMISSION_DENIED -- the same code a refusal produces. Asserting on it
	// would be asserting on the engine's build options.
	std::printf("  --    page saw geo '%s' (engine has no location provider "
	             "here, so this cannot distinguish grant from refusal)\n",
	             qPrintable(got("geo")));

	// 3. And the notifications path separately, since it is the one feature
	//    here that answers with a word rather than an exception.
	policy.set_setting("127.0.0.1", policy::feature::notifications,
	                    policy::setting::allow);
	run_case(2, "notifications allowed for this host");
	check(answered("notifications", "GRANTED"), "notifications are granted by our decider");
	check(got("notify") == "granted",
	      "and the page really is told granted — the one feature here whose "
	      "end-to-end outcome the engine can actually deliver");
	if (camera_reachable)
		check(refused("cam"), "while the camera is still refused");

	// 4. Camera and microphone *granted*, which nothing here used to do.
	//    Every earlier case grants something else and re-asserts that these two
	//    stay refused, so the mapping was proved and the delivery was not: a
	//    decider that answered GRANTED while the engine handed the page nothing
	//    would have passed every assertion above. That is the shape of the
	//    complaint this was written for -- a page reporting no camera support --
	//    and it is the one arrangement that can tell the two apart.
	policy.set_setting("127.0.0.1", policy::feature::camera,
	                    policy::setting::allow);
	policy.set_setting("127.0.0.1", policy::feature::microphone,
	                    policy::setting::allow);
	run_case(3, "camera and microphone allowed for this host");
	check(answered("microphone", "GRANTED") &&
	       (!camera_reachable || answered("camera", "GRANTED")),
	      "the decider grants both");
	// **The relationship, not the value.** Whether a real device answers is a
	// property of the machine -- `NotFoundError` is the honest reply where there
	// is no webcam, and asserting on 'stream' would fail on a headless box for a
	// reason that has nothing to do with this browser. What must change is that
	// the page stops being *refused*: NotAllowedError and AbortError are the two
	// spellings of that, and neither may survive the grant.
	check(!refused("mic"),
	      QString("the microphone is no longer refused (%1)").arg(got("mic")));
	if (camera_reachable)
		check(!refused("cam"),
		      QString("the camera is no longer refused (%1)").arg(got("cam")));

	check(prompts_seen == 0,
	      "nothing has been prompted for yet: every case above sets allow or "
	      "block, and a prompt appearing under one of those would mean the "
	      "engine reached the dialog past a decision already taken");

	// 5. `ask`, which is the whole reason the decider became asynchronous and
	//    is the one path nothing here could previously reach. A bool decider
	//    had nowhere to put "wait, a person is being asked".
	//
	//    Only geolocation moves: camera, microphone and notifications were left
	//    on `allow` by the cases above, so exactly one dialog can appear and
	//    the count below means what it says.
	press_button = "permission_allow";
	policy.set_setting("127.0.0.1", policy::feature::geolocation,
	                    policy::setting::ask);
	run_case(4, "geolocation set to ask, and the person presses Allow");
	check(prompts_seen == 1, "a prompt was put on the screen");
	check(answered("geolocation", "GRANTED"),
	      "and pressing Allow reaches the engine as a grant -- the answer "
	      "survived the round trip out to a dialog and back");
	check(policy.effective_setting(policy::feature::geolocation, "127.0.0.1") ==
	        policy::setting::ask,
	      "with Remember unticked the site rule is untouched: the answer was "
	      "for this run, not for ever");

	// 6. Dismissal refuses, and the run-scoped memory means the question just
	//    answered is not asked again.
	//
	//    **The microphone, not the camera.** The obvious choice was the camera
	//    and it silently tested nothing: with no `/dev/video*` the request never
	//    reaches the decider, so no dialog appears, and "the prompt was
	//    dismissed and the page was refused" passes trivially for the wrong
	//    reason. The microphone is asked for on this machine -- `NotReadableError`
	//    rather than `NotFoundError` is the device existing and refusing to open
	//    -- so it is the one that actually exercises the path.
	//
	//    Geolocation is deliberately left on `ask` here. It was answered in case
	//    5 and the shell remembers that for the run, so it must produce no second
	//    dialog -- which is what stops a page calling `getUserMedia` in a loop
	//    from putting prompts on the screen as fast as they are dismissed. Only
	//    the microphone is newly `ask`, so `prompts_seen` rising by exactly one
	//    is the assertion that the memory worked.
	press_button = nullptr;
	policy.set_setting("127.0.0.1", policy::feature::microphone,
	                    policy::setting::ask);
	run_case(5, "microphone set to ask, and the person dismisses the dialog");
	check(prompts_seen == 2,
	      "exactly one more prompt: the microphone asked, and the geolocation "
	      "answered a moment ago was not asked again");
	check(answered("microphone", "denied"),
	      "dismissing is a refusal, not a deferral -- a prompt that treated "
	      "Escape as 'ask me again shortly' is how a page nags somebody into "
	      "agreeing");
	check(refused("mic"),
	      QString("and the page is refused the microphone (%1)").arg(got("mic")));
	check(answered("geolocation", "GRANTED"),
	      "while geolocation is still granted, from the run's memory rather "
	      "than from a rule on disk");

	// 7. **The evidence a person can actually reach.**
	//
	//    Everything above reads the decider's own debug log, which is a
	//    developer's instrument: it needs an environment variable on the desktop
	//    and is unreachable on a phone. What somebody using the browser has is
	//    the "something got through here" button, and the claim worth testing is
	//    that pressing it captures what the page asked for -- because that is
	//    the whole reason capability requests became a signal.
	//
	//    Read off disk rather than out of the dialog: `report_annoyance` files
	//    before it opens anything, deliberately, so the file is the record and
	//    the dialog is a view of it. Checking the file also proves the evidence
	//    survives the INI, which is where a QStringList of lines with commas in
	//    them could quietly go wrong.
	std::printf("\n== what the page asked for, as a person can see it ==\n");
	{
		// Close the report as soon as it appears; the filing has already
		// happened by then.
		QTimer watcher;
		watcher.setInterval(150);
		QObject::connect(&watcher, &QTimer::timeout, [] {
			for (QWidget *tl : QApplication::topLevelWidgets())
				if (tl->isVisible() && tl->objectName() == "annoyed_dialog")
					if (auto *d = qobject_cast<QDialog *>(tl)) { d->reject(); return; }
		});
		watcher.start();

		QAction *annoyed = nullptr;
		for (QToolBar *bar : w.findChildren<QToolBar *>())
			for (QAction *a : bar->actions())
				if (a->toolTip().contains("got through here"))
					annoyed = a;
		check(annoyed != nullptr, "the report button is on the toolbar");
		if (annoyed) {
			annoyed->trigger();
			spin(1500);
			watcher.stop();

			annoyance_log log;
			const bool loaded = log.load(out + "/annoyances.ini");
			check(loaded, "pressing it files a report");
			QStringList caps;
			if (loaded && log.count_for("127.0.0.1") > 0)
				caps = log.for_host("127.0.0.1").first().capabilities;
			check(!caps.isEmpty(),
			      QString("and the report carries what this page asked for (%1 "
			               "entr%2)").arg(caps.size())
			        .arg(caps.size() == 1 ? "y" : "ies"));
			// The cases above asked for geolocation, the microphone and
			// notifications on this host, so at least one of them must be named.
			// Not all three: the camera never reaches the decider on a machine
			// with no camera, which case 1 already established.
			bool named = false;
			for (const QString &c : caps)
				if (c.contains("Location") || c.contains("Microphone") ||
				    c.contains("Notifications"))
					named = true;
			check(named,
			      "naming a capability by the label the settings page uses, not "
			      "an engine enum somebody has to look up");
			for (const QString &c : caps)
				std::printf("  saw  %s\n", qPrintable(c));
		}
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
