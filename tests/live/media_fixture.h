// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A local page that looks enough like a video site to exercise the media path.
//
// **Three drivers used to point at a real one by default.** `try_media`,
// `try_frame` and `try_mse` each carried a `dramafren.org` url as the value
// they used when given no argument, so running the sweep fetched a live
// ad-serving site -- announcing the machine to it, and pulling in whatever it
// served that day. That is how two `fedoq.com/clicks/...` tracking urls ended
// up written into the committed example tree.
//
// It also made those drivers unrepeatable in the way that matters: the thing
// they measured changed between runs for reasons nothing here controls, so a
// difference in output was as likely to be the site's week as the code's.
//
// So the default is this, and a real url is what you pass when a real site is
// the question. `try_extract` already worked that way and is why the shape was
// obvious.
//
// **What it is not.** It is a fixture, not a video site: the segments are a few
// bytes of nothing and the player does not play. What it produces is the
// *shape* the code reads -- a manifest request, segment requests under it, a
// player in an iframe, and a MediaSource that is opened and appended to. Where
// the question is whether a real site's obfuscation defeats the detector, only
// a real site answers it, and that is what the argument is for.
#include <QByteArray>
#include <QHostAddress>
#include <QStringList>
#include <QString>
#include <QTcpServer>
#include <QTcpSocket>

namespace media_fixture {

// Raw literals rather than concatenated lines: a continuation inside a string
// is still a continuation to the indentation gate, and HTML reads better
// unbroken anyway.
// A 1x1 transparent GIF, as hex, so no line here is a continuation.
inline const char *k_gif =
	"47494638396101000100800000000000ffffff21"
	"f90401000000002c00000000010001000002024401003b";

inline const char *k_manifest = R"M3U(#EXTM3U
#EXT-X-VERSION:3
#EXT-X-TARGETDURATION:4
#EXTINF:4.0,
seg-1.ts
#EXTINF:4.0,
seg-2.ts
#EXT-X-ENDLIST
)M3U";

inline const char *k_player = R"HTML(<!doctype html><title>player</title>
<video id=v controls muted></video>
<script>fetch('/stream.m3u8').then(function (r) { return r.text(); });</script>
)HTML";

inline const char *k_page = R"HTML(<!doctype html><title>fixture</title>
<h1>local media fixture</h1>
<iframe src="/player" width=320 height=180></iframe>
<video id=v controls muted></video>
<!-- Third-party and ad-shaped, so filter evolution has something to reason
     from. THIRD_PARTY is rewritten to 127.0.0.2 with this server's port
     before the page is served: a different host, still loopback. -->
<img src="THIRD_PARTY/ads/banner.gif" width=1 height=1>
<img src="THIRD_PARTY/analytics/beacon.gif" width=1 height=1>
<img src="THIRD_PARTY/track/pixel.gif" width=1 height=1>
<script src="THIRD_PARTY/adservice/loader.js"></script>
<script>
var v = document.getElementById('v');
if (window.MediaSource) {
  var ms = new MediaSource();
  v.src = URL.createObjectURL(ms);
  ms.addEventListener('sourceopen', function () {
    var sb;
    try { sb = ms.addSourceBuffer('video/mp4; codecs="avc1.42E01E"'); }
    catch (e) { return; }
    fetch('/seg-1.ts').then(function (r) { return r.arrayBuffer(); })
      .then(function (b) { try { sb.appendBuffer(b); } catch (e) {} });
  });
}
</script>
)HTML";

// The paths that stand in for somebody else's advertising. Kept as a
// function so the branch below is one line: a continuation inside an
// `if` is what the indentation gate counts.
inline bool is_ad_shaped(const QByteArray &path) {
	if (path.startsWith("/ads/"))
		return true;
	if (path.startsWith("/analytics/"))
		return true;
	return path.startsWith("/track/");
}

// Serves four things, each the smallest thing that carries its shape.
class server : public QTcpServer {
public:
	// The base url, once listening. Empty when it could not.
	//
	// **AnyIPv4, not LocalHost**, so the same server answers on 127.0.0.1 and
	// 127.0.0.2. Both are loopback and nothing leaves the machine, and they are
	// *different hosts* -- which is the only way this fixture can produce a
	// third-party request. `filter_signals::looks_ad_shaped` refuses anything
	// first-party outright, on the grounds that proposing a rule against the
	// page's own host is how a filter list breaks the page it was meant to fix.
	// `try_cookies` uses the same two addresses for the same reason.
	QString start() {
		if (!listen(QHostAddress::AnyIPv4, 0))
			return QString();
		return QStringLiteral("http://127.0.0.1:%1/").arg(serverPort());
	}

	// Where the fixture's "third party" lives, for a driver that wants to name
	// it in output.
	QString third_party() const {
		return QStringLiteral("http://127.0.0.2:%1/").arg(serverPort());
	}

	// Every path this served, for a driver that wants to say what was asked
	// for rather than trusting that it was.
	QStringList seen;

protected:
	void incomingConnection(qintptr fd) override {
		auto *s = new QTcpSocket(this);
		s->setSocketDescriptor(fd);
		QObject::connect(s, &QTcpSocket::readyRead, s, [this, s] {
			const QByteArray head = s->readAll();
			const int start = head.indexOf(' ') + 1;
			const QByteArray path = head.mid(start, head.indexOf(' ', start) - start);
			seen << QString::fromLatin1(path);
			reply(s, path);
		});
	}

private:
	void reply(QTcpSocket *s, const QByteArray &path) {
		QByteArray type = "text/html", body;

		if (path.startsWith("/stream.m3u8")) {
			// The shape the detector reads: a master naming segments under
			// itself. Not a valid stream, and it does not need to be.
			type = "application/vnd.apple.mpegurl";
			body = k_manifest;
		} else if (path.startsWith("/seg-")) {
			type = "video/mp2t";
			body = QByteArray(2048, '\0');
		} else if (is_ad_shaped(path)) {
			type = "image/gif";
			body = QByteArray::fromHex(k_gif);
		} else if (path.startsWith("/adservice/")) {
			type = "application/javascript";
			body = "/* fixture ad loader */";
		} else if (path.startsWith("/player")) {
			// What `try_frame` loads in a plain view: a page whose whole
			// content is a player, the way a mirror's iframe is.
			body = k_player;
		} else {
			// The page a person would open: a player in an iframe, and a
			// MediaSource opened and appended to, which is what the tap hooks.
			body = k_page;
			// The one substitution this fixture makes: the third-party host
			// cannot be a literal, because the port is only known once the
			// server is listening.
			body.replace("THIRD_PARTY", third_party().toUtf8().chopped(1));
		}

		// Built by appending rather than as one expression: a continuation
		// line is what the indentation gate counts, and there is no reason for
		// this to have any.
		QByteArray resp = "HTTP/1.1 200 OK\r\nContent-Type: ";
		resp += type;
		resp += "\r\nCache-Control: no-store";
		resp += "\r\nAccept-Ranges: bytes";
		resp += "\r\nContent-Length: " + QByteArray::number(body.size());
		resp += "\r\nConnection: close\r\n\r\n";
		resp += body;
		s->write(resp);
		s->flush();
		s->disconnectFromHost();
	}
};

}  // namespace media_fixture
