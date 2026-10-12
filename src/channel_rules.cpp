#include "channel_rules.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSettings>

namespace {

QString normal(const QString &s) {
	return s.simplified();
}

bool same(const QString &a, const QString &b) {
	return !normal(a).isEmpty() &&
	       normal(a).compare(normal(b), Qt::CaseInsensitive) == 0;
}

// The title split at the first `sep`, both sides trimmed; false without it.
bool split(const QString &title, const QString &sep, QString *before,
           QString *after) {
	const int at = title.indexOf(sep);
	if (at <= 0)
		return false;
	*before = normal(title.left(at));
	*after  = normal(title.mid(at + sep.size()));
	return !before->isEmpty() && !after->isEmpty();
}

const char *artist_name(channel_rule::artist_source a) {
	switch (a) {
		case channel_rule::artist_source::keep:     return "keep";
		case channel_rule::artist_source::uploader: return "uploader";
		case channel_rule::artist_source::before:   return "before";
		case channel_rule::artist_source::after:    return "after";
		case channel_rule::artist_source::fixed:    return "fixed";
	}
	return "keep";
}

const char *title_name(channel_rule::title_source t) {
	switch (t) {
		case channel_rule::title_source::keep:   return "keep";
		case channel_rule::title_source::whole:  return "whole";
		case channel_rule::title_source::before: return "before";
		case channel_rule::title_source::after:  return "after";
	}
	return "keep";
}

}  // namespace

QStringList channel_rules::separators() {
	return { QStringLiteral(" - "), QStringLiteral(" – "), QStringLiteral(" — "),
	         QStringLiteral(" | "), QStringLiteral(": "), QStringLiteral(" ~ ") };
}

QString channel_rules::without_tags(const QString &title) {
	// Trailing bracketed groups only: "(Official Video) [HD]" goes, a bracket
	// inside the title stays, since it may be the title.
	static const QRegularExpression tail(QStringLiteral("\\s*[\\(\\[][^\\)\\]]*[\\)\\]]\\s*$"));
	QString t = normal(title);
	for (int guard = 0; guard < 8; ++guard) {
		const QString shorter = normal(QString(t).remove(tail));
		if (shorter == t || shorter.isEmpty())
			break;
		t = shorter;
	}
	return t;
}

QString channel_rule::describe() const {
	QStringList parts;
	const QString quoted = "\"" + separator + "\"";
	switch (artist) {
		case artist_source::uploader:
			parts << "the artist is the uploader";
			break;
		case artist_source::before:
			parts << "the artist is the part of the title before " + quoted;
			break;
		case artist_source::after:
			parts << "the artist is the part of the title after " + quoted;
			break;
		case artist_source::fixed:
			parts << "the artist is always \"" + fixed_artist + "\"";
			break;
		case artist_source::keep:
			break;
	}
	switch (title) {
		case title_source::whole:
			parts << "the track is the whole title";
			break;
		case title_source::before:
			parts << "the track is the part of the title before " + quoted;
			break;
		case title_source::after:
			parts << "the track is the part of the title after " + quoted;
			break;
		case title_source::keep:
			break;
	}
	if (!kind.isEmpty())
		parts << "it is " + kind;
	if (parts.isEmpty())
		return QStringLiteral("nothing to remember");
	QString s = parts.join("; ");
	s[0] = s[0].toUpper();
	return s;
}

channel_rule channel_rules::infer(const QString &channel, const QString &video_title,
                                  const QString &uploader, const QString &artist,
                                  const QString &title, const QString &kind,
                                  const QString &read_kind) {
	channel_rule r;
	r.channel = channel;
	r.learned = QDateTime::currentDateTimeUtc();

	// The title first, because a separator it is split at says which side the
	// artist should come from.
	if (same(title, video_title) || same(title, without_tags(video_title))) {
		r.title = channel_rule::title_source::whole;
	} else {
		for (const QString &sep : separators()) {
			QString before, after;
			if (!split(video_title, sep, &before, &after))
				continue;
			if (same(title, after) || same(title, without_tags(after))) {
				r.title     = channel_rule::title_source::after;
				r.separator = sep;
				break;
			}
			if (same(title, before) || same(title, without_tags(before))) {
				r.title     = channel_rule::title_source::before;
				r.separator = sep;
				break;
			}
		}
	}

	// The artist: the other side of the title's separator, then the
	// uploader, then any separator, and last a name the channel always uses.
	QString before, after;
	const bool split_here = !r.separator.isEmpty() &&
	                        split(video_title, r.separator, &before, &after);
	if (split_here && r.title == channel_rule::title_source::after && same(artist, before)) {
		r.artist = channel_rule::artist_source::before;
	} else if (split_here && r.title == channel_rule::title_source::before &&
	           same(artist, without_tags(after))) {
		r.artist = channel_rule::artist_source::after;
	} else if (same(artist, uploader)) {
		r.artist = channel_rule::artist_source::uploader;
	} else {
		for (const QString &sep : separators()) {
			if (!split(video_title, sep, &before, &after))
				continue;
			if (same(artist, before) || same(artist, without_tags(after))) {
				r.artist = same(artist, before) ? channel_rule::artist_source::before
				                                : channel_rule::artist_source::after;
				// A title already split at another separator keeps its own.
				if (r.separator.isEmpty())
					r.separator = sep;
				else if (r.separator != sep)
					r.artist = channel_rule::artist_source::keep;
				break;
			}
		}
		if (r.artist == channel_rule::artist_source::keep && !normal(artist).isEmpty()) {
			r.artist       = channel_rule::artist_source::fixed;
			r.fixed_artist = normal(artist);
		}
	}

	if (!kind.isEmpty() && kind != read_kind)
		r.kind = kind;
	return r;
}

