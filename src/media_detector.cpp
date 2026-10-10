#include "media_detector.h"
#include "policy_engine.h"

#include <QFileInfo>
#include <QMutexLocker>

namespace {

const char *k_direct_ext[] = {
	"mp4", "webm", "mkv", "mov", "avi", "flv",
	"mp3", "m4a", "aac", "ogg", "opus", "flac", "wav",
	"pdf",
};

// A cap so a long autoplay session can't grow the list without bound; the
// detector is a hint surface, not a log.
constexpr int k_max_per_site = 64;

}  // namespace

QString media_mime_for(const QUrl &url) {
	const QString path = url.path().toLower();
	struct { const char *suffix; const char *mime; } table[] = {
		{ ".m3u8", "application/vnd.apple.mpegurl" },
		{ ".mpd",  "application/dash+xml" },
		{ ".mp4",  "video/mp4" },
		{ ".m4v",  "video/mp4" },
		{ ".webm", "video/webm" },
		{ ".mkv",  "video/x-matroska" },
		{ ".ts",   "video/mp2t" },
		{ ".mov",  "video/quicktime" },
		{ ".mp3",  "audio/mpeg" },
		{ ".m4a",  "audio/mp4" },
		{ ".opus", "audio/opus" },
		{ ".flac", "audio/flac" },
	};
	for (const auto &row : table)
		if (path.endsWith(QLatin1String(row.suffix)))
			return QString::fromLatin1(row.mime);
	return QStringLiteral("video/*");
}

media_detector::media_detector(policy_engine *policy, QObject *parent)
  : QObject(parent), m_policy(policy) {
	// **Connected here so that no caller can forget.** The first version of
	// this had the shell call `drop_disallowed()` from its shield handler,
	// which covered the shield and not the settings dialog -- and the settings
	// dialog is where the global default for this setting lives, so turning
	// watching off for everything left every record standing. That is the same
	// defect as the one this whole change is about, reproduced in the fix for
	// it: one rule with two enforcement points, one of them wired.
	//
	// `changed()` is emitted by every mutation the engine has, from wherever it
	// was made, so there is one point and it is this one. It also fires while
	// rules are loaded at startup, where there is nothing recorded yet and the
	// pass is a no-op.
	if (m_policy)
		connect(m_policy, &policy_engine::changed,
		         this, &media_detector::drop_disallowed);
}

media_kind media_detector::classify(const QUrl &url, bool *saveable) {
	*saveable = false;
	const QString path = url.path();
	const QString ext  = QFileInfo(path).suffix().toLower();

	if (ext == "m3u8") { *saveable = true; return media_kind::hls; }
	if (ext == "mpd")  { *saveable = true; return media_kind::dash; }
	// Segments are not offered for saving on their own -- they exist to tell us
	// which manifest is actually playing (sec 11.1).
	if (ext == "ts" || ext == "m4s") return media_kind::segment;

	for (const char *e : k_direct_ext) {
		if (ext == QLatin1String(e)) { *saveable = true; return media_kind::direct; }
	}
	// Not `segment`: a caller that asks "is this a segment" must not be told
	// yes about a stylesheet. See the note on `unknown` in the header.
	return media_kind::unknown;
}

void media_detector::on_request(const request_context &ctx, const request_decision &d) {
	if (d.block || ctx.site_host.isEmpty())
		return;

	// **Asked per request, and per site, which is where the switch lives.**
	// The alternative was to gate at the badge, and that would have been the
	// wrong shape: the detector would still be recording every stream a site
	// served and merely declining to mention them, so turning the setting off
	// would look like privacy and be bookkeeping. Refusing here means nothing
	// is written down in the first place.
	//
	// A detector built without a policy watches everything, which is what a
	// driver or a test that never set one up already expects.
	if (m_policy && !m_policy->is_allowed(policy::feature::media_detect,
	                                       ctx.site_host))
		return;

	bool saveable = false;
	const media_kind kind = classify(ctx.url, &saveable);
	const bool is_segment = (kind == media_kind::segment);
	if (!saveable && !is_segment)
		return;

	int count = 0;
	{
		QMutexLocker guard(&m_lock);
		QList<media_item> &list = m_by_site[ctx.site_host];

		if (is_segment) {
			// Credit the segment to the manifest it most likely belongs to:
			// the one sharing the longest directory prefix. This is what makes
			// "which stream is actually playing" answerable without response
			// bodies.
			const QString dir = ctx.url.adjusted(QUrl::RemoveFilename).toString();
			media_item *best = nullptr;
			int best_len = 0;
			for (media_item &m : list) {
				if (m.kind != media_kind::hls && m.kind != media_kind::dash)
					continue;
				const QString mdir = m.url.adjusted(QUrl::RemoveFilename).toString();
				int i = 0;
				while (i < dir.size() && i < mdir.size() && dir[i] == mdir[i]) ++i;
				if (i > best_len) { best_len = i; best = &m; }
			}
			if (!best)
				return;         // a segment with no known manifest tells us nothing yet
			best->hits++;
			count = list.size();
		} else {
			for (const media_item &m : list)
				if (m.url == ctx.url)
					return;     // already known
			if (list.size() >= k_max_per_site)
				return;
			media_item item;
			item.kind      = kind;
			item.url       = ctx.url;
			item.site_host = ctx.site_host;
			// Not just the filename: variant manifests are routinely all named
			// master.m3u8 or index.mpd, and a list of identical labels is
			// useless for choosing between them. Keep the parent directory,
			// which is what actually distinguishes them (/hd/, /720p/...).
			const QStringList segs =
			  ctx.url.path().split('/', Qt::SkipEmptyParts);
			if (segs.size() >= 2)
				item.label = segs.at(segs.size() - 2) + "/" + segs.last();
			else if (!segs.isEmpty())
				item.label = segs.last();
			else
				item.label = ctx.url.host();
			list.push_back(item);
			count = list.size();
		}
	}
	emit site_updated(ctx.site_host, count);
}

