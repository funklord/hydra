#include "single_instance.h"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QLocalSocket>
#include <QStandardPaths>
#include <QThread>
#include <QtGlobal>

namespace {

// A lock whose owner is gone is cleared by `QLockFile` the moment it is asked
// for -- measured, see `acquire()`. This number is the fallback for the one
// case where that reasoning cannot be applied: a lock file naming a *different*
// host, which happens when the data directory is on a network filesystem or
// when the machine has been renamed since the crash. Qt cannot ask another
// machine whether a pid is alive, so without an age limit such a lock would sit
// there for ever and the browser would never start again. Thirty seconds is
// Qt's own default and is stated rather than inherited, because it is the only
// thing standing between a renamed host and an unstartable browser. Measured
// on a hand-written lock naming another host: refused while fresh, granted
// once older than this, and refused for ever with the age limit set to zero --
// which is the configuration that looks tidier and is the one that bricks.
constexpr int k_stale_ms = 30000;

// The hand-over is one short line. These bound every wait in it, so that
// neither side can hang on the other: a second instance that cannot reach the
// first gives up and says so, and the first blocks its own event loop for at
// most `k_read_ms` per connection.
constexpr int k_connect_ms  = 500;
constexpr int k_connect_try = 4;
constexpr int k_retry_ms    = 250;
constexpr int k_write_ms    = 1000;
constexpr int k_read_ms     = 250;
constexpr int k_max_message = 8192;
// Bounded on purpose: a queue that grows while nothing is draining it is a
// leak with a plausible excuse.
constexpr int k_max_pending = 16;

QString lock_path_for(const QString &dir) {
	return QDir(dir).filePath(QStringLiteral("instance.lock"));
}

// Where the hand-over socket lives.
//
// **Not beside the lock, and not under a fixed name in /tmp.** A unix socket
// path goes into `sockaddr_un::sun_path`, which is 108 bytes on Linux, and the
// data directory is wherever `XDG_DATA_HOME` says -- deep enough under a test
// or a container root to overrun that silently. `RuntimeLocation` is short,
// per-user and mode 0700, so it is also the one place another user on the
// machine cannot leave a socket for us to connect to.
//
// The name still carries a digest of the directory, so that the socket and the
// lock always agree about which Hydra is meant. Two runs with different
// `XDG_DATA_HOME`s share one runtime directory and must not find each other.
QString socket_path_for(const QString &dir) {
	const QString runtime = QStandardPaths::writableLocation(
	  QStandardPaths::RuntimeLocation);
	if (runtime.isEmpty())
		return QString();
	const QByteArray key = QDir(dir).absolutePath().toUtf8();
	const QString digest = QString::fromLatin1(
	  QCryptographicHash::hash(key, QCryptographicHash::Sha1).toHex().left(16));
	return QDir(runtime).filePath(
	  QStringLiteral("hydra-%1.socket").arg(digest));
}

}  // namespace

single_instance::single_instance(const QString &dir)
  : m_lock_path(lock_path_for(dir)), m_socket_path(socket_path_for(dir)),
    m_lock(m_lock_path) {
	// The directory has to exist before a lock file can be made in it, and on
	// a first run nothing has created it yet.
	QDir().mkpath(dir);
	m_lock.setStaleLockTime(k_stale_ms);
}

single_instance::~single_instance() {
	// **Only the holder tidies up, and this was measured the hard way.** The
	// first version removed the socket here unconditionally, so a *refused*
	// instance -- which reaches this destructor a moment after handing its url
	// over -- unlinked the socket the running instance was still listening on.
	// The first link click worked, and every one after it was told the browser
	// was not answering. Nothing failed at the time it went wrong, which is
	// what made it worth a comment rather than a quiet fix.
	if (!m_primary)
		return;
	// Order matters: stop listening before the lock goes, so that the window
	// between "the lock is free" and "the socket is gone" cannot exist for the
	// next instance to connect into.
	delete m_server;
	m_server = nullptr;
	if (!m_socket_path.isEmpty())
		QLocalServer::removeServer(m_socket_path);
	// QLockFile unlocks and removes the file in its own destructor.
}

