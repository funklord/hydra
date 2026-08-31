// The signal-to-Qt-signal plumbing that lets a killed browser save first.
//
// **What this can and cannot establish.** It drives the real handler with a
// real `raise()`, so the self-pipe, the notifier and the emission are measured
// rather than argued. What it deliberately does not do is send a second signal
// to prove the second one kills: that would kill this process, which is the
// point. The disposition is inspected with `sigaction` instead, which answers
// the same question exactly and leaves the suite alive to report it.
//
// Every assertion here is guarded on `armed()`. Raising SIGTERM at a process
// whose handler was not installed terminates it with no output at all, and a
// suite that dies silently on an unsupported platform is worse than one that
// says it skipped.
#include "shutdown_signals.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSignalSpy>

#include <csignal>
#include <cstdio>
#include <functional>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void section(const char *n) { std::printf("\n== %s ==\n", n); }

// Run the event loop until `done` or the deadline. Bounded by the timer, not
// by a count of iterations: what is being waited for is one descriptor
// becoming readable, and 2000ms is far past any plausible scheduling delay
// while still being a number this suite is guaranteed to return from.
static void spin_until(const std::function<bool()> &done, int ms = 2000) {
	QElapsedTimer t;
	t.start();
	while (!done() && t.elapsed() < ms)
		QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

// What the process would do about `signo` right now, straight from the kernel.
static void *disposition_of(int signo) {
	struct sigaction sa;
	if (::sigaction(signo, nullptr, &sa) != 0)
		return nullptr;
	return reinterpret_cast<void *>(sa.sa_handler);
}

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);

	section("what it takes, and what it leaves alone");
	{
		const QVector<int> h = shutdown_signals::handled();
		check(h.contains(SIGTERM), "SIGTERM is handled: a session manager's kill");
		check(h.contains(SIGINT),  "SIGINT is handled: Ctrl-C");
		check(h.contains(SIGHUP),  "SIGHUP is handled: the terminal going away");
		check(!h.contains(SIGQUIT),
		      "SIGQUIT is not: somebody sending it asked for a core dump");
		check(!h.contains(SIGKILL),
		      "and SIGKILL is not, because it cannot be");
	}

	section("arming installs a handler and disarming takes it back");
	{
		void *before = disposition_of(SIGTERM);
		{
			shutdown_signals s;
			check(s.armed(), "a first instance arms");
			check(disposition_of(SIGTERM) != before,
			      "and SIGTERM no longer does what it did");

			// The fds and the dispositions are the process's, so a second
			// object cannot own them. It must say so rather than installing
			// over the first and closing the pipe it is watching.
			shutdown_signals second;
			check(!second.armed(), "a second instance while one is alive is inert");
		}
		check(disposition_of(SIGTERM) == before,
		      "and the disposition is put back when it goes out of scope");

		shutdown_signals again;
		check(again.armed(),
		      "so a later instance can arm: the destructor released the pipe");
	}

	section("a signal arrives as a Qt signal, on the event loop's thread");
	{
		shutdown_signals s;
		if (!s.armed()) {
			std::printf("  --    not armed on this platform; skipped\n");
		} else {
			QSignalSpy spy(&s, &shutdown_signals::received);

			// Nothing may have been emitted before the loop runs. This is the
			// assertion that separates the design from the one that does the
			// work in the handler: if `received` had already fired here, it
			// fired from inside the handler, where a save is not legal.
			::raise(SIGTERM);
			check(spy.isEmpty(),
			      "raising it emits nothing yet -- the handler only writes a byte");

			spin_until([&] { return !spy.isEmpty(); });
			check(spy.size() == 1, "the event loop then delivers it, once");
			if (!spy.isEmpty())
				check(spy.at(0).at(0).toInt() == SIGTERM,
				      "carrying the number of the signal that arrived");

			// SA_RESETHAND. A save that wedges must not make the process
			// unkillable, so the next SIGTERM has to be the default action
			// again. Asserted by inspection rather than by sending one,
			// which would end this suite here.
			check(disposition_of(SIGTERM) == reinterpret_cast<void *>(SIG_DFL),
			      "and SIGTERM is back to its default, so a second one kills");
		}
	}

	section("a shutdown is reported once, however many signals arrive");
	{
		shutdown_signals s;
		if (!s.armed()) {
			std::printf("  --    not armed on this platform; skipped\n");
		} else {
			QSignalSpy spy(&s, &shutdown_signals::received);
			::raise(SIGTERM);
			// SIGHUP still has this object's handler -- SA_RESETHAND reset
			// only the one that was delivered -- so this is a second byte in
			// the same pipe, which is exactly the case being pinned.
			::raise(SIGHUP);
			spin_until([&] { return !spy.isEmpty(); });
			// Give a second emission every chance to happen before denying it.
			spin_until([&] { return spy.size() > 1; }, 300);
			check(spy.size() == 1,
			      "two signals, one emission: the receiver may delete the "
			      "window it was called from");
		}
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
