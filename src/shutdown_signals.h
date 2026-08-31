#pragma once

#include <QObject>
#include <QVector>

class QSocketNotifier;

// SIGTERM, SIGINT and SIGHUP turned into an ordinary Qt signal, delivered on
// the thread that owns the event loop.
//
// **Why this exists.** `main_window::closeEvent` is what writes the tree, the
// policy, the view state and every live tab's suspended blob. A window that is
// closed runs it; a process that is signalled does not. So logging out, a
// shutdown, a Ctrl-C in the terminal it was started from, or the session
// manager reaping the application all discarded whatever had happened since
// the last debounced flush -- and discarded the view state and the tab blobs
// outright, since nothing but `closeEvent` ever wrote those.
//
// **A handler must not do the saving, which is the whole difficulty.** Only
// async-signal-safe functions may be called from one: not `QSettings`, not the
// model, not anything that allocates. A handler that saves directly can
// deadlock on a mutex the interrupted code already holds, or interleave with a
// write that was already in progress -- and it does it at the exact moment the
// system is taking the machine down, which is the worst available time to
// discover it.
//
// So the handler writes one byte to a pipe and returns, and a `QSocketNotifier`
// wakes the event loop to do the real work. This is Qt's own documented
// recipe, and the byte is the signal number so a receiver can report which one
// arrived. Everything past `received()` runs on the Qt thread under no
// restrictions at all.
//
// **A second signal is not swallowed.** The handlers are installed with
// `SA_RESETHAND`, so delivering one puts the default disposition back: if the
// save wedges, or takes longer than whoever sent the signal is willing to
// wait, the next Ctrl-C or the shutdown sequence's SIGKILL terminates the
// process the way it would have without any of this. A shutdown handler that
// can make a program unkillable is worse than none.
//
// **What it does not cover, and cannot.** SIGKILL and a genuine crash are not
// deliverable, so nothing here helps with either. That is the timer's job, and
// the timer is why the writers underneath were made atomic: an interrupted
// save that leaves a truncated `tree.txt` is a worse outcome than a stale one,
// because a stale file is still a tree.
//
// Unix only. Everything below is `sigaction`, `pipe2` and `write`; on a
// platform without them `armed()` is false, nothing is installed, and the
// program behaves exactly as it did before this class existed. Linux and
// Android are what this tree builds for and both have all three. macOS would
// need `pipe()` with two `fcntl` calls in place of `pipe2`, and is untried.
class shutdown_signals : public QObject {
	Q_OBJECT

public:
	explicit shutdown_signals(QObject *parent = nullptr);
	~shutdown_signals() override;

	shutdown_signals(const shutdown_signals &) = delete;
	shutdown_signals &operator=(const shutdown_signals &) = delete;

	// False when the handlers could not be installed -- an unsupported
	// platform, a pipe that could not be made, or a second instance built
	// while a first one is alive. A caller has nothing to do about it beyond
	// knowing that no `received()` is coming; the signal dispositions are the
	// process's, not this object's, so two of these cannot both own them.
	bool armed() const { return m_armed; }

	// The signals taken, in the order installed. Named rather than hard-coded
	// at each site so a test can ask instead of assuming.
	static QVector<int> handled();

signals:
	// Emitted once, on the event loop's thread, with the signal number that
	// arrived. Once, and not once per signal: a shutdown happens a single
	// time, `SA_RESETHAND` means a second delivery kills rather than queues,
	// and a receiver is entitled to tear down the window it was called from.
	void received(int signo);

private:
	void drain();

	QSocketNotifier *m_notifier = nullptr;
	bool             m_armed    = false;
};
