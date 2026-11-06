#include "download_route.h"

#include <QCoreApplication>
#include <QHostInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QNetworkAccessManager>
#include <QNetworkInterface>
#include <QPointer>
#include <QRandomGenerator>
#include <QSocketNotifier>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>

#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
#define HYDRA_ROUTE_BINDS 1
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

// How much is held in memory per direction before the sender is left waiting.
constexpr qint64 relay_buffer = 1 << 20;
constexpr int    dial_timeout_ms = 15000;
constexpr int    watch_interval_ms = 2000;

// SOCKS5 replies, RFC 1928 section 6.
enum : char {
	reply_ok          = 0,
	reply_failure     = 1,
	reply_net         = 3,
	reply_host        = 4,
	reply_refused     = 5,
	reply_command     = 7,
	reply_address     = 8,
};

bool device_up(const QString &name) {
	const QNetworkInterface device = QNetworkInterface::interfaceFromName(name);
	return device.isValid() && device.flags().testFlag(QNetworkInterface::IsUp) &&
	       !device.addressEntries().isEmpty();
}

#ifdef HYDRA_ROUTE_BINDS
// A socket on `device` with connect() under way, or -1 with `error` set.
//
// **Written by hand, because this is the one line the whole feature rests on.**
// The option is set before connect() on a descriptor nothing else has seen, so
// there is no path where a connection exists unbound: if the device is gone or
// the option refused, nothing is dialled at all.
int dial(const QString &device, const QHostAddress &to, quint16 port, int *error) {
	sockaddr_storage addr = {};
	socklen_t        size = 0;
	if (to.protocol() == QAbstractSocket::IPv6Protocol) {
		auto *six = reinterpret_cast<sockaddr_in6 *>(&addr);
		six->sin6_family = AF_INET6;
		six->sin6_port   = htons(port);
		const Q_IPV6ADDR raw = to.toIPv6Address();
		memcpy(&six->sin6_addr, raw.c, sizeof raw.c);
		size = sizeof *six;
	} else {
		auto *four = reinterpret_cast<sockaddr_in *>(&addr);
		four->sin_family      = AF_INET;
		four->sin_port        = htons(port);
		four->sin_addr.s_addr = htonl(to.toIPv4Address());
		size = sizeof *four;
	}
	const int fd = ::socket(addr.ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		*error = errno;
		return -1;
	}
	const QByteArray name = device.toLocal8Bit();
	if (::setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, name.constData(),
	                 socklen_t(name.size())) != 0 ||
	    (::connect(fd, reinterpret_cast<sockaddr *>(&addr), size) != 0 &&
	     errno != EINPROGRESS)) {
		*error = errno;
		::close(fd);
		return -1;
	}
	return fd;
}
#endif

// One client's connection, from the greeting to the end of the relay.
//
// Lives on the route's thread. Nothing here deletes a socket from inside that
// socket's own signal: everything is parented to this and goes with
// deleteLater, the shape `local_proxy` learned the hard way.
class tunnel : public QObject {
public:
	tunnel(QTcpSocket *client, QByteArray user, QByteArray password, QString device,
	       QObject *parent)
	    : QObject(parent), m_down(client), m_user(std::move(user)),
	      m_password(std::move(password)), m_device(std::move(device)) {
		m_down->setParent(this);
		m_down->setReadBufferSize(relay_buffer);
		connect(m_down, &QTcpSocket::readyRead, this, [this] { on_client(); });
		connect(m_down, &QTcpSocket::disconnected, this, [this] { on_closed(m_down, m_up); });
		connect(m_down, &QTcpSocket::bytesWritten, this, [this] { pump(m_up, m_down); });
		// A client that connects and says nothing must not stay for ever.
		m_deadline.setSingleShot(true);
		connect(&m_deadline, &QTimer::timeout, this, [this] { refuse(reply_host); });
		m_deadline.start(dial_timeout_ms);
		on_client();
	}

	~tunnel() override { drop_dial(); }

	void abort() {
		drop_dial();
		if (m_up)
			m_up->abort();
		m_down->abort();
		deleteLater();
	}

private:
	enum class step { greeting, password, request, resolving, dialling, relay, done };

	void on_client() {
		if (m_step == step::relay) {
			pump(m_down, m_up);
			return;
		}
		// Each stage returns false until its whole message has arrived.
		m_in += m_down->readAll();
		while (m_step == step::greeting || m_step == step::password ||
		       m_step == step::request) {
			const step was = m_step;
			if (!advance() || m_step == was)
				break;
		}
	}

