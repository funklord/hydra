#pragma once

#include "media_evidence.h"

#include <QList>
#include <QString>
#include <QStringList>
#include <QUrl>

// What a saved mix leaves beside itself (architecture doc sec 11.2).
//
// A set saved as one file holds its tracklist as chapters, which a player can
// step through and a mixing program cannot read. So beside the file go:
//
//   `<file>.tracklist.txt`  the title, the tracklist, and the whole description,
//                           saying plainly when there is no tracklist;
//   `<file>.m3u`            one entry per track, VLC's start and stop times;
//   `<file>.cue`            a cue sheet, the form DJ and audio software reads.
//
// **Named from the whole file name**, extension included, so "Both, as two
// files" gets a set for each and neither overwrites the other.
//
// **As fine as the source, and no finer.** A cue sheet counts 1/75 of a second
// and the text gives milliseconds, but a description says whole seconds, and a
// figure of `.000` is not a measurement. The text says which it has.
//
// Held as JSON text, the way `chapters` is held as ffmpeg's: it rides on a media
// row and a download job as one string, and only this file reads it.
struct media_sheet {
	QString     title;
	QString     artist;
	QUrl        page;
	double      duration = -1;
	QString     description;     // whole, not the evidence's capped copy
	QString     source;          // the candidate taken; empty for none
	QList<media_evidence::entry> entries;
	QString     untimed_source;  // where `untimed` came from
	QStringList untimed;         // a tracklist that gives no times
	QStringList others;          // candidates not taken, each with its size

	bool has_times() const { return !entries.isEmpty(); }
	bool has_tracklist() const { return has_times() || !untimed.isEmpty(); }

	// One line for a status bar or a download's row.
	QString summary() const;

	QString text() const;
	// Empty where the file would say nothing: no times, or a cue sheet past the
	// format's ninety-nine tracks.
	QString m3u(const QString &media_name) const;
	QString cue(const QString &media_name) const;

	QString to_json() const;
	static media_sheet from_json(const QString &json);

	// `source` is the candidate the reading chose; `whole_description` is
	// yt-dlp's own, since the evidence caps its copy.
	static media_sheet from_evidence(const media_evidence &ev, const QString &source,
	                                 const QString &artist, const QString &title,
	                                 const QUrl &page,
	                                 const QString &whole_description);

	// The files, beside `media_path`. An existing file is left alone: it may be
	// somebody's edit. False with `error` set if one could not be written.
	bool write_beside(const QString &media_path, QString *error) const;
};
