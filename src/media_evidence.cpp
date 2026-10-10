#include "media_evidence.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

namespace {

QStringList strings(const QJsonValue &v) {
	QStringList out;
	for (const QJsonValue &s : v.toArray())
		out << s.toString();
	return out;
}

}  // namespace

// One line's entry, when it has a time and something besides it.
bool media_evidence::entry_of(const QString &raw, entry *out) {
	// h:mm:ss or m:ss, with seconds and, where hours are given, minutes
	// under sixty; digits on neither side, so `2026:10` or `12:345` are not
	// read as times.
	static const QRegularExpression stamp(
	  QStringLiteral("(?<![\\d:])(?:(\\d{1,2}):)?(\\d{1,2}):(\\d{2})(?![\\d:])"));
	// A time in brackets goes with its brackets -- `[1:00:00]`, `(7:45)` --
	// and what is left at the ends of a line is trimmed of separators only,
	// so a bracket that belongs to the text, as in `Track [ID]`, stays.
	static const QRegularExpression bracketed(
	  QStringLiteral("[\\[(]\\s*(?:\\d{1,2}:)?\\d{1,2}:\\d{2}"
	                 "(?:\\s*[-\\x{2013}]\\s*(?:\\d{1,2}:)?\\d{1,2}:\\d{2})?"
	                 "\\s*[\\])]"));
	static const QRegularExpression framing(
	  QStringLiteral("^[\\s\\-\\x{2013}\\x{2014}|:.,]+|"
	                 "[\\s\\-\\x{2013}\\x{2014}|:.,]+$"));
	const QRegularExpressionMatch m = stamp.match(raw);
	if (!m.hasMatch())
		return false;
	const int h = m.captured(1).isEmpty() ? 0 : m.captured(1).toInt();
	const int mi = m.captured(2).toInt();
	const int s = m.captured(3).toInt();
	if (s >= 60 || (!m.captured(1).isEmpty() && mi >= 60))
		return false;
	QString rest = raw;
	rest.replace(bracketed, QStringLiteral(" "));
	rest.replace(stamp, QStringLiteral(" "));
	rest = rest.simplified();
	rest.remove(framing);
	if (rest.isEmpty())
		return false;
	out->start = h * 3600 + mi * 60 + s;
	out->text  = rest;
	return true;
}

QList<media_evidence::entry> media_evidence::timestamped_lines(const QString &text) {
	QList<entry> out;
	for (const QList<entry> &run : timestamped_runs(text))
		out += run;
	return out;
}

// **Runs, not every line of a text at once.** A description can hold more
// than one block of times -- measured on the first set looked at, whose
// tracklist was followed by a premiere's clock times in nine cities, `07:00
// -- Los Angeles` read as seven minutes -- and as one candidate the two
// interleaved into a list that went backwards, so the real tracklist was
// refused with them. Blank lines do not end a run; any other line does.
QList<QList<media_evidence::entry>> media_evidence::timestamped_runs(
  const QString &text) {
	QList<QList<entry>> runs;
	QList<entry> run;
	for (const QString &raw : text.split(QLatin1Char('\n'))) {
		if (raw.trimmed().isEmpty())
			continue;
		entry e;
		if (entry_of(raw, &e)) {
			run << e;
			continue;
		}
		if (!run.isEmpty())
			runs << run;
		run.clear();
	}
	if (!run.isEmpty())
		runs << run;
	return runs;
}

media_evidence media_evidence::from_json(const QByteArray &json) {
	media_evidence ev;
	QJsonObject o = QJsonDocument::fromJson(json).object();
	if (o.value("_type").toString() == "playlist") {
		const QJsonArray entries = o.value("entries").toArray();
		if (entries.isEmpty())
			return ev;
		o = entries.first().toObject();
	}
	ev.id           = o.value("id").toString();
	ev.title        = o.value("title").toString();
	ev.uploader     = o.value("uploader").toString();
	ev.uploader_id  = o.value("uploader_id").toString();
	ev.channel      = o.value("channel").toString();
	ev.artist       = o.value("artist").toString();
	if (ev.artist.isEmpty())
		ev.artist = o.value("creator").toString();
	ev.track        = o.value("track").toString();
	ev.album        = o.value("album").toString();
	ev.series       = o.value("series").toString();
	ev.categories   = strings(o.value("categories"));
	ev.tags         = strings(o.value("tags"));
	ev.duration     = o.value("duration").toDouble(-1);
	ev.width        = o.value("width").toInt();
	ev.height       = o.value("height").toInt();
	if (o.value("timestamp").isDouble())
		ev.timestamp = qint64(o.value("timestamp").toDouble());
	ev.upload_date  = o.value("upload_date").toString();
	if (o.value("release_year").isDouble())
		ev.release_year = o.value("release_year").toInt();
	ev.live_status  = o.value("live_status").toString();
	ev.description  = o.value("description").toString().left(k_description_cap);

	for (const QJsonValue &c : o.value("chapters").toArray()) {
		const QJsonObject co = c.toObject();
		chapter ch;
		ch.start = co.value("start_time").toDouble();
		ch.title = co.value("title").toString();
		ev.chapters << ch;
	}
	for (const QJsonValue &c : o.value("comments").toArray()) {
		if (ev.comments.size() >= k_max_comments)
			break;
		const QJsonObject co = c.toObject();
		// Top-level comments only: a reply is a conversation, not a
		// tracklist, and a long thread would crowd out the next comment.
		if (co.value("parent").toString() != QLatin1String("root") &&
		    co.contains("parent"))
			continue;
		comment cm;
		cm.author      = co.value("author").toString();
		cm.text        = co.value("text").toString().left(k_comment_cap);
		cm.pinned      = co.value("is_pinned").toBool();
		cm.by_uploader = co.value("author_is_uploader").toBool();
		cm.likes       = co.value("like_count").toInt();
		ev.comments << cm;
	}

	// Candidates, each source on its own.
	if (ev.chapters.size() >= k_min_entries) {
		tracklist t;
		t.source = QStringLiteral("chapters");
		for (const chapter &ch : ev.chapters)
			t.entries << entry{ ch.start, ch.title };
		ev.tracklists << t;
	}
	// Each run of a text its own candidate: "description", then
	// "description 2"; "comment 3", then "comment 3.2".
	const auto add_runs = [&ev](const QString &text, const QString &name,
	                            const QString &next) {
		int n = 0;
		for (const QList<entry> &run : timestamped_runs(text)) {
			if (run.size() < k_min_entries)
				continue;
			++n;
			ev.tracklists << tracklist{ n == 1 ? name : next.arg(n), run };
		}
	};
	add_runs(ev.description, QStringLiteral("description"),
	         QStringLiteral("description %1"));
	for (int i = 0; i < ev.comments.size(); ++i)
		add_runs(ev.comments.at(i).text, QString("comment %1").arg(i + 1),
		         QString("comment %1.").arg(i + 1) + QStringLiteral("%1"));
	return ev;
}