	bool advance() {
		const auto byte = [this](int i) { return quint8(m_in.at(i)); };
		if (m_step == step::greeting) {
			if (m_in.size() < 2 || m_in.size() < 2 + byte(1))
				return false;
			if (byte(0) != 5 || !m_in.mid(2, byte(1)).contains(char(2))) {
				m_down->write(QByteArray("\x05\xff", 2));
				finish();
				return false;
			}
			m_in.remove(0, 2 + byte(1));
			m_down->write(QByteArray("\x05\x02", 2));
			m_step = step::password;
			return true;
		}
		if (m_step == step::password) {
			if (m_in.size() < 2)
				return false;
			const int user = byte(1);
			if (m_in.size() < 3 + user || m_in.size() < 3 + user + byte(2 + user))
				return false;
			const int pass = byte(2 + user);
			const bool ok  = byte(0) == 1 && m_in.mid(2, user) == m_user &&
			                 m_in.mid(3 + user, pass) == m_password;
			m_in.remove(0, 3 + user + pass);
			m_down->write(QByteArray(ok ? "\x01\x00" : "\x01\x01", 2));
			if (!ok) {
				finish();
				return false;
			}
			m_step = step::request;
			return true;
		}
		// VER CMD RSV ATYP ADDR PORT
		if (m_in.size() < 5)
			return false;
		const int type = byte(3);
		const int size = type == 1 ? 4 : type == 4 ? 16 : type == 3 ? 1 + byte(4) : -1;
		if (size < 0) {
			refuse(reply_address);
			return false;
		}
		if (m_in.size() < 4 + size + 2)
			return false;
		if (byte(0) != 5 || byte(1) != 1) {
			refuse(reply_command);
			return false;
		}
		const QByteArray host = m_in.mid(4, size);
		m_port = quint16(byte(4 + size) << 8 | byte(5 + size));
		m_in.remove(0, 4 + size + 2);
		if (type == 3) {
			m_step = step::resolving;
			QHostInfo::lookupHost(QString::fromUtf8(host.mid(1)), this,
			                      [this](const QHostInfo &found) {
				m_addresses = found.addresses();
				next_address(reply_host);
			});
			return true;
		}
		m_step = step::resolving;
		m_addresses = { type == 1
		  ? QHostAddress(quint32(quint8(host[0])) << 24 | quint8(host[1]) << 16 |
		                 quint8(host[2]) << 8 | quint8(host[3]))
		  : QHostAddress(reinterpret_cast<const quint8 *>(host.constData())) };
		next_address(reply_host);
		return true;
	}

	void next_address(char reason) {
		drop_dial();
#ifdef HYDRA_ROUTE_BINDS
		while (!m_addresses.isEmpty()) {
			const QHostAddress to = m_addresses.takeFirst();
			int error = 0;
			m_fd = dial(m_device, to, m_port, &error);
			if (m_fd < 0) {
				reason = error == ECONNREFUSED ? reply_refused : reply_net;
				continue;
			}
			m_step = step::dialling;
			m_deadline.start(dial_timeout_ms);
			m_notifier = new QSocketNotifier(m_fd, QSocketNotifier::Write, this);
			connect(m_notifier, &QSocketNotifier::activated, this, [this] { on_dialled(); });
			return;
		}
#endif
		refuse(reason);
	}

	void on_dialled() {
#ifdef HYDRA_ROUTE_BINDS
		int       error = 0;
		socklen_t size  = sizeof error;
		::getsockopt(m_fd, SOL_SOCKET, SO_ERROR, &error, &size);
		if (error != 0) {
			next_address(error == ECONNREFUSED ? reply_refused : reply_host);
			return;
		}
		m_notifier->setEnabled(false);
		m_notifier->deleteLater();
		m_notifier = nullptr;
		m_up = new QTcpSocket(this);
		m_up->setReadBufferSize(relay_buffer);
		if (!m_up->setSocketDescriptor(m_fd, QAbstractSocket::ConnectedState)) {
			refuse(reply_failure);
			return;
		}
		m_fd = -1;
		m_deadline.stop();
		connect(m_up, &QTcpSocket::readyRead, this, [this] { pump(m_up, m_down); });
		connect(m_up, &QTcpSocket::bytesWritten, this, [this] { pump(m_down, m_up); });
		connect(m_up, &QTcpSocket::disconnected, this, [this] { on_closed(m_up, m_down); });
		m_step = step::relay;
		m_down->write(QByteArray("\x05\x00\x00\x01\x00\x00\x00\x00\x00\x00", 10));
		// Whatever the client sent behind its request.
		if (!m_in.isEmpty()) {
			m_up->write(m_in);
			m_in.clear();
		}
		pump(m_down, m_up);
#endif
	}