QList<media_item> media_detector::items_for(const QString &site_host) const {
	QMutexLocker guard(&m_lock);
	QList<media_item> list = m_by_site.value(site_host);
	// Manifests before direct files, and within manifests the most-fetched
	// first -- that is the heuristic for "the stream the page is playing".
	std::sort(list.begin(), list.end(), [](const media_item &a, const media_item &b) {
		const bool am = (a.kind == media_kind::hls || a.kind == media_kind::dash);
		const bool bm = (b.kind == media_kind::hls || b.kind == media_kind::dash);
		if (am != bm) return am;
		if (a.hits != b.hits) return a.hits > b.hits;
		return a.label < b.label;
	});
	return list;
}

int media_detector::count_for(const QString &site_host) const {
	QMutexLocker guard(&m_lock);
	return m_by_site.value(site_host).size();
}

media_item media_detector::primary_for(const QString &site_host) const {
	const QList<media_item> list = items_for(site_host);
	return list.isEmpty() ? media_item{} : list.first();
}

void media_detector::add_item(const QString &site_host, const media_item &item) {
	{
		QMutexLocker lock(&m_lock);
		QList<media_item> &list = m_by_site[site_host];
		for (const media_item &existing : list)
			if (existing.url == item.url)
				return;
		list.prepend(item);   // an authoritative answer belongs at the top
	}
	emit site_updated(site_host, count_for(site_host));
}

int media_detector::clear_all() {
	QStringList sites;
	{
		QMutexLocker guard(&m_lock);
		sites = m_by_site.keys();
		m_by_site.clear();
	}
	// **Outside the lock, and one per site.** The badge is driven by this
	// signal, so a clear that emitted nothing would empty the list and leave
	// the number over it saying there were seven.
	for (const QString &s : sites)
		emit site_updated(s, 0);
	return int(sites.size());
}

void media_detector::clear_site(const QString &site_host) {
	{
		QMutexLocker guard(&m_lock);
		m_by_site.remove(site_host);
	}
	// **Emitted, which this did not do.** `clear_all` says why a clear has to
	// announce itself -- the badge is driven by this signal, so emptying the
	// list silently leaves the number over it saying there were seven -- and
	// this function was written without it. It had no caller anywhere, so
	// nothing had ever shown the fault; the two defects hid each other.
	emit site_updated(site_host, 0);
}

int media_detector::drop_disallowed() {
	if (!m_policy)
		return 0;
	// **Every site, not only the one in front.** A per-site switch can be
	// changed from the shield for a tab that is not current, and a record left
	// standing for a site whose watching has just been turned off is the thing
	// this exists to remove. The map holds one entry per site visited this
	// session, so walking it is cheaper than deciding which entry to look at.
	QStringList dropped;
	{
		QMutexLocker guard(&m_lock);
		for (auto it = m_by_site.begin(); it != m_by_site.end();) {
			if (m_policy->is_allowed(policy::feature::media_detect, it.key())) {
				++it;
				continue;
			}
			dropped << it.key();
			it = m_by_site.erase(it);
		}
	}
	for (const QString &s : dropped)
		emit site_updated(s, 0);
	return int(dropped.size());
}

namespace {

bool is_youtube_host(const QString &host) {
	const QString h = host.toLower();
	for (const char *d : { "youtube.com", "youtu.be", "youtube-nocookie.com",
	                       "googlevideo.com" }) {
		const QString base = QString::fromLatin1(d);
		if (h == base || h.endsWith(QLatin1Char('.') + base))
			return true;
	}
	return false;
}

}  // namespace

bool media_saving_allowed_on(bool android, const QString &site_host,
                             const QUrl &media_url, QString *why) {
	if (!android)
		return true;
	if (!is_youtube_host(site_host) && !is_youtube_host(media_url.host()))
		return true;
	if (why)
		*why = QStringLiteral("Saving from YouTube is not available on Android: "
		                      "YouTube's terms forbid downloading, and Google "
		                      "Play does not allow apps that break a service's "
		                      "terms.");
	return false;
}

