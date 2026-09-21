// Does the local model get noticed when it is actually running?
//
// **Both servers are in-process**, from `ollama_stub.h`. They used to be a
// stub Ollama on port 8811 and a "blackhole" listener started by hand, which
// is why this suite sat outside `make test` -- and why the timeout section,
// the only place the probe's own deadline is measured, printed "(no
// blackhole endpoint given)" and checked nothing whenever it did run.
#include "ollama_provider.h"
#include "ollama_stub.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTimer>
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

	ollama_stub stub;
	const QString up = stub.start();
	blackhole silent;
	const QString hole = silent.start();
	if (up.isEmpty() || hole.isEmpty()) {
		std::printf("could not listen\n");
		return 1;
	}
	// A port nothing is on, for the refused-at-once case. Bound and closed
	// rather than picked: a hard-coded 9 is a guess about this machine, and a
	// discard service answering would turn "unreachable" into a pass for the
	// wrong reason.
	const QString down = [] {
		QTcpServer t;
		t.listen(QHostAddress::LocalHost, 0);
		const QString u = QStringLiteral("http://127.0.0.1:%1").arg(t.serverPort());
		t.close();
		return u;
	}();

	section("the bug: an async probe read immediately is always 'absent'");
	{
		ollama_provider p;
		p.set_endpoint(QUrl(up));
		p.probe();
		// This is exactly what choose_ai() used to do.
		check(!p.available(),
		      "reading available() straight after probe() sees the stale answer");
		check(p.probe_now(), "probe_now() waits and sees the running server");
	}

	section("probe_now against a reachable server");
	{
		ollama_provider p;
		p.set_endpoint(QUrl(up));
		QElapsedTimer t; t.start();
		check(p.probe_now(), "reports reachable");
		check(t.elapsed() < 2000,
		      QString("and returns promptly (%1 ms)").arg(t.elapsed()));
		check(p.available(), "available() agrees afterwards");
	}

	section("probe_now against nothing");
	{
		ollama_provider p;
		p.set_endpoint(QUrl(down));
		QElapsedTimer t; t.start();
		check(!p.probe_now(), "reports unreachable");
		check(t.elapsed() < 4000,
		      QString("without hanging (%1 ms)").arg(t.elapsed()));
	}

	section("it notices a change rather than caching");
	{
		ollama_provider p;
		p.set_endpoint(QUrl(down));
		check(!p.probe_now(), "unreachable first");
		p.set_endpoint(QUrl(up));
		check(p.probe_now(),
		      "and reachable after — a cached answer would have said no");
	}

	section("the timeout is configurable, and honoured");
	{
		// A port that *refuses* answers instantly; only a host that accepts
		// and then says nothing actually costs the timeout. `blackhole` is
		// that, and it is why this section can run at all now.
		ollama_provider p;
		p.set_endpoint(QUrl(hole));

		p.set_probe_timeout(600);
		check(p.probe_timeout() == 600, "the timeout is settable");
		QElapsedTimer t; t.start();
		check(!p.probe_now(), "a silent host is reported unreachable");
		const qint64 quick = t.elapsed();
		check(quick >= 500 && quick < 1600,
		      QString("and the short timeout is what was waited (%1 ms)")
		          .arg(quick));

		p.set_probe_timeout(2000);
		t.restart();
		p.probe_now();
		const qint64 slow = t.elapsed();
		check(slow > quick + 700,
		      QString("a longer timeout waits longer (%1 ms vs %2 ms)")
		          .arg(slow).arg(quick));
		check(slow < 3200, QString("but still bounded (%1 ms)").arg(slow));

		p.set_probe_timeout(10);
		check(p.probe_timeout() >= 100,
		      QString("an absurdly short timeout is clamped (%1 ms)")
		          .arg(p.probe_timeout()));
	}

	section("the probe asked the endpoint what it has");
	{
		// The stub is the fixture, so say what it saw: a suite whose server
		// was never asked anything would pass every check above by having
		// nothing to disagree with.
		check(!stub.seen.isEmpty(),
		       QString("the stub was asked for %1").arg(stub.seen.join(", ")));
		check(stub.seen.contains("/api/tags"),
		       "and it was /api/tags, which is what carries the model list");
	}

	section("the signal fires for the settings status line");
	{
		ollama_provider p;
		p.set_endpoint(QUrl(up));
		QSignalSpy spy(&p, &ollama_provider::probe_finished);
		p.probe();
		QEventLoop l; QTimer::singleShot(2500, &l, &QEventLoop::quit); l.exec();
		check(spy.count() == 1, QString("probe_finished emitted once (%1)").arg(spy.count()));
		check(spy.value(0).value(0).toBool(), "carrying the result");
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
