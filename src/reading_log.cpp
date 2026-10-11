#include "reading_log.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>

void reading_log::add(const reading_record &r) {
	m_records.push_back(r);
	while (m_records.size() > k_max)
		m_records.removeFirst();
}

QList<reading_record> reading_log::for_channel(const QString &channel) const {
	QList<reading_record> out;
	for (const reading_record &r : m_records)
		if (!channel.isEmpty() && r.channel == channel)
			out << r;
	return out;
}

int reading_log::clear_all() {
	const int n = int(m_records.size());
	m_records.clear();
	return n;
}

bool reading_log::load(const QString &path) {
	if (!QFileInfo::exists(path))
		return false;
	QSettings f(path, QSettings::IniFormat);
	// `allKeys()` first: the parse is lazy, and `status()` asked before any
	// access reports NoError for a file QSettings cannot read.
	f.allKeys();
	if (f.status() != QSettings::NoError) {
		qCritical("readings: %s did not parse; leaving it alone rather than "
		           "treating it as empty", qPrintable(path));
		return false;
	}
	QList<reading_record> loaded;
	const int n = f.beginReadArray("readings");
	for (int i = 0; i < n; ++i) {
		f.setArrayIndex(i);
		reading_record r;
		r.when       = QDateTime::fromString(f.value("when").toString(), Qt::ISODate);
		r.page       = f.value("page").toString();
		r.id         = f.value("id").toString();
		r.extractor  = f.value("extractor").toString();
		r.uploader   = f.value("uploader").toString();
		r.channel    = f.value("channel").toString();
		r.model      = f.value("model").toString();
		r.kind       = f.value("kind").toString();
		r.artist     = f.value("artist").toString();
		r.title      = f.value("title").toString();
		r.is_set     = f.value("is_set").toBool();
		r.tracklist  = f.value("tracklist").toString();
		r.confidence = f.value("confidence").toDouble();
		r.file_name  = f.value("file_name").toString();
		r.from_model = f.value("from_model").toStringList();
		r.rejected   = f.value("rejected").toStringList();
		r.flagged    = f.value("flagged").toStringList();
		if (r.page.isEmpty() && r.id.isEmpty())
			continue;   // a reading of nothing is not a reading
		loaded << r;
	}
	f.endArray();
	m_records = loaded;
	while (m_records.size() > k_max)
		m_records.removeFirst();
	return true;
}

bool reading_log::save(const QString &path) const {
	QDir().mkpath(QFileInfo(path).absolutePath());
	QSettings f(path, QSettings::IniFormat);
	f.remove("readings");   // rewritten whole; a shorter list must leave no tail
	f.beginWriteArray("readings", int(m_records.size()));
	for (int i = 0; i < m_records.size(); ++i) {
		f.setArrayIndex(i);
		const reading_record &r = m_records.at(i);
		f.setValue("when", r.when.toString(Qt::ISODate));
		f.setValue("page", r.page);
		f.setValue("id", r.id);
		f.setValue("extractor", r.extractor);
		f.setValue("uploader", r.uploader);
		f.setValue("channel", r.channel);
		f.setValue("model", r.model);
		f.setValue("kind", r.kind);
		f.setValue("artist", r.artist);
		f.setValue("title", r.title);
		f.setValue("is_set", r.is_set);
		f.setValue("tracklist", r.tracklist);
		f.setValue("confidence", r.confidence);
		f.setValue("file_name", r.file_name);
		// Lists only when there is something in them, so a clean reading's
		// entry stays short enough to read by eye.
		if (!r.from_model.isEmpty())
			f.setValue("from_model", r.from_model);
		if (!r.rejected.isEmpty())
			f.setValue("rejected", r.rejected);
		if (!r.flagged.isEmpty())
			f.setValue("flagged", r.flagged);
	}
	f.endArray();
	f.sync();
	return f.status() == QSettings::NoError;
}
