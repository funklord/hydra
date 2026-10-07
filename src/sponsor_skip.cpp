#include "sponsor_skip.h"

#include "policy_engine.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrlQuery>

namespace {

// The community database. Named here rather than spelled into a request, so
// that what this talks to is one line somebody can read.
const char *k_service = "https://sponsor.ajay.app";

// **Overridable for the suite only, and it does not touch the gate.** Without
// this the one property worth asserting -- that the feature being off means no
// request leaves the machine -- could not be asserted at all: a test with no
// network cannot tell a request that was never made from one that failed. With
// it, the suite points this at a local server that counts what it is asked
// for, and zero is zero.
//
// It cannot weaken anything: the policy check happens before the url is built,
// so an environment variable can change where a permitted request goes and
// never whether one is permitted.
QString service_base() {
	const QByteArray from_env = qgetenv("HYDRA_SPONSOR_SERVICE");
	return from_env.isEmpty() ? QString::fromLatin1(k_service)
	                           : QString::fromUtf8(from_env);
}

// A body big enough for every video sharing a prefix and no bigger. Measured
// against nothing: it is a ceiling rather than a size, and the point is that a
// reply which keeps coming is stopped.
constexpr qint64 k_max_bytes = 4 * 1024 * 1024;

}  // namespace

sponsor_skip::sponsor_skip(policy_engine *policy, QObject *parent)
  : QObject(parent), m_policy(policy) {}

QString sponsor_skip::bridge_name() {
	return QStringLiteral("hydraSponsor");
}

QUrl sponsor_skip::query_url(const QString &prefix) {
	QUrl url(service_base() + QStringLiteral("/api/skipSegments/") + prefix);
	// The categories are named in the query so the answer carries only what
	// would be acted on. It is also the honest thing to send: asking for
	// everything and discarding most of it tells the service more about what
	// is wanted than asking for what is wanted.
	QUrlQuery q;
	for (const QString &c : sponsor_segments::skipped_categories())
		q.addQueryItem(QStringLiteral("category"), c);
	url.setQuery(q);
	return url;
}

void sponsor_skip::set_page_host(const QString &host) {
	m_host = host;
}

void sponsor_skip::reply_with(const QString &token,
                               const QList<sponsor_segment> &segs) {
	QJsonArray arr;
	for (const sponsor_segment &s : segs) {
		QJsonObject o;
		o.insert(QStringLiteral("f"), s.from);
		o.insert(QStringLiteral("t"), s.to);
		arr.append(o);
	}
	emit answer(token, QString::fromUtf8(
	  QJsonDocument(arr).toJson(QJsonDocument::Compact)));
}

void sponsor_skip::ask(const QString &token, const QString &url) {
	// **The gate, before anything is built.** An empty answer and no request:
	// the feature being off has to mean nothing goes out, not that nothing is
	// skipped while the query still happens.
	if (!m_policy ||
	    !m_policy->is_allowed(policy::feature::sponsor_skip, m_host)) {
		reply_with(token, {});
		return;
	}
	const QString id = sponsor_segments::video_id(QUrl(url));
	if (id.isEmpty()) {
		// A frame that is not a player. Most frames on most pages.
		reply_with(token, {});
		return;
	}
	const QString prefix = sponsor_segments::hash_prefix(id);
	if (prefix.isEmpty()) {
		reply_with(token, {});
		return;
	}
	// The bound from the header: a page with a dozen frames each naming a
	// different video is not a page with a dozen players.
	if (!m_asked.contains(id) && m_asked.size() >= k_max_videos_per_page) {
		reply_with(token, {});
		return;
	}
	m_asked.insert(id);
	const auto cached = m_cache.constFind(prefix);
	if (cached != m_cache.constEnd()) {
		reply_with(token, sponsor_segments::parse(cached.value(), id));
		return;
	}

	if (!m_net)
		m_net = new QNetworkAccessManager(this);
	QNetworkRequest req(query_url(prefix));
	req.setTransferTimeout(15000);
	req.setMaximumRedirectsAllowed(2);
	req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
	                  QNetworkRequest::NoLessSafeRedirectPolicy);
	req.setHeader(QNetworkRequest::UserAgentHeader,
	               QStringLiteral("hydra/sponsor-segments"));
	// **No cookies and no cache, on a request that is about what somebody is
	// watching.** The default jar would carry whatever the engine has for that
	// host, and a cache entry would leave the prefix on disk; neither is worth
	// anything here, and both are more than the request needs to say.
	req.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
	                  QNetworkRequest::AlwaysNetwork);
	req.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
	req.setAttribute(QNetworkRequest::CookieLoadControlAttribute,
	                  QNetworkRequest::Manual);
	req.setAttribute(QNetworkRequest::CookieSaveControlAttribute,
	                  QNetworkRequest::Manual);

	QNetworkReply *reply = m_net->get(req);
	connect(reply, &QNetworkReply::downloadProgress, reply,
	         [reply](qint64 got, qint64) {
		if (got > k_max_bytes)
			reply->abort();
	});
	connect(reply, &QNetworkReply::finished, this,
	         [this, reply, token, prefix, id] {
		const QByteArray body = reply->readAll();
		const int status =
		  reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const bool ok = reply->error() == QNetworkReply::NoError &&
		                 status == 200;
		reply->deleteLater();
		if (!ok) {
			// **404 is the ordinary answer**, not a failure: it means nobody
			// has submitted anything for any video with that prefix. Either
			// way there is nothing to skip and nothing to say about it, and a
			// status line per video would be noise on every page.
			reply_with(token, {});
			return;
		}
		m_cache.insert(prefix, body);
		reply_with(token, sponsor_segments::parse(body, id));
	});
}