	void pump(QTcpSocket *from, QTcpSocket *to) {
		if (!from || !to || m_step != step::relay)
			return;
		while (from->bytesAvailable() > 0 && to->bytesToWrite() < relay_buffer) {
			const QByteArray chunk = from->read(64 * 1024);
			if (chunk.isEmpty())
				break;
			to->write(chunk);
		}
		// One side has gone and what it sent has all been passed on.
		if (from->state() == QAbstractSocket::UnconnectedState &&
		    from->bytesAvailable() == 0)
			to->disconnectFromHost();
		if (m_down->state() == QAbstractSocket::UnconnectedState &&
		    (!m_up || m_up->state() == QAbstractSocket::UnconnectedState))
			finish();
	}

	void on_closed(QTcpSocket *from, QTcpSocket *to) {
		if (m_step == step::relay)
			pump(from, to);
		else
			finish();
	}

	void refuse(char reason) {
		if (m_step == step::done)
			return;
		m_down->write(QByteArray("\x05") + reason +
		              QByteArray("\x00\x01\x00\x00\x00\x00\x00\x00", 8));
		finish();
	}

	void finish() {
		if (m_step == step::done)
			return;
		m_step = step::done;
		m_deadline.stop();
		drop_dial();
		// Out before the socket goes: `disconnectFromHost` would send what is
		// waiting, but not from a socket about to be deleted, and a refusal
		// then reaches the client as a connection that simply closed.
		m_down->flush();
		m_down->disconnectFromHost();
		deleteLater();
	}

	void drop_dial() {
		if (m_notifier) {
			m_notifier->setEnabled(false);
			m_notifier->deleteLater();
			m_notifier = nullptr;
		}
#ifdef HYDRA_ROUTE_BINDS
		if (m_fd >= 0)
			::close(m_fd);
#endif
		m_fd = -1;
	}

	QTcpSocket          *m_down = nullptr;
	QTcpSocket          *m_up   = nullptr;
	QSocketNotifier     *m_notifier = nullptr;
	QTimer               m_deadline;
	QByteArray           m_user, m_password, m_in;
	QString              m_device;
	QList<QHostAddress>  m_addresses;
	quint16              m_port = 0;
	int                  m_fd   = -1;
	step                 m_step = step::greeting;
};

// The listening half. Everything in it runs on the route's thread.
class route_server : public QObject {
public:
	route_server(QByteArray user, QByteArray password)
	    : m_user(std::move(user)), m_password(std::move(password)) {}

	quint16 open() {
		m_server = new QTcpServer(this);
		connect(m_server, &QTcpServer::newConnection, this, [this] { on_connection(); });
		// Loopback only: this must never be reachable from the network.
		if (!m_server->listen(QHostAddress::LocalHost, 0))
			return 0;
		m_watch = new QTimer(this);
		m_watch->setInterval(watch_interval_ms);
		connect(m_watch, &QTimer::timeout, this, [this] {
			if (!device_up(m_device))
				close_tunnels();
		});
		return m_server->serverPort();
	}

	void set_device(const QString &device) {
		if (device == m_device)
			return;
		m_device = device;
		close_tunnels();
		if (device.isEmpty())
			m_watch->stop();
		else
			m_watch->start();
	}

private:
	void on_connection() {
		while (QTcpSocket *client = m_server->nextPendingConnection()) {
			if (m_device.isEmpty()) {
				client->abort();
				client->deleteLater();
				continue;
			}
			new tunnel(client, m_user, m_password, m_device, this);
		}
	}

	// **A tunnel that loses its device does not end by itself**: TCP goes on
	// retransmitting into nothing for many minutes, and the download sits
	// there saying it is running. Closing it is what makes the job fail.
	void close_tunnels() {
		const QObjectList all = children();
		for (QObject *child : all)
			if (auto *t = dynamic_cast<tunnel *>(child))
				t->abort();
	}

	QTcpServer *m_server = nullptr;
	QTimer     *m_watch  = nullptr;
	QByteArray  m_user, m_password;
	QString     m_device;
};

QByteArray random_word() {
	QByteArray raw(16, Qt::Uninitialized);
	QRandomGenerator::system()->generate(raw.begin(), raw.end());
	return raw.toHex();
}

}  // namespace

struct download_route::state {
	mutable QMutex lock;
	QString        device;
	QThread       *thread = nullptr;
	route_server  *server = nullptr;
	QByteArray     user, password;
	quint16        port = 0;
};

download_route &download_route::shared() {
	static download_route route;
	return route;
}

download_route::download_route() : m_d(new state) {}

// Never reached with a thread running: `stop` below is a post routine, since a
// function-local static outlives QCoreApplication and QThread must not.
download_route::~download_route() {
	delete m_d;
}

