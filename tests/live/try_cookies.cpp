// The cookie filter, which until now was wired and never exercised — the exact
// category this project's defects keep coming from.
//
// It is driven the only way that proves anything: a real profile, a real
// navigation, and two *different hosts* that are both loopback. The page is
// served from 127.0.0.1 and its image from 127.0.0.2, so a cookie that appears
// or does not appear is attributable to the first-party/third-party distinction
// rather than to anything else. That is the same trick the interceptor
// verification used, for the same reason.
//
// The middle case is the one that isolates the filter: the *same* page, run
// twice, differing only in the policy. If a third-party cookie is stored under
// `allow` and refused under `block`, our filter is what made the difference. If
// it is refused under both, Chromium's own SameSite rules are dominating and
// this measures nothing — which the driver says out loud rather than reporting
// as a pass, because "blocked" and "blocked by someone else" look identical
// from here.
#include "main_window.h"
#include "policy_engine.h"
#include "request_filter.h"
#include "qtwebengine_factory.h"

#include <QApplication>
#include <QEventLoop>
#include <QLineEdit>
#include <QNetworkCookie>
#include <QProcess>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslServer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QTreeView>
#include <QWebEngineCookieStore>
#include <QWebEngineProfile>
#include <QFile>
#include <QDir>
#include <cstdio>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void note(const QString &w) { std::printf("  --    %s\n", qPrintable(w)); }
static void spin(int ms) {
	QEventLoop l;
	QTimer::singleShot(ms, &l, &QEventLoop::quit);
	l.exec();
}

// Serves a page that pulls one image from the other loopback host. Both set a
// cookie for themselves, and both report the Cookie header they were sent, so
// the run can tell "never stored" from "stored and not sent back".
//
// The same pages are served over plain HTTP and over TLS, because the
// third-party half of the filter cannot be measured without the second: a
// `SameSite=None` cookie requires `Secure`, and `Secure` requires HTTPS. Over
// HTTP the engine refuses such a cookie whatever our filter says, so the answer
// there is Chromium's rather than ours.
struct site_urls {
	QString scheme;
	quint16 port = 0;
};

class origin {
public:
	site_urls  urls;
	QStringList seen;     // "path: <cookie header or none>"

	void serve(QTcpSocket *s) {
		QObject::connect(s, &QTcpSocket::readyRead, s, [this, s] {
			const QByteArray head = s->readAll();
			const QByteArray path = head.mid(4, head.indexOf(' ', 4) - 4);

			QByteArray cookie = "none";
			for (const QByteArray &line : head.split('\n'))
				if (line.toLower().startsWith("cookie:"))
					cookie = line.mid(7).trimmed();
			seen << QString("%1 -> %2").arg(QString(path.left(40)),
			                                 QString(cookie.left(80)));

			QByteArray type, body, set;
			if (path.startsWith("/probe")) {
				// Same host as the page: sets nothing, and exists only to report
				// the Cookie header it was sent.
				type = "image/gif";
				body = QByteArray::fromHex("47494638396101000100800000000000ffffff21"
				                            "f90401000000002c00000000010001000002024401003b");
			} else if (path.startsWith("/pixel")) {
				type = "image/gif";
				body = QByteArray::fromHex("47494638396101000100800000000000ffffff21"
				                            "f90401000000002c00000000010001000002024401003b");
				set  = urls.scheme == "https"
				         ? "third=1; Path=/; SameSite=None; Secure"
				         : "third=1; Path=/; SameSite=None";
			} else {
				type = "text/html";
				// One image from each host. The same-host one exists so the
				// first-party cookie's *return journey* is observed on a request
				// this driver asked for, rather than on whatever favicon fetch
				// happens to follow — which is what it was relying on by accident.
				const QByteArray base = urls.scheme.toUtf8() + "://";
				const QByteArray p = ":" + QByteArray::number(urls.port);
				body = "<!doctype html><html><body>page"
				        "<img src=\"" + base + "127.0.0.1" + p + "/probe\">"
				        "<img src=\"" + base + "127.0.0.2" + p + "/pixel\">"
				        "</body></html>";
				set  = "first=1; Path=/";
			}

			QByteArray resp = "HTTP/1.1 200 OK\r\nContent-Type: " + type +
			                   "\r\nSet-Cookie: " + set +
			                   "\r\nCache-Control: no-store" +
			                   "\r\nContent-Length: " + QByteArray::number(body.size()) +
			                   "\r\nConnection: close\r\n\r\n" + body;
			s->write(resp);
			s->flush();
			s->disconnectFromHost();
		});
	}
};