QString sponsor_skip::relay_source() {
	// The main frame only: it has the channel. Everything it does is carry a
	// question up and an answer back down.
	return QStringLiteral(R"JS(
(function () {
  if (window.__hydra_sponsor_relay) return;
  window.__hydra_sponsor_relay = 1;
  var bridge = null;
  var waiting = [];     // [{ win, href, token }]
  var seq = 0;
  var send = function (w) {
    if (!bridge) return;
    while (waiting.length) {
      var q = waiting.shift();
      bridge.ask(q.token, q.href);
    }
  };
  var take = function (win, href) {
    // **The frame says which video, and nothing else.** The site the policy is
    // read against is this window's own, decided in C++ from the view -- a
    // frame does not get to choose the site it speaks for, which is the rule
    // `mse_tap`'s relay states for the same reason.
    var token = 'f' + (++seq);
    waiting.push({ win: win, href: String(href || ''), token: token });
    pending[token] = win;
    send();
  };
  var pending = {};
  window.addEventListener('message', function (e) {
    if (!e || !e.data || !e.data.__hydra_sponsor_ask) return;
    take(e.source, e.data.__hydra_sponsor_ask);
  }, true);
  var start = function () {
    if (!window.hydraChannel) return setTimeout(start, 50);
    window.hydraChannel(function (objs) {
      bridge = objs.hydraSponsor;
      if (!bridge) return;
      bridge.answer.connect(function (token, json) {
        var win = pending[token];
        delete pending[token];
        if (!win) return;
        // Back to the frame that asked, not broadcast: another frame's
        // segments are not this one's, and a wildcard target would hand them
        // to every frame on the page.
        try {
          win.postMessage({ __hydra_sponsor_segs: json }, '*');
        } catch (e) { /* the frame has gone */ }
      });
      send();
    });
  };
  start();
})();
)JS");
}

QString sponsor_skip::frame_source() {
	// Every frame, including the main one. It has the DOM and no channel, so
	// it asks upwards and applies what comes back to its own video.
	return QStringLiteral(R"JS(
(function () {
  if (window.__hydra_sponsor_frame) return;
  window.__hydra_sponsor_frame = 1;
  var segs = null;
  // The rule, and the only copy of it. A quarter-second lead-in because a
  // player reports the position it has reached rather than the one it is
  // about to; without it the skip lands inside the sponsor and is audible.
  // Walking the sorted list with the position advancing is what chains
  // segments that abut: one beginning where another ends is reached later in
  // the same walk, already past the first.
  var apply = function (v) {
    if (!segs || !segs.length || !v) return;
    var t = v.currentTime;
    if (typeof t !== 'number' || !isFinite(t)) return;
    var here = t, to = -1;
    for (var i = 0; i < segs.length; i++) {
      if (here + 0.25 < segs[i].f || here >= segs[i].t) continue;
      here = segs[i].t;
      to = segs[i].t;
    }
    if (to > t) v.currentTime = to;
  };
  var watch = function () {
    var vids = document.getElementsByTagName('video');
    for (var i = 0; i < vids.length; i++) {
      var v = vids[i];
      // Marked on the element, because a player that swaps its video gets a
      // listener on the new one and the old one goes with its mark.
      if (v.__hydraSponsorWatched) continue;
      v.__hydraSponsorWatched = 1;
      v.addEventListener('timeupdate', function (e) { apply(e.target); });
    }
  };
  window.addEventListener('message', function (e) {
    if (!e || !e.data || typeof e.data.__hydra_sponsor_segs !== 'string')
      return;
    try { segs = JSON.parse(e.data.__hydra_sponsor_segs); }
    catch (err) { segs = null; }
    if (!segs || !segs.length) return;
    watch();
    // Only once there is something to skip: a player replaces its video
    // element, and watching the whole document for that is not work worth
    // doing on a page with no segments.
    if (window.MutationObserver && document.documentElement) {
      new window.MutationObserver(watch).observe(document.documentElement,
        { childList: true, subtree: true });
    }
  }, true);
  var ask = function () {
    try {
      window.top.postMessage({ __hydra_sponsor_ask: String(location.href) },
                              '*');
    } catch (e) { /* cross-origin top with no postMessage is not a case */ }
  };
  ask();
})();
)JS");
}
