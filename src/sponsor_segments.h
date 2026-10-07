#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <QUrl>

// Sponsor segments: the times inside a video that somebody has submitted as
// worth skipping, and the arithmetic for using them.
//
// **The query is built so that it does not carry the video.** The service is
// asked about a *prefix* -- the first four hex characters of the SHA-256 of the
// video id -- and answers with every video whose hash begins with it, which is
// thousands of them. The one being watched is picked out here, on this machine.
// So the request says "something in this sixteen-thousandth of the catalogue",
// which is the reason a feature that asks a third party about what you are
// watching can be offered at all.
//
// That makes `parse` privacy-critical rather than merely correct: the answer
// contains other people's videos, and a bug that failed to filter by id would
// skip at times belonging to a different video -- wrong, and wrong in a way
// that looks like bad data rather than like a bug here.
//
// This half is free of the network on purpose. Deciding what a url names, what
// to ask about it, which of an answer belongs to it and where to jump are the
// parts that have to be right, and they are the parts a test can reach without
// a server.
struct sponsor_segment {
	double  from = 0;        // seconds
	double  to   = 0;
	QString category;        // "sponsor", "selfpromo", "interaction", ...
};

namespace sponsor_segments {

// The video id a url names, or empty when it names none. Every form a player
// arrives as is accepted -- a watch page, a short link, an embed, a short --
// because an embedded player in an iframe is the case this has to work for.
//
// **Validated rather than trusted.** An id is eleven characters of
// `[A-Za-z0-9_-]`; anything else is not one, and would otherwise be hashed,
// sent and compared as though it were.
QString video_id(const QUrl &url);

// The first four hex characters of the SHA-256 of the id: what goes out.
QString hash_prefix(const QString &video_id);

// The categories skipped automatically. uBlock's analogue and SponsorBlock's
// own defaults agree here: the three that are an interruption rather than part
// of the programme. Intros, outros, previews, filler and off-topic music are
// deliberately absent -- people disagree about those, and skipping something
// somebody wanted is worse than not skipping something they did not.
QStringList skipped_categories();

// The segments of `video_id` in the answer, in the skipped categories, sorted
// by start. Everything else in the answer belongs to other videos and is
// dropped here.
QList<sponsor_segment> parse(const QByteArray &json, const QString &video_id);

// **The rule for where to jump is NOT here**, and that is deliberate. It is
// applied by the injected script, against the page's own `<video>`, and a copy
// of it in C++ would be a second implementation of one rule with no caller --
// the shape this tree calls a correct function and no working feature. The
// script's own rule is tested by running it, in `test_sponsor`.

}  // namespace sponsor_segments
