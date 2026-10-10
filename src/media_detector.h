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

	// The name to save it as, when something knew better than the url --
	// yt-dlp's metadata (`ytdlp_resolver::file_name_for`). Empty: the url's.
	QString file_name;
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
	// it from this thread is safe because `policy_engine::effective_setting`
	// takes a read lock, which is the same licence `request_filter` runs under
	// from the same callback.
	//
	// **This cited a different reason and the reason was wrong.** It said the
	// rule set "tolerates a stale snapshot" -- quoting a sentence in
	// `policy_engine.h` that argued about when a value was written where the
	// question was a span of memory, and under which this read was a
	// use-after-free waiting for the UI thread to add a rule. The read here was
	// never the defect; the licence it was granted under was.
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

	// Forget the sites whose watching has been turned off, and say how many.
	//
	// **The setting's own words were a promise nothing kept.** "Turning it off
	// empties the media badge here" is what the shield says about
	// Auto-detect media, and `on_request` refusing to record anything new only
	// half delivers it: everything found before the switch was flipped stayed
	// in the list, stayed on the badge, and stayed offerable to Save and Watch.
	// So a person who turned watching off for a site went on being shown what
	// had been watched.
	//
	// The policy lives here already, so the question is asked where the records
	// are rather than by a caller that would have to enumerate both -- and the
	// constructor connects it to the engine's `changed()`, so an edit made from
	// anywhere reaches it and no caller has to remember. Public and returning a
	// count so a test can call it directly and read what it did.
	int drop_disallowed();

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

// **Whether this build may save media from a site.** False in one case: on
// Android, for YouTube -- the page or the media itself. Set by the copyright
// holder on 2026-10-10, conditional on the rule being real; it is.
//
// Google Play's Device and Network Abuse policy, read 2026-10-10 at
// https://support.google.com/googleplay/android-developer/answer/9888379,
// lists among its "Examples of common Device and Network Abuse violations":
//
//     Apps that access or use a service or API in a manner that violates
//     its terms of service.
//
// and YouTube's Terms of Service (effective December 15, 2023), under
// "Permissions and Restrictions", say "You are not allowed to":
//
//     access, reproduce, download, distribute, transmit, broadcast,
//     display, sell, license, alter, modify or otherwise use any part of
//     the Service or any Content except: (a) as expressly authorized by the
//     Service; or (b) with prior written permission from YouTube and, if
//     applicable, the respective rights holders;
//
// So a Play build saves nothing from YouTube: not a detected stream, not an
// assembled one, not a capture, and yt-dlp is not asked. Desktop is not a
// Play build and is unchanged. Watching in an external player is not
// saving, and is left as it was -- recorded in project.md as the holder's to
// decide, since the same terms reach it.
//
// `android` is a parameter so a desktop test can ask the Android question;
// `media_saving_allowed` answers for the build it is in.
bool media_saving_allowed_on(bool android, const QString &site_host,
                             const QUrl &media_url, QString *why = nullptr);
inline bool media_saving_allowed(const QString &site_host, const QUrl &media_url,
                                 QString *why = nullptr) {
#ifdef Q_OS_ANDROID
	return media_saving_allowed_on(true, site_host, media_url, why);
#else
	return media_saving_allowed_on(false, site_host, media_url, why);
#endif
}

