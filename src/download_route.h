#pragma once

#include <QList>
#include <QNetworkProxy>
#include <QString>

class QNetworkAccessManager;

// Which connection downloads leave by (architecture doc sec 11.4).
//
// **Hydra ships no VPN, and this is not one.** A person who has a VPN up on
// the system can name its interface, and from then on downloads go through
// that interface and nothing else, while browsing stays on the ordinary
// route. The same device name is what qBittorrent's "network interface" takes.
// If the device goes away downloads stop; they never go out another way.
//
// **Why a proxy.** libtorrent binds its own sockets, but QNetworkAccessManager
// offers no way to name an interface, and yt-dlp takes only a source address,
// which steers nothing where the tunnel is not the default route. So this runs
// a SOCKS5 server on loopback, and it is the one place that dials: each socket
// gets SO_BINDTODEVICE before connect(), which an ordinary user could do when
// measured on Linux 6.12. Everything that fetches for a download then only has
// to be pointed at this.
//
// **Password required, because any local program can find the port.** Without
// it the server would hand anything on this machine a way into the tunnel.
//
// **Its own thread**, because every byte of a bound download passes through
// here and the window's thread stalls for reasons of its own.
//
// Not on Android: an app may not bind a device there, and the system's own
// settings are where a VPN is limited to certain apps.
class download_route {
public:
	static download_route &shared();

	static bool supported();

	// A device name, or empty for the ordinary route. Open tunnels are closed
	// when it changes: a transfer left running would still be on the old one.
	void    set_interface_name(const QString &name);
	QString interface_name() const;
	bool    bound() const { return !interface_name().isEmpty(); }

	// True when a download may start: nothing is bound, or the device is up
	// and the server listening. `why` is a sentence for the person.
	bool ready(QString *why = nullptr) const;

	// The proxy a fetch must use. An ordinary QNetworkProxy with no type set
	// when nothing is bound, which leaves the application's own behaviour.
	QNetworkProxy proxy() const;
	void          apply(QNetworkAccessManager *net) const;

	// socks5h://user:password@127.0.0.1:port for a child process. Empty when
	// nothing is bound. It holds the password, so it goes in the environment
	// and never in an argument, which any user can read from /proc.
	QString proxy_url() const;

	// What a person can choose from: every interface with an address except
	// loopback, tunnels first.
	struct choice {
		QString name;
		bool    tunnel = false;
	};
	static QList<choice> choices();

	// `listen` for libtorrent with every host replaced by `device`, ports kept.
	static QString listen_on(const QString &device, const QString &listen);

private:
	download_route();
	~download_route();
	download_route(const download_route &)            = delete;
	download_route &operator=(const download_route &) = delete;

	struct state;
	state *m_d;
};