bool download_route::supported() {
#ifdef HYDRA_ROUTE_BINDS
	return true;
#else
	return false;
#endif
}

void download_route::set_interface_name(const QString &name) {
	if (!supported())
		return;
	QMutexLocker hold(&m_d->lock);
	if (name == m_d->device)
		return;
	m_d->device = name;

	if (!m_d->thread && !name.isEmpty()) {
		m_d->user     = random_word();
		m_d->password = random_word();
		m_d->thread   = new QThread;
		m_d->thread->setObjectName("download route");
		m_d->server   = new route_server(m_d->user, m_d->password);
		m_d->server->moveToThread(m_d->thread);
		m_d->thread->start();
		QMetaObject::invokeMethod(m_d->server, [this] { return m_d->server->open(); },
		                          Qt::BlockingQueuedConnection, &m_d->port);
		qAddPostRoutine([] {
			state *d = download_route::shared().m_d;
			QMutexLocker hold(&d->lock);
			if (!d->thread)
				return;
			QMetaObject::invokeMethod(d->server, [d] { delete d->server; },
			                          Qt::BlockingQueuedConnection);
			d->server = nullptr;
			d->thread->quit();
			d->thread->wait();
			delete d->thread;
			d->thread = nullptr;
			d->port   = 0;
		});
	}
	if (route_server *server = m_d->server) {
		const auto tell = [server, name] { server->set_device(name); };
		QMetaObject::invokeMethod(server, tell, Qt::BlockingQueuedConnection);
	}
}

QString download_route::interface_name() const {
	QMutexLocker hold(&m_d->lock);
	return m_d->device;
}

bool download_route::ready(QString *why) const {
	QMutexLocker hold(&m_d->lock);
	if (m_d->device.isEmpty())
		return true;
	QString fault;
	if (!device_up(m_d->device))
		fault = QString("%1 is not connected, so nothing was sent: downloads "
		                "are set to leave by it alone.").arg(m_d->device);
	else if (!m_d->port)
		fault = QString("Could not open the way to %1, so nothing was sent.")
		          .arg(m_d->device);
	if (why)
		*why = fault;
	return fault.isEmpty();
}

QNetworkProxy download_route::proxy() const {
	QMutexLocker hold(&m_d->lock);
	if (m_d->device.isEmpty())
		return {};
	// With a port of zero Qt reports a failure, which is the right outcome:
	// no proxy here would mean the ordinary route.
	return QNetworkProxy(QNetworkProxy::Socks5Proxy, QStringLiteral("127.0.0.1"),
	                     m_d->port, QString::fromLatin1(m_d->user),
	                     QString::fromLatin1(m_d->password));
}

void download_route::apply(QNetworkAccessManager *net) const {
	const QNetworkProxy wanted = proxy();
	if (net->proxy() != wanted)
		net->setProxy(wanted);
}

QString download_route::proxy_url() const {
	QMutexLocker hold(&m_d->lock);
	if (m_d->device.isEmpty())
		return {};
	return QString("socks5h://%1:%2@127.0.0.1:%3")
	  .arg(QString::fromLatin1(m_d->user), QString::fromLatin1(m_d->password))
	  .arg(m_d->port);
}

QList<download_route::choice> download_route::choices() {
	QList<choice> tunnels, others;
	const QList<QNetworkInterface> all = QNetworkInterface::allInterfaces();
	for (const QNetworkInterface &device : all) {
		const auto flags = device.flags();
		if (flags.testFlag(QNetworkInterface::IsLoopBack) ||
		    !flags.testFlag(QNetworkInterface::IsUp) ||
		    device.addressEntries().isEmpty())
			continue;
		// A tunnel has no hardware behind it, and most say so one of these
		// ways. A wrong guess only moves it down the list.
		const QString name = device.name();
		const bool tunnel  = flags.testFlag(QNetworkInterface::IsPointToPoint) ||
		                     name.startsWith("tun") || name.startsWith("tap") ||
		                     name.startsWith("wg") || name.startsWith("ppp") ||
		                     name.startsWith("proton") || name.startsWith("nordlynx") ||
		                     name.startsWith("mullvad");
		(tunnel ? tunnels : others) << choice{ name, tunnel };
	}
	return tunnels + others;
}

QString download_route::listen_on(const QString &device, const QString &listen) {
	QStringList out;
	for (const QString &entry : listen.split(',', Qt::SkipEmptyParts)) {
		// What follows the last colon is the port and any `s` for SSL.
		const QString tail = entry.trimmed().section(':', -1);
		const QString bound = device + ':' + tail;
		if (entry.contains(':') && !out.contains(bound))
			out << bound;
	}
	return out.isEmpty() ? device + ":6881" : out.join(',');
}
