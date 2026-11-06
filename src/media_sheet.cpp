#include "media_sheet.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>

namespace {

// 00:12:34.000: fixed width, so a column of them lines up and sorts.
QString clock(double seconds) {
	const qint64 ms = qRound64(seconds * 1000);
	return QString("%1:%2:%3.%4")
	  .arg(ms / 3600000, 2, 10, QLatin1Char('0'))
	  .arg(ms / 60000 % 60, 2, 10, QLatin1Char('0'))
	  .arg(ms / 1000 % 60, 2, 10, QLatin1Char('0'))
	  .arg(ms % 1000, 3, 10, QLatin1Char('0'));
}

// Minutes, seconds and frames of 1/75 s. Minutes do not roll over into hours:
// the format has none, and an hour in is 60:00:00.
QString cue_index(double seconds) {
	const qint64 frames = qRound64(seconds * 75);
	return QString("%1:%2:%3")
	  .arg(frames / 75 / 60, 2, 10, QLatin1Char('0'))
	  .arg(frames / 75 % 60, 2, 10, QLatin1Char('0'))
	  .arg(frames % 75, 2, 10, QLatin1Char('0'));
}

// "01. Artist - Title" into its parts. No separator: all of it is the title.
void split_track(QString text, QString *performer, QString *title) {
	static const QRegularExpression number(QStringLiteral("^\\d{1,3}[.)]\\s+"));
	static const QRegularExpression dash(QStringLiteral("\\s[-\\x{2013}\\x{2014}]\\s"));
	text.remove(number);
	const QRegularExpressionMatch m = dash.match(text);
	if (!m.hasMatch()) {
		performer->clear();
		*title = text.trimmed();
		return;
	}
	*performer = text.left(m.capturedStart()).trimmed();
	*title     = text.mid(m.capturedEnd()).trimmed();
}

// A cue sheet's strings are quoted and have no escape.
QString quoted(QString s) {
	return QLatin1Char('"') + s.replace(QLatin1Char('"'), QLatin1Char('\'')) +
	       QLatin1Char('"');
}

bool whole_seconds(const QList<media_evidence::entry> &entries) {
	for (const media_evidence::entry &e : entries)
		if (qRound64(e.start * 1000) % 1000 != 0)
			return false;
	return true;
}

bool write_new(const QString &path, const QString &text, QString *error) {
	if (text.isEmpty() || QFileInfo::exists(path))
		return true;
	QSaveFile f(path);
	if (f.open(QIODevice::WriteOnly) && f.write(text.toUtf8()) >= 0 && f.commit())
		return true;
	if (error)
		*error = QString("Could not write %1: %2")
		           .arg(QFileInfo(path).fileName(), f.errorString());
	return false;
}

}  // namespace

QString media_sheet::summary() const {
	if (has_times())
		return QString("%1 tracks, with times, from the %2").arg(entries.size()).arg(source);
	if (!untimed.isEmpty())
		return QString("%1 tracks, without times, from the %2")
		  .arg(untimed.size()).arg(untimed_source);
	return QStringLiteral("no tracklist found");
}

QString media_sheet::text() const {
	QString out = title + QLatin1Char('\n') + QString(title.size(), QLatin1Char('=')) +
	              QLatin1Char('\n');
	if (!artist.isEmpty())
		out += "Artist: " + artist + QLatin1Char('\n');
	if (page.isValid())
		out += "From: " + page.toString() + QLatin1Char('\n');
	if (duration > 0)
		out += "Length: " + clock(duration) + QLatin1Char('\n');
	out += QLatin1Char('\n');

	if (has_times()) {
		out += QString("Tracklist: %1 tracks, from the %2. Times are the source's, %3.\n\n")
		         .arg(entries.size()).arg(source,
		              whole_seconds(entries) ? QStringLiteral("to the second")
		                                     : QStringLiteral("to the millisecond"));
		for (const media_evidence::entry &e : entries)
			out += clock(e.start) + QStringLiteral("  ") + e.text + QLatin1Char('\n');
	} else if (!untimed.isEmpty()) {
		out += QString("Tracklist: %1 tracks, from the %2, which gives no times.\n\n")
		         .arg(untimed.size()).arg(untimed_source);
		for (const QString &line : untimed)
			out += line + QLatin1Char('\n');
	} else {
		out += QStringLiteral("Tracklist: none found.\n");
	}
	if (!others.isEmpty())
		out += "\nAlso seen, and not taken: " + others.join(QStringLiteral(", ")) +
		       QStringLiteral(".\n");

	if (!description.isEmpty())
		out += QStringLiteral("\nDescription\n-----------\n") + description +
		       QLatin1Char('\n');
	return out;
}