bool single_instance::acquire() {
	// **The lock is the arbiter, and the socket cannot be.** The obvious
	// socket-only design is "connect; if the connection is refused the owner
	// is gone, so `removeServer()` and listen instead". `removeServer()`
	// unlinks unconditionally -- it does not check that nobody is listening --
	// and two instances started together both fail to connect before either
	// has listened, so both unlink and both listen, and the guard passes twice.
	// That is not a corner case: a double-clicked launcher is exactly two
	// starts a few milliseconds apart, which is the thing this exists to stop.
	// Measured rather than argued: with one process listening on a path,
	// another calling `removeServer()` on it got true and its `listen()`
	// succeeded, leaving two servers where the design permits one.
	// `QLockFile::tryLock` is atomic and has no such window.
	//
	// `QSharedMemory` was the other candidate and fails the harder
	// requirement: on Unix a segment survives the process that made it, so one
	// `kill -9` leaves the browser permanently convinced it is already
	// running. Every workaround for that is a race dressed up as a heuristic.
	if (!m_lock.tryLock(0)) {
		qint64 pid = 0;
		QString host;
		QString name;
		if (m_lock.getLockInfo(&pid, &host, &name))
			m_owner = QStringLiteral("%1, pid %2").arg(name).arg(pid);
		else
			m_owner = QStringLiteral("an instance that left %1 behind")
			            .arg(m_lock_path);
		return false;
	}
	m_primary = true;

	// **A stale lock does not survive to a second run, and that is measured
	// rather than assumed.** `QLockFile::tryLock` reads the pid out of the
	// file, asks the system whether that pid is alive, and compares the
	// executable name so that a recycled pid does not count. That comparison is
	// worth checking rather than assuming, because it is the whole guard: were
	// the name Qt writes the *application* name, "Hydra" would never match the
	// `hydra` that /proc reports and every live owner would be declared stale.
	// It writes the executable name -- the lock file left behind by a run of
	// this program reads `hydra`, not `Hydra`. A `kill -9` while
	// the lock is held therefore leaves a file that the next start removes and
	// replaces: with the owner killed, `tryLock` returned true immediately,
	// with `setStaleLockTime` at 30000 and again at 0, so the age limit above
	// is not what saves us and cannot be tuned into bricking the browser. A
	// live owner was refused in the same probe, which is what makes the
	// passing case mean anything.
	//
	// Holding the lock is also what makes the next line safe: nobody else can
	// be listening, so any socket file here was left by an instance that is
	// gone, and `QLocalServer::listen` refuses a path that already exists.
	if (m_socket_path.isEmpty()) {
		qWarning("no runtime directory; a second instance will be refused "
		          "rather than handed its url");
		return true;
	}
	QLocalServer::removeServer(m_socket_path);
	m_server = new QLocalServer;
	// Belt and braces under a 0700 runtime directory, and the whole defence if
	// Qt ever has to fall back to a shared temporary directory.
	m_server->setSocketOptions(QLocalServer::UserAccessOption);
	if (!m_server->listen(m_socket_path)) {
		qWarning("cannot listen on %s: %s -- a second instance will be "
		          "refused rather than handed its url",
		          qPrintable(m_socket_path),
		          qPrintable(m_server->errorString()));
		delete m_server;
		m_server = nullptr;
		return true;
	}
	// The server is the context object, so the connection dies with it.
	QObject::connect(m_server, &QLocalServer::newConnection, m_server,
	                  [this] { accept_peer(); });
	return true;
}

bool single_instance::hand_over(const QString &message) {
	if (m_socket_path.isEmpty())
		return false;
	QLocalSocket peer;
	// Retried, because losing the lock race is not the same as losing it late:
	// the winner may have taken the lock a millisecond ago and not reached
	// `listen()` yet. Four attempts half a second apart, so a failure here
	// really is "there is nothing listening" rather than "we were early".
	for (int attempt = 0; attempt < k_connect_try; ++attempt) {
		peer.connectToServer(m_socket_path);
		if (peer.waitForConnected(k_connect_ms))
			break;
		peer.abort();
		QThread::msleep(k_retry_ms);
	}
	if (peer.state() != QLocalSocket::ConnectedState)
		return false;

	// The newline is the frame, and it is why an empty message is still a
	// message: without it a bare "come to the front" would put no bytes on the
	// wire, and the reader could not tell it from a peer that connected and
	// said nothing.
	const QByteArray payload = message.toUtf8() + '\n';
	if (payload.size() > k_max_message)
		return false;
	peer.write(payload);
	if (!peer.waitForBytesWritten(k_write_ms))
		return false;
	peer.disconnectFromServer();
	if (peer.state() != QLocalSocket::UnconnectedState)
		peer.waitForDisconnected(k_write_ms);
	return true;
}

void single_instance::on_message(std::function<void(const QString &)> handler) {
	m_handler = std::move(handler);
	const QStringList waiting = m_pending;
	m_pending.clear();
	for (const QString &message : waiting)
		deliver(message);
}

void single_instance::accept_peer() {
	while (QLocalSocket *peer = m_server->nextPendingConnection()) {
		// **Read here rather than on `readyRead`, and block while doing it.**
		// The alternative is a socket, a timer and a lifetime for each peer,
		// to save a wait that only a process running as this user can even
		// start -- the runtime directory is 0700 -- and that is bounded at a
		// quarter of a second whatever it does.
		QByteArray payload;
		QElapsedTimer clock;
		clock.start();
		while (!payload.contains('\n') && payload.size() < k_max_message) {
			const qint64 left = k_read_ms - clock.elapsed();
			if (left <= 0 || !peer->waitForReadyRead(int(left)))
				break;
			payload += peer->read(k_max_message - payload.size());
		}
		peer->disconnectFromServer();
		delete peer;

		const int end = payload.indexOf('\n');
		deliver(QString::fromUtf8(end < 0 ? payload : payload.left(end)));
	}
}

void single_instance::deliver(const QString &message) {
	if (m_handler) {
		m_handler(message);
		return;
	}
	if (m_pending.size() < k_max_pending)
		m_pending << message;
}
