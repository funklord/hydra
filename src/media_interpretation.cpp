#include "media_interpretation.h"

#include "ai_provider.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

namespace {

const char *const k_kinds[] = { "music", "movie", "episode", "talk", "learning",
                                "news", "gaming", "short", "misc" };

QString normal(const QString &s) {
	return s.simplified().toCaseFolded();
}

// The first `{...}` that parses as an object with a "kind" key, as the
// investigation finds its actions: a model that wraps the answer in prose or
// a code fence is still understood.
QJsonObject answer_in(const QString &reply) {
	for (int start = reply.indexOf('{'); start >= 0;
	     start = reply.indexOf('{', start + 1)) {
		int depth = 0;
		bool in_string = false;
		for (int i = start; i < reply.size(); ++i) {
			const QChar c = reply.at(i);
			if (in_string) {
				if (c == '\\')
					++i;
				else if (c == '"')
					in_string = false;
				continue;
			}
			if (c == '"')
				in_string = true;
			else if (c == '{')
				++depth;
			else if (c == '}' && --depth == 0) {
				const QJsonDocument d = QJsonDocument::fromJson(
				  reply.mid(start, i - start + 1).toUtf8());
				if (d.isObject() && d.object().contains("kind"))
					return d.object();
				break;
			}
		}
	}
	return QJsonObject();
}

// Why a tracklist cannot be the one, or empty when it can.
QString tracklist_problem(const media_evidence::tracklist &t, double duration) {
	// Strictly rising: two tracks cannot start at once, and a list of
	// times that repeat -- a premiere's start in four cities sharing a
	// zone -- is something other than a tracklist.
	double last = -1;
	for (const media_evidence::entry &e : t.entries) {
		if (e.start <= last)
			return QStringLiteral("its times do not rise");
		last = e.start;
	}
	if (duration > 0 && last >= duration)
		return QStringLiteral("it runs past the end of the video");
	return QString();
}

}  // namespace

QString media_reading::name_of(kind k) {
	return QString::fromLatin1(k_kinds[int(k)]);
}

bool media_reading::kind_from(const QString &name, kind *out) {
	const QString n = name.trimmed().toLower();
	for (int i = 0; i < int(std::size(k_kinds)); ++i)
		if (n == QLatin1String(k_kinds[i])) {
			*out = kind(i);
			return true;
		}
	return false;
}

QStringList media_reading::kind_names() {
	QStringList out;
	for (const char *k : k_kinds)
		out << QString::fromLatin1(k);
	return out;
}

media_interpretation::media_interpretation(ai_provider *provider,
                                           const media_evidence &evidence,
                                           QObject *parent)
  : QObject(parent), m_provider(provider), m_evidence(evidence) {
	if (m_provider) {
		connect(m_provider, &ai_provider::finished, this,
		         &media_interpretation::on_reply);
		connect(m_provider, &ai_provider::failed, this,
		         &media_interpretation::on_failed);
	}
}

media_interpretation::~media_interpretation() {
	if (m_provider && m_waiting)
		m_provider->cancel();
}

void media_interpretation::start() {
	if (!m_provider) {
		emit finished(by_rules(m_evidence));
		return;
	}
	m_waiting = true;
	m_provider->send(system_prompt(), prompt_for(m_evidence));
}

void media_interpretation::on_reply(const QString &reply) {
	if (!m_waiting)
		return;
	m_waiting = false;
	emit finished(checked(reply, m_evidence));
}

void media_interpretation::on_failed(const QString &error) {
	if (!m_waiting)
		return;
	m_waiting = false;
	media_reading r = by_rules(m_evidence);
	r.rejected << "model: " + error;
	emit finished(r);
}