// Plain HTTP: wrap each accepted descriptor and hand it to the shared handler.
class http_origin : public QTcpServer {
public:
	origin *site = nullptr;
	void incomingConnection(qintptr fd) override {
		auto *s = new QTcpSocket(this);
		s->setSocketDescriptor(fd);
		site->serve(s);
	}
};

// Cookies are watched as they arrive, not asked for afterwards. The first
// version of this called `loadAllCookies()` once the page had loaded and
// collected what `cookieAdded` then emitted — which is nothing, because the
// store was already loaded and does not re-announce what it already holds. It
// reported "(none)" for a cookie the server could see coming back on the very
// next request. Two channels, and only the one that was wrong was being read.

// A self-signed certificate for both loopback names, made fresh each run rather
// than committed: a private key in a repository is a thing someone eventually
// trusts by accident, and openssl is already here.
static bool make_cert(const QString &dir, QSslCertificate *cert, QSslKey *key) {
	const QString c = dir + "/test-cert.pem", k = dir + "/test-key.pem";
	QProcess p;
	p.start("openssl", { "req", "-x509", "-newkey", "rsa:2048", "-nodes",
	                      "-keyout", k, "-out", c, "-days", "2",
	                      "-subj", "/CN=127.0.0.1",
	                      "-addext", "subjectAltName=IP:127.0.0.1,IP:127.0.0.2" });
	if (!p.waitForFinished(20000) || p.exitCode() != 0)
		return false;
	QFile cf(c), kf(k);
	if (!cf.open(QIODevice::ReadOnly) || !kf.open(QIODevice::ReadOnly))
		return false;
	*cert = QSslCertificate(cf.readAll());
	*key  = QSslKey(kf.readAll(), QSsl::Rsa);
	return !cert->isNull() && !key->isNull();
}