bool channel_rules::apply(const channel_rule &rule, const QString &video_title,
                          const QString &uploader, QString *artist, QString *title,
                          QString *kind, QString *why) {
	bool whole = true;
	QString before, after;
	const bool has_split = !rule.separator.isEmpty() &&
	                       split(video_title, rule.separator, &before, &after);
	auto missing = [&] {
		whole = false;
		if (why)
			*why = QString("the title has no \"%1\", so it was read as usual")
			           .arg(rule.separator);
	};

	switch (rule.artist) {
		case channel_rule::artist_source::uploader:
			if (!normal(uploader).isEmpty())
				*artist = normal(uploader);
			break;
		case channel_rule::artist_source::before:
			has_split ? void(*artist = before) : missing();
			break;
		case channel_rule::artist_source::after:
			has_split ? void(*artist = without_tags(after)) : missing();
			break;
		case channel_rule::artist_source::fixed:
			*artist = rule.fixed_artist;
			break;
		case channel_rule::artist_source::keep:
			break;
	}
	switch (rule.title) {
		case channel_rule::title_source::whole:
			*title = without_tags(video_title);
			break;
		case channel_rule::title_source::before:
			has_split ? void(*title = without_tags(before)) : missing();
			break;
		case channel_rule::title_source::after:
			has_split ? void(*title = without_tags(after)) : missing();
			break;
		case channel_rule::title_source::keep:
			break;
	}
	if (!rule.kind.isEmpty() && kind)
		*kind = rule.kind;
	return whole;
}

void channel_rules::set(const channel_rule &rule) {
	for (channel_rule &r : m_rules)
		if (r.channel == rule.channel) {
			r = rule;
			return;
		}
	m_rules << rule;
}

const channel_rule *channel_rules::find(const QString &channel) const {
	if (channel.isEmpty())
		return nullptr;
	for (const channel_rule &r : m_rules)
		if (r.channel == channel)
			return &r;
	return nullptr;
}

int channel_rules::clear_all() {
	const int n = int(m_rules.size());
	m_rules.clear();
	return n;
}

bool channel_rules::load(const QString &path) {
	if (!QFileInfo::exists(path))
		return false;
	QSettings f(path, QSettings::IniFormat);
	f.allKeys();   // the parse is lazy; status() before it says NoError
	if (f.status() != QSettings::NoError) {
		qCritical("channel rules: %s did not parse; leaving it alone rather "
		           "than treating it as empty", qPrintable(path));
		return false;
	}
	auto artist_of = [](const QString &s) {
		for (auto a : { channel_rule::artist_source::uploader,
		                channel_rule::artist_source::before,
		                channel_rule::artist_source::after,
		                channel_rule::artist_source::fixed })
			if (s == artist_name(a))
				return a;
		return channel_rule::artist_source::keep;
	};
	auto title_of = [](const QString &s) {
		for (auto t : { channel_rule::title_source::whole,
		                channel_rule::title_source::before,
		                channel_rule::title_source::after })
			if (s == title_name(t))
				return t;
		return channel_rule::title_source::keep;
	};
	QList<channel_rule> loaded;
	const int n = f.beginReadArray("rules");
	for (int i = 0; i < n; ++i) {
		f.setArrayIndex(i);
		channel_rule r;
		r.channel      = f.value("channel").toString();
		r.artist       = artist_of(f.value("artist").toString());
		r.title        = title_of(f.value("title").toString());
		r.separator    = f.value("separator").toString();
		r.fixed_artist = f.value("fixed_artist").toString();
		r.kind         = f.value("kind").toString();
		r.learned      = QDateTime::fromString(f.value("learned").toString(),
		                                       Qt::ISODate);
		if (r.channel.isEmpty() || r.empty())
			continue;   // a rule for nobody, or that says nothing, is not one
		loaded << r;
	}
	f.endArray();
	m_rules = loaded;
	return true;
}

bool channel_rules::save(const QString &path) const {
	QDir().mkpath(QFileInfo(path).absolutePath());
	QSettings f(path, QSettings::IniFormat);
	f.remove("rules");   // rewritten whole; a shorter list must leave no tail
	f.beginWriteArray("rules", int(m_rules.size()));
	for (int i = 0; i < m_rules.size(); ++i) {
		f.setArrayIndex(i);
		const channel_rule &r = m_rules.at(i);
		f.setValue("channel", r.channel);
		f.setValue("artist", artist_name(r.artist));
		f.setValue("title", title_name(r.title));
		f.setValue("learned", r.learned.toString(Qt::ISODate));
		// Only what is used, so an entry stays short enough to read by eye.
		if (!r.separator.isEmpty())
			f.setValue("separator", r.separator);
		if (!r.fixed_artist.isEmpty())
			f.setValue("fixed_artist", r.fixed_artist);
		if (!r.kind.isEmpty())
			f.setValue("kind", r.kind);
	}
	f.endArray();
	f.sync();
	return f.status() == QSettings::NoError;
}
