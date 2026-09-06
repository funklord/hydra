// The guard that keeps two browsers out of one profile directory.
//
// **It had no test at all, and its correctness rested on a comment.**
// `single_instance` is what stops a second launch opening the same tree, state
// directory and engine profile as a running one -- a shared profile is how a
// tab tree gets written twice and read once -- and every claim about how it
// works was recorded in prose beside the code that makes it work.
//
// The load-bearing claim is about Qt rather than about this project, which is
// what makes it worth pinning: `QLockFile` decides whether a lock is stale by
// comparing the *executable* name it wrote against what the system reports for
// that pid. Were it the **application** name, "Hydra" would never match the
// `hydra` that /proc reports, every live owner would be declared stale, and
// the guard would let a second instance straight through -- silently, and
// exactly when two are running. So this suite sets an application name that
// differs from the executable's and asserts which of the two comes back.
//
// What is deliberately not tested here: taking over a lock left by a process
// that was killed. Inducing it needs either a child to kill or a lock file
// hand-written in QLockFile's own format, and a test that hard-codes another
// library's file format tests the format rather than the guard. The claim is
// recorded in `single_instance.cpp` and remains unheld.
#include "single_instance.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QString>

#include <cstdio>
#include <functional>
#include <unistd.h>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void section(const char *n) { std::printf("\n== %s ==\n", n); }

static void spin_until(const std::function<bool()> &done, int ms = 2000) {
	QElapsedTimer t;
	t.start();
	while (!done() && t.elapsed() < ms)
		QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

int main(int argc, char **argv) {
	QCoreApplication app(argc, argv);
	// **Deliberately not the executable name.** The assertion below turns on
	// the two being different; with them equal every check here would pass
	// whichever name Qt wrote.
	QCoreApplication::setApplicationName("Hydra");

	const QString root = QDir::tempPath() + "/hydra-instance-test";
	QDir(root).removeRecursively();
	QDir().mkpath(root + "/one");
	QDir().mkpath(root + "/two");

	section("one process may hold a directory");
	{
		single_instance first(root + "/one");
		check(first.acquire(), "the first instance takes it");
		check(first.owner().isEmpty(),
		      "and has nobody to name, having been refused by nobody");

		single_instance second(root + "/one");
		check(!second.acquire(), "a second instance pointed at it is refused");
		check(!second.owner().isEmpty(),
		      QString("and is told who has it (%1)").arg(second.owner()));

		// **The Qt claim, pinned.** `owner()` is built from
		// `QLockFile::getLockInfo`, whose name field is what the staleness
		// check compares against /proc. It must be the executable, because
		// the application name would never match.
		check(second.owner().contains("test_instance"),
		      QString("named by the executable, which is what the staleness "
		               "check compares (%1)").arg(second.owner()));
		check(!second.owner().contains("Hydra"),
		      "and not by the application name, which would never match "
		      "/proc and would make every live owner look stale");
		check(second.owner().contains(QString::number(::getpid())),
		      QString("with the pid holding it (%1)").arg(second.owner()));
	}

	section("a different directory is a different application");
	{
		// The whole reason everything is keyed on the directory rather than on
		// a fixed name: two runs pointed at different profiles are two
		// applications, which is what makes an isolated test run possible.
		single_instance one(root + "/one");
		single_instance two(root + "/two");
		check(one.acquire(), "one profile is taken");
		check(two.acquire(), "and the other is taken as well, not refused");
	}

	section("the directory is released when its holder goes");
	{
		{
			single_instance held(root + "/one");
			check(held.acquire(), "held once");
		}
		single_instance again(root + "/one");
		check(again.acquire(),
		      "and taken again after the holder was destroyed, rather than "
		      "leaving a lock nothing can clear");
	}

	section("a refused instance hands its argument over");
	{
		single_instance holder(root + "/one");
		check(holder.acquire(), "the holder has the directory");

		QString got;
		bool arrived = false;
		holder.on_message([&](const QString &m) { got = m; arrived = true; });

		single_instance late(root + "/one");
		check(!late.acquire(), "a later instance is refused");
		check(late.hand_over("https://example.test/page"),
		      "and hands its argument to the one holding it");
		spin_until([&] { return arrived; });
		check(arrived, "which arrives");
		check(got == "https://example.test/page",
		      QString("whole, rather than truncated or empty (%1)").arg(got));

		// An empty message is the "launched again with nothing to open" case,
		// which must still be delivered -- it is a request to come to the
		// front, and dropping it makes a second launch do nothing at all.
		arrived = false;
		got = "unset";
		check(late.hand_over(QString()), "an empty message is handed over too");
		spin_until([&] { return arrived; });
		check(arrived && got.isEmpty(),
		      QString("and delivered as empty rather than dropped (%1)")
		          .arg(arrived ? "arrived" : "never arrived"));
	}

	section("with nobody holding it there is nobody to hand to");
	{
		// The control for the section above: `hand_over` must be able to fail,
		// or "it handed over" says nothing.
		QDir().mkpath(root + "/three");
		single_instance alone(root + "/three");
		check(!alone.hand_over("anything"),
		      "handing over to an empty directory fails rather than "
		      "reporting a delivery nobody received");
	}

	QDir(root).removeRecursively();
	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
