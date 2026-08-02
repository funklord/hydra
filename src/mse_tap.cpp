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
    try {
      document.dispatchEvent(new CustomEvent('hydra-mse', { detail: {
        mime: mime,
        bytes: s.bytes,
        appends: s.appends,
        position: v && isFinite(v.currentTime) ? v.currentTime : 0,
        duration: v && isFinite(v.duration) ? v.duration : 0
      }}));
    } catch (e) { /* a page may have replaced CustomEvent; nothing to do */ }
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
  document.addEventListener('hydra-mse', function (e) {
    if (!e || !e.detail) return;
    pending.push(e.detail);
    if (pending.length > 64) pending.shift();   // bound a hostile page
    flush();
  }, true);
  var start = function () {
    if (typeof QWebChannel === 'undefined' || !window.qt || !qt.webChannelTransport)
      return setTimeout(start, 200);
    new QWebChannel(qt.webChannelTransport, function (ch) {
      bridge = ch.objects.hydraMse;
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
