// SPDX-License-Identifier: GPL-3.0-or-later
#include "mse_tap.h"

#include <QJsonArray>
#include <QJsonDocument>

#include <algorithm>

namespace {

// Runs in the page's own world. Deliberately small and dull: it wraps two
// methods, counts bytes, and dispatches a DOM event. No bridge, no tokens,
// nothing worth taking, nothing to escalate with.
const char *k_hook = R"JS(
(function () {
  if (window.__hydra_mse) return;
  window.__hydra_mse = 1;

  var seen = {};
  var send = function (mime) {
    var s = seen[mime];
    var v = document.querySelector('video');
    var detail = {
      mime: mime,
      bytes: s.bytes,
      appends: s.appends,
      position: v && isFinite(v.currentTime) ? v.currentTime : 0,
      duration: v && isFinite(v.duration) ? v.duration : 0
    };
    // In the top frame the relay is in the isolated world of this same
    // document, and a DOM event reaches it.
    //
    // In a subframe there is no relay: Qt installs `qt.webChannelTransport` in
    // the main frame alone, so one injected into an iframe would have nothing
    // to connect a QWebChannel to. A DOM event dispatched here is heard by
    // nobody. So a subframe hands its report up to the frame that does have a
    // relay -- which is the whole difference between the tap working and the
    // tap being silent on every page whose player is an embed, and that is most
    // of them.
    try {
      if (window.top !== window)
        window.top.postMessage({ __hydra_mse: detail }, '*');
      else
        document.dispatchEvent(new CustomEvent('hydra-mse', { detail: detail }));
    } catch (e) { /* a page may have replaced either; nothing to do */ }
  };

  if (window.MediaSource && MediaSource.prototype.addSourceBuffer) {
    var add = MediaSource.prototype.addSourceBuffer;
    MediaSource.prototype.addSourceBuffer = function (mime) {
      var sb = add.apply(this, arguments);
      if (!seen[mime]) seen[mime] = { bytes: 0, appends: 0, last: 0 };
      send(mime);
      var append = sb.appendBuffer;
      sb.appendBuffer = function (buf) {
        var n = 0;
        if (buf) n = (buf.byteLength !== undefined) ? buf.byteLength : (buf.length || 0);
        var s = seen[mime];
        s.bytes += n;
        s.appends += 1;
        // If capture is armed, the same bytes go to the shell. Order matters:
        // this is the *first* buffer of this mime, so an init segment reaches
        // the file before any media segment does, and the result is playable.
        if (window.__hydra_capture && n > 0 && !s.skip) {
          try {
            var copy = buf.buffer ? buf.buffer.slice(buf.byteOffset || 0,
                                                     (buf.byteOffset || 0) + n)
                                  : buf;
            fetch(window.__hydra_capture, { method: 'POST', body: copy,
                                            mode: 'cors', keepalive: false })
              .catch(function () { s.skip = true; });
          } catch (e) { s.skip = true; }
        }
        // Report on a byte threshold rather than per append: a player appends
        // constantly, and the shell only needs to know roughly how much is
        // buffered.
        if (s.bytes - s.last >= 262144) { s.last = s.bytes; send(mime); }
        return append.apply(this, arguments);
      };
      return sb;
    };
  }
})();
)JS";

