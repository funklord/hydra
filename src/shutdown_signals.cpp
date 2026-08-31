#include "shutdown_signals.h"

#include <QSocketNotifier>
#include <QtGlobal>

#ifdef Q_OS_UNIX
#include <csignal>
#include <cstring>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

#ifdef Q_OS_UNIX
namespace {

// Process-wide, because signal dispositions are. The handler cannot be given
// a `this` -- it takes the signal number and nothing else -- so the write end
// has to be reachable from file scope, and that in turn is what makes a second
// live instance meaningless rather than merely redundant.
int g_fd[2] = {-1, -1};

// `extern "C"` because that is what a signal handler's type is; a C++-linkage
// function passed to `sigaction` is undefined behaviour that happens to work
// everywhere.
extern "C" void on_signal(int signo) {
	// Async-signal-safe calls only. `write` is on POSIX's list; nothing else
	// this function touches is a call at all.
	//
	// **errno is saved and restored**, which is easy to leave out and hard to
	// debug when it is. A signal can be delivered between any two instructions,
	// including immediately after a failing syscall and before the code that
	// reads errno to decide what to do about it. A handler that leaves EPIPE
	// where EINTR was turns a retry into an error return three frames away,
	// with nothing near the fault to suggest a signal was involved.
	const int saved = errno;

	const unsigned char byte = static_cast<unsigned char>(signo);
	// EINTR is the only failure worth retrying. A full pipe means a signal is
	// already queued and undrained, which is the case this exists to handle
	// and needs no second byte; a closed one means the object is gone. Neither
	// is actionable from here, and `SA_RESETHAND` has already restored the
	// default disposition, so the next signal ends the process regardless.
	while (::write(g_fd[1], &byte, 1) < 0 && errno == EINTR)
		;

	errno = saved;
}

void set_disposition(void (*handler)(int), int flags) {
	struct sigaction sa;
	std::memset(&sa, 0, sizeof sa);
	sa.sa_handler = handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = flags;
	for (int s : shutdown_signals::handled())
		::sigaction(s, &sa, nullptr);
}

}  // namespace
#endif

QVector<int> shutdown_signals::handled() {
#ifdef Q_OS_UNIX
	// SIGTERM is what a session manager, `systemctl`, a service supervisor and
	// a plain `kill` send. SIGINT is Ctrl-C, which is how the browser is most
	// often stopped during development and therefore the one whose loss got
	// noticed. SIGHUP is the terminal going away, which under a session that
	// ends without a manager is the only notice anything gets.
	//
	// Deliberately not SIGQUIT: its default action is a core dump and somebody
	// sending it has asked for one, not for a tidy exit.
	return {SIGTERM, SIGINT, SIGHUP};
#else
	return {};
#endif
}

shutdown_signals::shutdown_signals(QObject *parent) : QObject(parent) {
#ifdef Q_OS_UNIX
	// A second instance would install handlers over the first one's and then
	// close the pipe the first one is still watching. Refuse instead, and say
	// so: this is a programming error rather than a condition to handle.
	if (g_fd[0] != -1) {
		qWarning("shutdown_signals: one is already installed; this one is inert");
		return;
	}

	// O_CLOEXEC because Qt WebEngine forks helper processes and they have no
	// business inheriting this; O_NONBLOCK because `drain()` reads on a
	// notifier that has already said there is something there, and a blocking
	// read on a pipe that a spurious wakeup found empty would stall the whole
	// event loop.
	if (::pipe2(g_fd, O_CLOEXEC | O_NONBLOCK) != 0) {
		g_fd[0] = g_fd[1] = -1;
		qWarning("shutdown_signals: no pipe (%s); signals stay at their "
		          "defaults and a signalled exit will not save",
		          std::strerror(errno));
		return;
	}

	m_notifier = new QSocketNotifier(g_fd[0], QSocketNotifier::Read, this);
	connect(m_notifier, &QSocketNotifier::activated,
	        this, &shutdown_signals::drain);

	// SA_RESTART so that a signal arriving mid-syscall does not turn into an
	// EINTR every caller in the tree would have to be audited for; SA_RESETHAND
	// so that the second signal is the default action. See the header for why
	// the second one is not optional.
	set_disposition(on_signal, SA_RESTART | SA_RESETHAND);
	m_armed = true;
#else
	Q_UNUSED(parent);
#endif
}

shutdown_signals::~shutdown_signals() {
#ifdef Q_OS_UNIX
	if (!m_armed)
		return;

	// Order matters, and getting it wrong is silent. The handlers go first, so
	// nothing can write to a descriptor that is about to close. The notifier
	// goes next and explicitly: it is a child of this object, so `~QObject`
	// would destroy it -- but that runs *after* this body, which would leave a
	// notifier watching a closed descriptor for the length of the destructor,
	// and Qt warns or spins depending on the platform.
	set_disposition(SIG_DFL, 0);

	delete m_notifier;
	m_notifier = nullptr;

	::close(g_fd[0]);
	::close(g_fd[1]);
	g_fd[0] = g_fd[1] = -1;
	m_armed = false;
#endif
}

void shutdown_signals::drain() {
#ifdef Q_OS_UNIX
	unsigned char buf[16];
	const ssize_t n = ::read(g_fd[0], buf, sizeof buf);
	if (n <= 0)
		return;   // a spurious wakeup; nothing arrived

	// Disarm before emitting, not after. A receiver's whole job here is to
	// save and quit, which may delete the window that owns this object -- so
	// this function must have finished touching itself before the emission,
	// and there must be no second one behind it. Only the first byte is
	// reported for the same reason; the rest are the same shutdown.
	m_notifier->setEnabled(false);
	emit received(static_cast<int>(buf[0]));
#endif
}
