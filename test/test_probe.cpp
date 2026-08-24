// Does the local model get noticed when it is actually running?
#include "ollama_provider.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QSignalSpy>
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
	const QString up   = argc > 1 ? argv[1] : "http://127.0.0.1:8811";
	const QString down = "http://127.0.0.1:9";   // nothing listening

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
		// A port that *refuses* answers instantly; only a host that accepts and
		// then says nothing actually costs the timeout. argv[2] is that.
		const QString hole = argc > 2 ? argv[2] : QString();
		if (hole.isEmpty()) {
			std::printf("  --    (no blackhole endpoint given)\n");
		} else {
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