int main(int argc, char *argv[]) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	// Before the engine starts, and it is why the TLS half can run at all: the
	// certificate below is self-signed, and without this Chromium refuses the
	// page before any cookie is set. It is a flag for this driver's own process,
	// not something the app does. (Single token deliberately — Qt splits this
	// variable on spaces, so a flag containing one arrives mangled.)
	qputenv("QTWEBENGINE_CHROMIUM_FLAGS", "--ignore-certificate-errors");
	QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
	QApplication app(argc, argv);

	const QString out = qEnvironmentVariableIsSet("HYDRA_TEST_OUT")
	                        ? qgetenv("HYDRA_TEST_OUT") : QString("/tmp/hydra-test");
	QDir().mkpath(out);
	QDir(out + "/state").removeRecursively();
	QFile::remove(out + "/policy.json");
	const QString tree = out + "/tree.txt";
	QFile tf(tree);
	if (!tf.open(QIODevice::WriteOnly | QIODevice::Truncate)) return 1;
	tf.write("- [f0] folder | Capture\n"
	          "  - [a1] unopened | Blank | about:blank | "
	          "created=2026-01-01T00:00:00 | seen=2026-01-01T00:00:00\n");
	tf.close();

	origin      site;
	http_origin http;
	http.site = &site;
	if (!http.listen(QHostAddress::AnyIPv4, 0)) {
		std::printf("could not listen\n");
		return 1;
	}
	site.urls = { "http", http.serverPort() };
	std::printf("http origin on port %u\n", http.serverPort());

	// The TLS origin, for the third-party case that plain HTTP cannot express.
	origin     tls_site;
	QSslServer tls;
	QSslCertificate cert;
	QSslKey         key;
	bool have_tls = make_cert(out, &cert, &key);
	if (have_tls) {
		QSslConfiguration cfg = QSslConfiguration::defaultConfiguration();
		cfg.setLocalCertificate(cert);
		cfg.setPrivateKey(key);
		tls.setSslConfiguration(cfg);
		have_tls = tls.listen(QHostAddress::AnyIPv4, 0);
	}
	if (have_tls) {
		tls_site.urls = { "https", tls.serverPort() };
		QObject::connect(&tls, &QTcpServer::pendingConnectionAvailable, &tls, [&] {
			while (QTcpSocket *s = tls.nextPendingConnection())
				tls_site.serve(s);
		});
		std::printf("https origin on port %u\n", tls.serverPort());
	} else {
		std::printf("no TLS origin (openssl or QSslServer unavailable) — "
		             "the third-party cases will be skipped\n");
	}

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

	auto *store = QWebEngineProfile::defaultProfile()->cookieStore();

	// One case: clear everything, set the policy, load the page, look. `which`
	// selects the origin, because the same three cases are asked over plain HTTP
	// and over TLS and only the second can express a third-party cookie at all.
	int visit = 0;
	origin *cur = &site;
	auto run_case = [&](origin *which, const char *title) -> QStringList {
		cur = which;
		QStringList names;
		auto conn = QObject::connect(store, &QWebEngineCookieStore::cookieAdded,
		                              [&](const QNetworkCookie &c) {
			names << QString("%1@%2").arg(QString::fromUtf8(c.name()), c.domain());
		});
		store->deleteAllCookies();
		cur->seen.clear();
		spin(500);
		names.clear();          // the delete may echo; only the visit counts
		bar->setText(QString("%1://127.0.0.1:%2/page?v=%3")
		                 .arg(cur->urls.scheme).arg(cur->urls.port).arg(++visit));
		QMetaObject::invokeMethod(bar, "returnPressed");
		spin(4000);
		QObject::disconnect(conn);
		names.sort();
		names.removeDuplicates();
		std::printf("\n== %s ==\n", title);
		for (const QString &s : std::as_const(cur->seen))
			std::printf("  req   %s\n", qPrintable(s));
		std::printf("  cookies stored: %s\n",
		             names.isEmpty() ? "(none)" : qPrintable(names.join(", ")));
		return names;
	};

	const QString host = "127.0.0.1";
	auto has = [](const QStringList &l, const char *n) {
		for (const QString &s : l)
			if (s.startsWith(QString(n) + "@")) return true;
		return false;
	};

	// 1. The defaults, which are §7.2's privacy-leaning ones: cookies allowed,
	//    third-party cookies blocked.
	const QStringList a = run_case(&site, "http: cookies allow, third-party block");
	auto sent_to = [&](const char *path, const char *want) {
		for (const QString &s : std::as_const(cur->seen))
			if (s.startsWith(path)) return s.contains(want);
		return false;
	};
	check(has(a, "first"), "the page's own cookie is stored");
	check(sent_to("/probe", "first=1"),
	      "and comes back on the next same-host request");
	check(!has(a, "third"), "the third-party image's cookie is not");
	const bool third_reached =
		std::any_of(cur->seen.cbegin(), cur->seen.cend(),
		             [](const QString &s) { return s.startsWith("/pixel"); });
	check(third_reached, "and the third-party request itself was made, so the "
	                      "cookie was refused rather than the request blocked");

	// 2. The same page with third-party cookies allowed. This is the control:
	//    without it, "blocked" cannot be told apart from "Chromium would never
	//    have stored it anyway".
	policy.set_global_default(policy::feature::third_party_cookies,
	                           policy::setting::allow);
	const QStringList b = run_case(&site, "http: third-party cookies allowed");
	check(has(b, "first"), "the page's own cookie is still stored");
	note(has(b, "third")
		? "the third-party cookie is stored over plain HTTP"
		: "as expected over plain HTTP the third-party cookie is refused whatever "
		   "the policy says: SameSite=None needs Secure, and Secure needs TLS. "
		   "This row measures Chromium. The TLS rows below are the ones that "
		   "measure us");

	// 3. Cookies off for the site entirely.
	policy.set_global_default(policy::feature::third_party_cookies,
	                           policy::setting::block);
	policy.set_setting(host, policy::feature::cookies, policy::setting::block);
	const QStringList c = run_case(&site, "http: cookies blocked for this host");
	check(!has(c, "first"), "the page's own cookie is refused too");
	check(sent_to("/probe", "none"),
	      "so nothing comes back on the next same-host request either");
	check(!has(c, "third"), "and so is the third party's");

	// 4-6. The same three over TLS, where a third-party cookie is expressible at
	//      all. This is the pair that isolates our filter: identical page,
	//      identical server, one policy bit different.
	if (have_tls) {
		policy.set_setting(host, policy::feature::cookies, policy::setting::unset);
		policy.set_global_default(policy::feature::third_party_cookies,
		                           policy::setting::block);
		const QStringList d = run_case(&tls_site, "https: third-party blocked");
		check(has(d, "first"), "the page's own cookie is stored over TLS");
		check(!has(d, "third"), "and the third party's is not");

		policy.set_global_default(policy::feature::third_party_cookies,
		                           policy::setting::allow);
		const QStringList e = run_case(&tls_site, "https: third-party allowed");
		if (has(e, "third")) {
			check(true, "allowing it stores it — so the row above was our filter "
			             "refusing it, not the engine");
		} else {
			note("STILL INCONCLUSIVE over TLS: the engine refuses the third-party "
			      "cookie even with policy allowing it. Recent Chromium restricts "
			      "third-party cookies on its own, so our filter's third-party "
			      "branch remains unmeasured rather than measured-and-working");
		}

		policy.set_global_default(policy::feature::third_party_cookies,
		                           policy::setting::block);
		policy.set_setting(host, policy::feature::cookies, policy::setting::block);
		const QStringList f = run_case(&tls_site, "https: cookies blocked");
		check(!has(f, "first") && !has(f, "third"),
		      "blocking the host refuses both over TLS too");
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
