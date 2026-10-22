#pragma once

#include "media_detector.h"

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QString>
#include <QUrl>

// What looking a page up found, kept per page (sec 11.5).
//
// **Asked for by the holder 2026-10-22**: the lookup ran only when somebody
// pressed Ask yt-dlp, and its rows were filed per site -- so a second video
// on the same site listed the first one's streams beside its own, and
// nothing ever took them away. Find Media now looks the page up when it
// opens, and this is what keeps the answer: per page, so another video is
// another entry, and only for as long as it is true.
//
// An answer stops being true when its addresses do. A CDN that signs its
// stream urls says when they lapse -- YouTube's carry `expire=` -- so an
// entry lasts until a few minutes before the first of them; one that says
// nothing is kept an hour. A failure is kept ten minutes, so reopening the
// dialog does not run a lookup that just failed again, and Refresh is the
// way to insist.
struct media_lookup {
	enum class state { running, done, failed };
	state             st = state::running;
	QDateTime         expires;   // invalid while running
	QList<media_item> rows;
	QString           note;      // what was said: the summary, or the error
};

class media_lookups {
public:
	// The page an entry is for: its address without the fragment, which a
	// player rewrites as it seeks and which names the same video.
	static QString key(const QUrl &page);

	// When rows found at `now` stop being worth listing.
	static QDateTime expiry_for(const QList<media_item> &rows, const QDateTime &now);
	static constexpr int k_unsigned_minutes = 60;
	static constexpr int k_failure_minutes  = 10;
	static constexpr int k_margin_minutes   = 5;

	// The entry for a page, or null when there is none or it has lapsed --
	// a lapsed one is dropped here, so it cannot be listed by mistake.
	const media_lookup *find(const QUrl &page, const QDateTime &now);

	void start(const QUrl &page);
	void finish(const QUrl &page, const QList<media_item> &rows, const QString &note,
	            const QDateTime &now);
	void fail(const QUrl &page, const QString &error, const QDateTime &now);
	void forget(const QUrl &page);
	// For "Clear browsing data": these are a record of where somebody went.
	int  clear();

private:
	QHash<QString, media_lookup> m_entries;
};