QString media_sheet::m3u(const QString &media_name) const {
	if (!has_times())
		return QString();
	QString out = QStringLiteral("#EXTM3U\n#PLAYLIST:") + title + QLatin1Char('\n');
	for (int i = 0; i < entries.size(); ++i) {
		const double start = entries.at(i).start;
		const double stop  = i + 1 < entries.size() ? entries.at(i + 1).start : duration;
		out += QString("#EXTINF:%1,%2\n")
		         .arg(stop > start ? qRound64(stop - start) : -1)
		         .arg(entries.at(i).text);
		out += QString("#EXTVLCOPT:start-time=%1\n").arg(start, 0, 'f', 3);
		if (stop > start)
			out += QString("#EXTVLCOPT:stop-time=%1\n").arg(stop, 0, 'f', 3);
		out += media_name + QLatin1Char('\n');
	}
	return out;
}

QString media_sheet::cue(const QString &media_name) const {
	if (!has_times() || entries.size() > 99)
		return QString();
	QString out;
	if (!artist.isEmpty())
		out += "PERFORMER " + quoted(artist) + QLatin1Char('\n');
	out += "TITLE " + quoted(title) + QLatin1Char('\n');
	// What the format has a word for; WAVE is what players take for the rest.
	const bool mp3 = media_name.endsWith(QStringLiteral(".mp3"), Qt::CaseInsensitive);
	out += "FILE " + quoted(media_name) + (mp3 ? " MP3\n" : " WAVE\n");
	for (int i = 0; i < entries.size(); ++i) {
		QString performer, name;
		split_track(entries.at(i).text, &performer, &name);
		out += QString("  TRACK %1 AUDIO\n").arg(i + 1, 2, 10, QLatin1Char('0'));
		out += "    TITLE " + quoted(name) + QLatin1Char('\n');
		if (!performer.isEmpty())
			out += "    PERFORMER " + quoted(performer) + QLatin1Char('\n');
		out += "    INDEX 01 " + cue_index(entries.at(i).start) + QLatin1Char('\n');
	}
	return out;
}

QString media_sheet::to_json() const {
	QJsonArray marks;
	for (const media_evidence::entry &e : entries)
		marks << QJsonObject{ { "at", e.start }, { "text", e.text } };
	return QString::fromUtf8(QJsonDocument(QJsonObject{
		{ "title", title }, { "artist", artist }, { "page", page.toString() },
		{ "duration", duration }, { "description", description }, { "source", source },
		{ "entries", marks }, { "untimed_source", untimed_source },
		{ "untimed", QJsonArray::fromStringList(untimed) },
		{ "others", QJsonArray::fromStringList(others) },
	}).toJson(QJsonDocument::Compact));
}

media_sheet media_sheet::from_json(const QString &json) {
	const QJsonObject o = QJsonDocument::fromJson(json.toUtf8()).object();
	const auto strings = [](const QJsonValue &v) {
		QStringList out;
		for (const QJsonValue &s : v.toArray())
			out << s.toString();
		return out;
	};
	media_sheet s;
	s.title          = o.value("title").toString();
	s.artist         = o.value("artist").toString();
	s.page           = QUrl(o.value("page").toString());
	s.duration       = o.value("duration").toDouble(-1);
	s.description    = o.value("description").toString();
	s.source         = o.value("source").toString();
	s.untimed_source = o.value("untimed_source").toString();
	s.untimed        = strings(o.value("untimed"));
	s.others         = strings(o.value("others"));
	for (const QJsonValue &v : o.value("entries").toArray()) {
		const QJsonObject e = v.toObject();
		s.entries << media_evidence::entry{ e.value("at").toDouble(),
		                                    e.value("text").toString() };
	}
	return s;
}

media_sheet media_sheet::from_evidence(const media_evidence &ev, const QString &source,
                                       const QString &artist, const QString &title,
                                       const QUrl &page,
                                       const QString &whole_description) {
	media_sheet s;
	s.title       = title.isEmpty() ? ev.title : title;
	s.artist      = artist;
	s.page        = page;
	s.duration    = ev.duration;
	s.description = whole_description.isEmpty() ? ev.description : whole_description;
	for (const media_evidence::tracklist &t : ev.tracklists) {
		if (t.source == source) {
			s.source  = t.source;
			s.entries = t.entries;
		} else {
			s.others << QString("%1 (%2 lines)").arg(t.source).arg(t.entries.size());
		}
	}
	if (s.entries.isEmpty() && !ev.untimed.isEmpty()) {
		s.untimed_source = ev.untimed.first().source;
		s.untimed        = ev.untimed.first().lines;
	}
	return s;
}

bool media_sheet::write_beside(const QString &media_path, QString *error) const {
	const QString name = QFileInfo(media_path).fileName();
	return write_new(media_path + QStringLiteral(".tracklist.txt"), text(), error) &&
	       write_new(media_path + QStringLiteral(".m3u"), m3u(name), error) &&
	       write_new(media_path + QStringLiteral(".cue"), cue(name), error);
}
