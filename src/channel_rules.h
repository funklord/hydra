#pragma once

#include <QDateTime>
#include <QList>
#include <QString>

// **What a correction teaches about a channel.** Asked for by the holder on
// 2026-10-12: a reading that takes the uploader for the artist, when the
// artist is in the title, is corrected once in Recent Readings, and the
// channel's later readings follow the correction rather than repeating the
// mistake.
//
// A rule, not an example: where the artist and title come from, said in
// terms of the video's own title and uploader, so it applies to a title the
// person has never seen. Inferred from one correction by finding the source
// that reproduces it, and shown to the person before it is kept.
struct channel_rule {
	enum class artist_source { keep, uploader, before, after, fixed };
	enum class title_source  { keep, whole, before, after };

	QString       channel;
	artist_source artist = artist_source::keep;
	title_source  title  = title_source::keep;
	QString       separator;      // where `before` and `after` split the title
	QString       fixed_artist;   // for `artist_source::fixed`
	QString       kind;           // a `media_reading` kind name; empty keeps it
	QDateTime     learned;

	bool empty() const {
		return artist == artist_source::keep && title == title_source::keep &&
		       kind.isEmpty();
	}
	// In words, for the person deciding whether to keep it.
	QString describe() const;
};

class channel_rules {
public:
	// The separators a title is tried at, in order: the ones that split
	// "Artist - Track" in the wild.
	static QStringList separators();

	// The rule that reproduces a correction: from the video's own title and
	// uploader, the artist, title and kind the person says are right, and
	// the kind the reading said. Parts it cannot explain are `keep`, so an
	// unexplained title still yields a rule for the artist.
	static channel_rule infer(const QString &channel, const QString &video_title,
	                          const QString &uploader, const QString &artist,
	                          const QString &title, const QString &kind,
	                          const QString &read_kind);

	// The rule applied to a new video. Fills what the rule decides and leaves
	// the rest; false, with `why`, where a part could not be applied -- a
	// title with no separator -- which is then left as read.
	static bool apply(const channel_rule &rule, const QString &video_title,
	                  const QString &uploader, QString *artist, QString *title,
	                  QString *kind, QString *why = nullptr);

	// A title without the tags a reading strips: trailing "(Official Video)",
	// "[HD]" and the like, so a corrected title matches the part it came from.
	static QString without_tags(const QString &title);

	void set(const channel_rule &rule);   // replaces the channel's rule
	const channel_rule *find(const QString &channel) const;
	QList<channel_rule> all() const { return m_rules; }
	int  size() const { return int(m_rules.size()); }
	int  clear_all();

	// The shape every log here has: a file that will not parse is left alone
	// and reported, not treated as empty.
	bool load(const QString &path);
	bool save(const QString &path) const;

private:
	QList<channel_rule> m_rules;
};
