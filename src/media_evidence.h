#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

// **Everything a site says about a video, collected without judging any of
// it.** The first of the three steps the holder agreed on 2026-10-10 for
// saving media by what it is: evidence, then a weak model's interpretation
// checked against this evidence, then the person's corrections remembered.
//
// Why no judgement here: the holder pointed out that the "facts" a first
// version of this turned into rules -- the uploader is the artist, the title
// is the track, a long video with chapters is a set -- each have exceptions,
// and telling which applies is interpretation. So this step only gathers
// and extracts; it never decides who the artist is or which tracklist is the
// right one. Those are the model's, and every answer it gives is checked
// against what is here.
struct media_evidence {
	// --- what the site states ---------------------------------------------
	QString     id;
	QString     title;
	QString     uploader;
	QString     uploader_id;
	QString     channel;
	QString     artist;          // only where the site has music metadata
	QString     track;
	QString     album;
	QString     series;
	QStringList categories;
	QStringList tags;
	double      duration = -1;   // seconds
	int         width = 0;
	int         height = 0;
	qint64      timestamp = -1;  // upload, seconds since the epoch, UTC
	QString     upload_date;     // YYYYMMDD
	int         release_year = -1;
	QString     live_status;     // "not_live", "was_live", ...
	QString     description;     // capped at k_description_cap characters

	struct chapter {
		double  start = 0;
		QString title;
	};
	QList<chapter> chapters;     // the site's own, where it has them

	struct comment {
		QString author;
		QString text;            // capped at k_comment_cap characters
		bool    pinned = false;
		bool    by_uploader = false;
		int     likes = 0;
	};
	QList<comment> comments;     // in the order the site ranked them

	// --- extracted, still without judgement ----------------------------
	//
	// **Every place that holds what looks like a tracklist**: three or more
	// lines carrying a timestamp. The site's chapters, the description, and
	// each comment are separate candidates, because they disagree often
	// enough -- a comment correcting the description, chapters a viewer's
	// guess -- that choosing between them is a judgement.
	struct entry {
		double  start = 0;       // seconds, from the first timestamp on the line
		QString text;            // the line without its timestamps
	};
	struct tracklist {
		QString      source;     // "chapters", "description", "description 2",
		                         // "comment 3", "comment 3.2"
		QList<entry> entries;    // in the source's own order
	};
	QList<tracklist> tracklists;

	static constexpr int k_description_cap = 6000;
	static constexpr int k_comment_cap     = 3000;
	static constexpr int k_max_comments    = 20;
	static constexpr int k_min_entries     = 3;

	bool empty() const { return id.isEmpty() && title.isEmpty(); }

	// From yt-dlp's `--dump-single-json`, with comments when it was asked
	// for them. A playlist's first entry, as `ytdlp_resolver::parse` takes.
	static media_evidence from_json(const QByteArray &json);

	// The timestamped lines of one text, as a candidate's entries. Pure, so
	// the extraction can be checked on its own. A time is `h:mm:ss` or
	// `m:ss` with two-digit seconds under sixty and no digit or colon on
	// either side, so `3:2` and `2026:10:09` are not times; a line counts
	// when something other than times and separators is left on it.
	static QList<entry> timestamped_lines(const QString &text);
	// The same, as the runs it falls into: a block of timestamped lines,
	// broken by any line that is not one, blanks aside. Each run of a text
	// is a candidate of its own.
	static QList<QList<entry>> timestamped_runs(const QString &text);
	static bool entry_of(const QString &line, entry *out);
};
