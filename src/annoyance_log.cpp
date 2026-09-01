#include "annoyance_log.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>

void annoyance_log::add(const annoyance_report &r) {
	m_reports.append(r);
}

QList<annoyance_report> annoyance_log::for_host(const QString &host) const {
	QList<annoyance_report> out;
	for (const annoyance_report &r : m_reports)
		if (r.host == host)
			out.append(r);
	return out;
}

int annoyance_log::count_for(const QString &host) const {
	int n = 0;
	for (const annoyance_report &r : m_reports)
		if (r.host == host)
			++n;
	return n;
}

void annoyance_log::set_outcome(const QString &host, const QString &outcome) {
	// The most recent for that host, walking backwards. Reports are appended,
	// so the last match is the one just filed.
	for (int i = m_reports.size() - 1; i >= 0; --i) {
		if (m_reports[i].host != host)
			continue;
		m_reports[i].outcome = outcome;
		return;
	}
}

void annoyance_log::clear_host(const QString &host) {
	for (int i = m_reports.size() - 1; i >= 0; --i)
		if (m_reports[i].host == host)
			m_reports.removeAt(i);
}

bool annoyance_log::load(const QString &path) {
	m_reports.clear();
	if (!QFileInfo::exists(path))
		return false;
	QSettings f(path, QSettings::IniFormat);
	const int n = f.beginReadArray("reports");
	for (int i = 0; i < n; ++i) {
		f.setArrayIndex(i);
		annoyance_report r;
		r.host = f.value("host").toString();
		if (r.host.isEmpty())
			continue;   // a report with no site is not a report
		r.when     = QDateTime::fromString(f.value("when").toString(), Qt::ISODate);
		r.page     = f.value("page").toString();
		r.observed = f.value("observed", 0).toInt();
		r.outcome  = f.value("outcome").toString();
		// **Stored as a QStringList and read back as one.** URLs contain commas
		// often enough that splitting a joined string is wrong, and QSettings
		// escapes list elements itself -- which is the whole reason to hand it
		// the list rather than a string we joined.
		r.suspects = f.value("suspects").toStringList();
		// Absent in files written before capability signals existed, which
		// `value` answers with an empty list rather than a failure -- so an old
		// annoyances.ini loads as a report that captured no capabilities,
		// which is exactly what it is.
		r.capabilities = f.value("capabilities").toStringList();
		m_reports.append(r);
	}
	f.endArray();
	return true;
}

bool annoyance_log::save(const QString &path) const {
	QDir().mkpath(QFileInfo(path).absolutePath());
	QSettings f(path, QSettings::IniFormat);
	f.remove("reports");   // rewritten whole; a shrinking array must not leave a tail
	f.beginWriteArray("reports", int(m_reports.size()));
	for (int i = 0; i < m_reports.size(); ++i) {
		f.setArrayIndex(i);
		const annoyance_report &r = m_reports[i];
		f.setValue("host", r.host);
		f.setValue("when", r.when.toString(Qt::ISODate));
		f.setValue("page", r.page);
		f.setValue("observed", r.observed);
		f.setValue("suspects", r.suspects);
		if (!r.capabilities.isEmpty())
			f.setValue("capabilities", r.capabilities);
		if (!r.outcome.isEmpty())
			f.setValue("outcome", r.outcome);
	}
	f.endArray();
	f.sync();
	return f.status() == QSettings::NoError;
}