QString media_interpretation::system_prompt() {
	return QStringLiteral(
	  "You read the metadata of an online video and say what it is, so it "
	  "can be saved under the right name. Reply with ONE JSON object and "
	  "nothing else:\n"
	  "{\"kind\": \"...\", \"artist\": \"...\", \"title\": \"...\", "
	  "\"is_set\": true|false, \"tracklist\": \"...\", \"confidence\": 0.0-1.0}\n\n"
	  "kind is one of: %1.\n"
	  "  music: songs, music videos, albums, DJ sets, concerts.\n"
	  "  movie: a feature film.  episode: a TV or web series episode.\n"
	  "  talk: podcasts, interviews, lectures, long commentary.\n"
	  "  learning: tutorials, how-to, explainers.  news: news reports.\n"
	  "  gaming: game play.  short: a very short clip, under a minute.\n"
	  "  misc: anything else.\n"
	  "artist: who it is by -- for music the performing artist, or for a set "
	  "or compilation whoever made or curated it; otherwise the person or "
	  "channel responsible. title: what it is called -- for a song the song's "
	  "name alone, without the artist, \"official video\" or similar.\n"
	  "COPY artist and title EXACTLY from the metadata -- the title, "
	  "uploader, channel, artist, track, tags or description. Never invent "
	  "or translate a name. Use \"\" when you cannot tell.\n"
	  "is_set: true for a mix, DJ set, full album or compilation of tracks.\n"
	  "tracklist: the source of the tracklist, copied from the list of "
	  "candidate tracklists given (\"chapters\", \"description\", "
	  "\"comment 3\"...), or \"\" if none is a real tracklist for this "
	  "video.\n").arg(media_reading::kind_names().join(", "));
}

QString media_interpretation::prompt_for(const media_evidence &ev) {
	QString p;
	const auto line = [&p](const char *k, const QString &v) {
		if (!v.isEmpty())
			p += QString::fromLatin1(k) + ": " + v + "\n";
	};
	line("title", ev.title);
	line("uploader", ev.uploader);
	line("channel", ev.channel);
	line("artist (site's music metadata)", ev.artist);
	line("track (site's music metadata)", ev.track);
	line("album", ev.album);
	line("series", ev.series);
	line("categories", ev.categories.join(", "));
	line("tags", ev.tags.mid(0, 20).join(", "));
	if (ev.duration > 0)
		line("duration", QString("%1 minutes").arg(ev.duration / 60, 0, 'f', 1));
	if (ev.width > 0 && ev.height > 0)
		line("frame", QString("%1x%2").arg(ev.width).arg(ev.height));
	line("live", ev.live_status);
	p += "\ndescription:\n" + ev.description.left(1500) + "\n";
	p += "\ncandidate tracklists:\n";
	if (ev.tracklists.isEmpty())
		p += "  none\n";
	for (const media_evidence::tracklist &t : ev.tracklists) {
		p += QString("  %1 (%2 entries):").arg(t.source).arg(t.entries.size());
		for (int i = 0; i < t.entries.size() && i < 4; ++i)
			p += " | " + t.entries.at(i).text;
		p += "\n";
	}
	if (!ev.comments.isEmpty()) {
		p += "\ntop comments:\n";
		for (int i = 0; i < ev.comments.size() && i < 5; ++i) {
			const media_evidence::comment &c = ev.comments.at(i);
			p += QString("  %1%2: %3\n")
			       .arg(c.author, c.by_uploader ? " (the uploader)" : "",
			            c.text.left(200).simplified());
		}
	}
	return p;
}

bool media_interpretation::grounded(const QString &value,
                                    const media_evidence &ev) {
	const QString v = normal(value);
	if (v.isEmpty())
		return false;
	QStringList sources = { ev.title, ev.uploader, ev.channel, ev.artist,
	                        ev.track, ev.album, ev.series, ev.description };
	sources += ev.tags;
	for (const QString &s : sources)
		if (normal(s).contains(v))
			return true;
	return false;
}

