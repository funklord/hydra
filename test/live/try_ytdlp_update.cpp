// The yt-dlp updater against the real GitHub release, end to end: it asks
// which release is latest, checks the signed checksums, fetches yt-dlp,
// and the resolver then runs that copy. Live: it needs the network.
//
//     ./test/build-make/try_ytdlp_update
//
// **Why this exists**: asked for by the holder 2026-10-22, after the
// shipped copy was found three months old. `test_ytdlp` checks the parts
// with a recorded release; this is the one place they meet the service.
// It works in Qt's test mode (`live_paths.cpp`), so nothing lands in the
// person's own profile.
#include "ytdlp_resolver.h"
#include "ytdlp_updater.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QProcess>
#include <QTimer>
#include <cstdio>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}

int main(int argc, char *argv[]) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);
	ytdlp_updater up;
	bool done = false, ok = false, updated = false;
	QString said;
	QObject::connect(&up, &ytdlp_updater::finished, [&](bool o, bool u, const QString &t) {
		done = true; ok = o; updated = u; said = t;
	});
	up.check(true);
	QEventLoop loop;
	QTimer poll;
	poll.setInterval(200);
	QObject::connect(&poll, &QTimer::timeout, [&] { if (done) loop.quit(); });
	poll.start();
	QTimer::singleShot(180000, &loop, &QEventLoop::quit);
	loop.exec();
	check(done && ok, QString("the check finished well (%1)").arg(said));
	std::printf("  at %s\n", qPrintable(ytdlp_updater::directory()));
	check(!ytdlp_updater::program().isEmpty() && !ytdlp_updater::installed_version().isEmpty(),
	      QString("a release is in place, %1").arg(ytdlp_updater::installed_version()));

	ytdlp_resolver r;
	check(r.description().contains("updated") || !updated,
	      QString("and the resolver runs it where it is newer (%1)").arg(r.description()));
	QProcess p;
	p.start("python3", { ytdlp_updater::program(), "--version" });
	p.waitForFinished(30000);
	const QString v = QString::fromUtf8(p.readAllStandardOutput()).trimmed();
	check(v == ytdlp_updater::installed_version(),
	      QString("and it runs, saying it is %1").arg(v));

	bool again = false;
	QString said_again;
	done = false;
	QObject::disconnect(&up, nullptr, nullptr, nullptr);
	QObject::connect(&up, &ytdlp_updater::finished, [&](bool, bool u, const QString &t) {
		done = true; again = u; said_again = t;
	});
	up.check(true);
	QEventLoop loop2;
	QObject::connect(&poll, &QTimer::timeout, [&] { if (done) loop2.quit(); });
	QTimer::singleShot(120000, &loop2, &QEventLoop::quit);
	loop2.exec();
	check(done && !again, QString("a second check fetches nothing (%1)").arg(said_again));
	up.check(false);
	check(!up.busy(), "and an unforced one within the day does not even ask");

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
