#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QUrl>

// What a page is actually feeding its <video> element (architecture doc sec 11.6).
struct mse_stream {
	QString mime;         // "video/mp4;codecs=..." as the player declared it
	qint64  bytes = 0;    // appended so far
	int     appends = 0;
	double  position = 0; // where the element has played to, in seconds
	double  duration = 0; // -1 / 0 when the page has not said
};

// The Media Source tap.
//
// sec 11.1's URL-shaped detection was measured against a real site and finds
// nothing there: the manifest is disguised, and part of the delivery is
// peer-to-peer. But every adaptive player, whatever fetched the bytes, ends up
// pushing them through `SourceBuffer.appendBuffer` -- so a hook on that call
// sees the video when nothing else does. Measured on that site: a
// `video/mp4;codecs=mp4a.40.2,avc1.64001E` buffer, past 5 MB appended, with the
// element reporting real playback positions.
//
// **Two scripts, on purpose.** The hook has to run in the page's own world,
// because an isolated world cannot wrap the page's `MediaSource`. Anything
// there is visible and modifiable by the page, so the main-world half holds
// nothing and grants nothing: it wraps two methods and dispatches a DOM
// CustomEvent. The privileged half -- the QWebChannel bridge -- stays in the
// isolated world where autofill already lives, and only listens for those
// events. The page can therefore lie to us, and everything arriving here is
// treated as a claim rather than a fact; it is page-controlled either way.
//
// This reports *that* a page is playing and what it is playing. Getting the
// bytes out is the next step and belongs on the local proxy (sec 11.6), not on
// this channel.
class mse_tap : public QObject {
	Q_OBJECT
public:
	explicit mse_tap(QObject *parent = nullptr);

	// The main-world hook, and the isolated-world relay that forwards its
	// events. Injected through the two different seam calls of the same name.
	static QString hook_source();
	static QString relay_source();

	// Arms capture for the next load: a one-line main-world script that names
	// the endpoint the hook posts segments to. Injected separately so the hook
	// itself stays constant and carries no per-session state.
	static QString capture_source(const QUrl &endpoint);

	// The object name the relay expects on the bridge.
	static const char *bridge_name() { return "hydraMse"; }

	// What has been seen for a page, newest activity first.
	QList<mse_stream> streams_for(const QString &site_host) const;
	bool active_for(const QString &site_host) const;
	void clear_site(const QString &site_host);

	// Every key held. The hook reports `location.hostname` of the frame it runs
	// in, which on a page whose player is a third-party iframe is *not* the host
	// the shell looks up -- so "nothing here" and "filed under a name nobody
	// asked for" are different answers, and without this they look the same.
	QStringList sites() const;

	// Called from the page. Untrusted input: a hostile page can call these with
	// anything, so nothing here indexes, allocates or trusts on their say-so
	// beyond a bounded record per site.
	Q_INVOKABLE void report(const QString &site_host, const QString &mime,
	                         double bytes, double appends, double position,
	                         double duration);

signals:
	// Coalesced by the shell into the media badge; a page that is playing
	// should not look like a page with nothing on it.
	void site_updated(const QString &site_host);

private:
	QHash<QString, QHash<QString, mse_stream>> m_by_site;   // site -> mime -> stream
};