media_reading media_interpretation::by_rules(const media_evidence &ev) {
	media_reading r;
	r.artist = !ev.artist.isEmpty() ? ev.artist
	           : !ev.uploader.isEmpty() ? ev.uploader : ev.channel;
	r.title  = !ev.track.isEmpty() ? ev.track : ev.title;
	// A title that calls itself a music video, audio or lyrics.
	static const QRegularExpression music_title(
	  QStringLiteral("official (music )?(video|audio)|\\((official )?audio\\)|"
	                 "\\blyrics?\\b|\\bvisuali[sz]er\\b"),
	  QRegularExpression::CaseInsensitiveOption);
	const bool music = !ev.track.isEmpty() || !ev.artist.isEmpty() ||
	                   !ev.album.isEmpty() ||
	                   ev.categories.contains(QStringLiteral("Music")) ||
	                   ev.channel.endsWith(QLatin1String(" - Topic")) ||
	                   music_title.match(ev.title).hasMatch();
	const bool film = ev.duration >= 3600 &&
	                  (ev.categories.contains(QStringLiteral("Film & Animation")) ||
	                   ev.release_year > 0 ||
	                   ev.title.contains(QLatin1String("full movie"),
	                                     Qt::CaseInsensitive));
	if (music)
		r.what = media_reading::kind::music;
	else if (!ev.series.isEmpty())
		r.what = media_reading::kind::episode;
	else if (film)
		r.what = media_reading::kind::movie;
	else if (ev.duration > 0 && ev.duration < 60)
		r.what = media_reading::kind::short_clip;
	else if (ev.categories.contains(QStringLiteral("Gaming")))
		r.what = media_reading::kind::gaming;
	else if (ev.categories.contains(QStringLiteral("News & Politics")))
		r.what = media_reading::kind::news;
	else if (ev.categories.contains(QStringLiteral("Education")) ||
	         ev.categories.contains(QStringLiteral("Howto & Style")) ||
	         ev.categories.contains(QStringLiteral("Science & Technology")))
		r.what = media_reading::kind::learning;
	else
		r.what = media_reading::kind::misc;
	return r;
}

media_reading media_interpretation::checked(const QString &reply,
                                            const media_evidence &ev) {
	media_reading r = by_rules(ev);
	const QJsonObject a = answer_in(reply);
	if (a.isEmpty()) {
		r.rejected << QStringLiteral("answer: the reply held no JSON answer");
		return r;
	}
	r.model_answered = true;
	r.confidence = qBound(0.0, a.value("confidence").toDouble(), 1.0);

	media_reading::kind k;
	if (media_reading::kind_from(a.value("kind").toString(), &k)) {
		r.what = k;
		r.from_model << QStringLiteral("kind");
	} else {
		r.rejected << QString("kind: \"%1\" is not one of the categories")
		                  .arg(a.value("kind").toString());
	}

	for (const char *field : { "artist", "title" }) {
		const QString v = a.value(QLatin1String(field)).toString().trimmed();
		if (v.isEmpty()) {
			r.rejected << QString("%1: left empty").arg(QLatin1String(field));
			continue;
		}
		if (!grounded(v, ev)) {
			r.rejected << QString("%1: \"%2\" is not in the metadata")
			                  .arg(QLatin1String(field), v);
			continue;
		}
		(field == QLatin1String("artist") ? r.artist : r.title) = v;
		r.from_model << QLatin1String(field);
	}

	r.is_set = a.value("is_set").toBool();
	const QString source = a.value("tracklist").toString().trimmed();
	if (source.isEmpty()) {
		r.from_model << QStringLiteral("tracklist");
	} else {
		const media_evidence::tracklist *found = nullptr;
		for (const media_evidence::tracklist &t : ev.tracklists)
			if (t.source == source)
				found = &t;
		if (!found) {
			r.rejected << QString("tracklist: \"%1\" is not one of the "
			                      "candidates").arg(source);
		} else if (const QString why = tracklist_problem(*found, ev.duration);
		           !why.isEmpty()) {
			r.rejected << QString("tracklist: %1 %2").arg(source, why);
		} else {
			r.tracklist = source;
			r.from_model << QStringLiteral("tracklist");
		}
	}
	return r;
}