// Runs in the isolated world, where the bridge is. Its only job is to carry
// events across; it never touches the page's objects.
const char *k_relay = R"JS(
(function () {
  if (window.__hydra_mse_relay) return;
  window.__hydra_mse_relay = 1;
  var bridge = null;
  var pending = [];
  var flush = function () {
    if (!bridge) return;
    while (pending.length) {
      var d = pending.shift();
      bridge.report(location.hostname, String(d.mime || ''),
                    Number(d.bytes) || 0, Number(d.appends) || 0,
                    Number(d.position) || 0, Number(d.duration) || 0);
    }
  };
  var take = function (d) {
    if (!d) return;
    pending.push(d);
    if (pending.length > 64) pending.shift();   // bound a hostile page
    flush();
  };
  document.addEventListener('hydra-mse', function (e) {
    take(e && e.detail);
  }, true);
  // The same report, handed up by a player in a subframe. Any window can post
  // here, so this is exactly as trustworthy as the DOM event above -- which is
  // to say not at all, and it gets the same treatment: a claim about a mime and
  // a byte count, length-capped and bounded per site on the C++ side.
  //
  // What is *not* taken from the message is the name it gets filed under. That
  // stays `location.hostname` of this, the top frame: the page the user is on,
  // which is also the name the shell looks the tap up by. A frame does not get
  // to choose the site it speaks for, for the same reason §13.2 does not let a
  // page choose the origin it asks for credentials about.
  window.addEventListener('message', function (e) {
    if (e && e.data && e.data.__hydra_mse) take(e.data.__hydra_mse);
  }, true);
  // The page's one channel, not a second one: see the bootstrap in
  // qtwebengine_view.cpp for what building your own costs.
  var start = function () {
    if (!window.hydraChannel) return setTimeout(start, 50);
    window.hydraChannel(function (objs) {
      bridge = objs.hydraMse;
      flush();
    });
  };
  start();
})();
)JS";

}  // namespace

mse_tap::mse_tap(QObject *parent) : QObject(parent) {}

QString mse_tap::hook_source()  { return QString::fromUtf8(k_hook); }
QString mse_tap::relay_source() { return QString::fromUtf8(k_relay); }

QString mse_tap::capture_source(const QUrl &endpoint) {
	// Only a loopback URL is ever handed to a page. Belt and braces: the caller
	// is the shell, but this is the one value that crosses into the page's own
	// world and it should be impossible to point somewhere else by accident.
	if (endpoint.host() != "127.0.0.1")
		return QStringLiteral("/* refused: capture endpoint is not loopback */");
	return QString("window.__hydra_capture = %1;")
	    .arg(QString::fromUtf8(QJsonDocument(QJsonArray{ endpoint.toString() })
	                                .toJson(QJsonDocument::Compact))
	             .mid(1)      // strip the array brackets, leaving the quoted string
	             .chopped(1));
}

void mse_tap::report(const QString &site_host, const QString &mime, double bytes,
                      double appends, double position, double duration) {
	// Everything here came from a page and is a claim, not a fact. Clamp it and
	// bound what one site can occupy; a hostile page calling this in a loop
	// with random mimes must not be able to grow this map without limit.
	const QString site = site_host.left(255);
	if (site.isEmpty())
		return;
	QHash<QString, mse_stream> &per_mime = m_by_site[site];
	const QString key = mime.isEmpty() ? QStringLiteral("(unknown)") : mime.left(120);
	if (!per_mime.contains(key) && per_mime.size() >= 8)
		return;

	mse_stream &s = per_mime[key];
	s.mime     = key;
	s.bytes    = qMax<qint64>(0, qint64(bytes));
	s.appends  = qMax(0, int(appends));
	s.position = position > 0 ? position : 0;
	s.duration = duration > 0 ? duration : 0;
	emit site_updated(site);
}

QList<mse_stream> mse_tap::streams_for(const QString &site_host) const {
	QList<mse_stream> out = m_by_site.value(site_host).values();
	std::sort(out.begin(), out.end(), [](const mse_stream &a, const mse_stream &b) {
		return a.bytes > b.bytes;
	});
	return out;
}

bool mse_tap::active_for(const QString &site_host) const {
	for (const mse_stream &s : m_by_site.value(site_host))
		if (s.bytes > 0)
			return true;
	return false;
}

void mse_tap::clear_site(const QString &site_host) {
	m_by_site.remove(site_host);
}

QStringList mse_tap::sites() const {
	QStringList out = m_by_site.keys();
	out.sort();
	return out;
}
