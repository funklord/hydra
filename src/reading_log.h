#pragma once

#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>

// **One reading of what a video is, kept.** Asked for by the holder on
// 2026-10-10, after a reading ran and the only trace of what it decided was a
// status line that had already gone: the model's call was in Ollama's log,
// the tally's counts were in the settings, and the answer itself -- artist,
// title, tracklist, the file name it produced -- was nowhere.
//
// Kept for two readers: the person, who wants to see what was read after
// the fact, and the corrections still to come, which are per channel and
// need to know what was read for a channel before they can say it was wrong.
struct reading_record {
	QDateTime   when;
	QString     page;
	QString     id;           // the site's video id
	QString     extractor;
	QString     uploader;
	QString     channel;
	QString     model;        // who read it: "Ollama qwen2.5-coder:14b", or "rules"
	QString     kind;
	QString     artist;
	QString     title;
	bool        is_set = false;
	QString     tracklist;    // the candidate taken, or empty
	double      confidence = 0;
	QString     file_name;    // the name it produced, without the extension
	QStringList from_model;   // the tasks the model's answer was kept for
	QStringList rejected;
	QStringList flagged;
	// The video's own title as the site gave it: what a correction's rule
	// is learned from (`channel_rules::infer`).
	QString     video_title;
	// What decided the artist and title where a channel's rule did, in words;
	// "corrected" where the person corrected this reading itself.
	QString     rule;
	// The model's kept answers the person corrected, already counted against
	// it: a reading corrected twice counts once.
	QStringList overruled;
};

// Newest last, at most `k_max` -- a record of what was read, not an archive
// of it -- in an INI file beside the annoyance reports, by the rule that what
// Hydra writes is INI unless its shape defeats it.
class reading_log {
public:
	static constexpr int k_max = 200;

	void add(const reading_record &r);
	QList<reading_record> all() const { return m_records; }
	// The readings of one channel, newest last: what a correction for that
	// channel will be checked against.
	QList<reading_record> for_channel(const QString &channel) const;
	int  size() const { return int(m_records.size()); }
	// Puts a corrected reading in place of the one at `index` in `all()`.
	bool replace(int index, const reading_record &r);
	// Forgets every reading; how many there were.
	int  clear_all();

	// Nothing is cleared until the file has been read, and a file that will
	// not parse is left alone and reported, not treated as an empty log --
	// the shape `annoyance_log::load` learned.
	bool load(const QString &path);
	bool save(const QString &path) const;

private:
	QList<reading_record> m_records;
};
