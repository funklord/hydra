#pragma once

#include "request_filter.h"

#include <QHash>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QMap>
#include <QUrl>

// What kind of saveable thing a URL looks like (architecture doc sec 11.1).
enum class media_kind {
	direct,      // a file: .mp4/.webm/.mkv/.mp3/.m4a/.pdf ...
	hls,         // an .m3u8 manifest
	dash,        // an .mpd manifest
	segment,     // .ts / .m4s -- betrays a manifest we may not have seen
	// **Everything else, and it needs its own value.** This used to come back
	// as `segment` with the saveable flag false, on the reasoning that the
	// caller checks the flag. `on_request` checks it and then asks a second
	// question -- "is this a segment?" -- to which the answer was yes for every
	// script, image, font and beacon on the page. Each of them was credited to
	// the nearest manifest, and `hits` is what decides which stream is the
	// primary one, so the stream handed to a player was chosen partly by how
	// much unrelated traffic happened to share its directory.
	unknown,
};

struct media_item {
	media_kind kind = media_kind::direct;
	QUrl       url;
	QString    site_host;   // the page it was found on
	QString    label;       // filename or manifest name, for the list
	int        hits = 0;    // segment counts tell us which stream is playing

	// What this particular stream needs sent with it. Empty for anything found
	// by watching request shapes -- those are replayed with the page's own
	// context -- and filled by a learned extractor (sec 11.5), which is asked for
	// them precisely because a CDN answers 403 without them (sec 11.3).
	QMap<QString, QString> headers;
};

// The media type to hand a system player along with the URL.
//
// Android's `ACTION_VIEW` needs one: with no type, the chooser offers whatever
// claims the scheme -- a browser, most often, which would hand the stream back to
// us in a loop. With a video type it offers video players. There is no
// equivalent on the desktop, where the player is named outright.
//
// From the url alone, like the rest of sec 11.1's detection, and wrong the same way
// a url can be: a `.mp4` that is really something else is a lie the server tells
// and the player will discover. Anything unrecognised gets `video/*`, which asks
// for a video player without claiming to know the container.
QString media_mime_for(const QUrl &url);

// The first interceptor consumer (architecture doc sec 10/sec 11): it watches the
// request stream and classifies anything saveable, grouped by the page that
// requested it.
//
// Detection is URL-shaped only. The interceptor sees requests, not responses,
// so real Content-Types and manifest bodies are not available here -- that is
// the optional local proxy's job (sec 10), and this degrades to extension and
// path heuristics without it. Obfuscated manifests are still betrayed by their
// segment requests, which is why segments are tracked at all.
//
// Thread note: on_request() arrives off the UI thread; everything here is
// guarded by a mutex and the UI reads snapshots.
class policy_engine;

class media_detector : public QObject, public request_observer {
	Q_OBJECT
public:
	// **The policy engine is consulted per request and is not owned.** Reading
	// it from this thread is what `policy_engine` already documents as safe --
	// the rule set is mutated only on the UI thread and reads tolerate a stale
	// snapshot, which is the same licence `request_filter` runs under from the
	// same callback.
	//
	// Optional, so a driver or a test can build a detector with no policy at
	// all and get the old behaviour: with none, everything is watched.
	explicit media_detector(policy_engine *policy = nullptr,
	                         QObject *parent = nullptr);

	void on_request(const request_context &ctx, const request_decision &d) override;

	// Snapshot for a page, best candidate first.
	QList<media_item> items_for(const QString &site_host) const;
	int count_for(const QString &site_host) const;

	// The primary stream: the manifest whose segments are actively being
	// fetched, else the first manifest, else the first direct file (sec 11.3).
	media_item primary_for(const QString &site_host) const;

	void clear_site(const QString &site_host);
	// Forget every site at once, for "Clear browsing data".
	//
	// **This is a browsing record and it was not being cleared.** The shell's
	// clear dropped cookies, the cache, session permission answers and the
	// proxy's publications, and left this behind for the life of the process
	// -- which is the moment a person has just said they want the browser to
	// forget where they have been.
	//
	// Returns how many sites were dropped, so the caller can say what it did
	// rather than claiming a clear it cannot see the size of.
	int clear_all();


	// Add something found by other means -- the yt-dlp handoff (sec 11.5), which
	// resolves a page authoritatively rather than guessing from URL shape.
	// Deduplicated by URL, so asking twice does not double the list.
	void add_item(const QString &site_host, const media_item &item);

	static media_kind classify(const QUrl &url, bool *saveable);

signals:
	// Queued to the UI thread by Qt because the emit happens off it.
	void site_updated(const QString &site_host, int count);

private:
	policy_engine *m_policy = nullptr;   // not owned; may be null
	mutable QMutex m_lock;
	QHash<QString, QList<media_item>> m_by_site;
};
