#pragma once

#include "download_source.h"

// A download source that accepts anything and finishes only when told, so a job
// reaches a terminal state without a server. It adds no signals of its own -- it
// emits download_source's inherited finished() -- so it needs no moc.
class fake_download_source : public download_source {
public:
	QString id() const override { return "fake"; }
	QString display_name() const override { return "Fake"; }
	source_capabilities capabilities() const override { return {}; }
	bool accepts(const QUrl &, QString *) const override { return true; }
	// Defaults to succeeding, so every existing case is unaffected. A source
	// refusing to start is the fifth way a job reaches a terminal state, and
	// the only one that happens inside `pump`'s sweep.
	bool refuse_start = false;
	bool start(const download_request &, QString *error) override {
		if (!refuse_start)
			return true;
		if (error)
			*error = QStringLiteral("the fake refused to start");
		return false;
	}
	void cancel(int) override {}
	void finish(int job_id, bool ok) { emit finished(job_id, ok, QString()); }
};

// **One definition, included by both suites that need it.** It lived inline in
// `test_settings.cpp` until `test_rotation` needed the same thing to drive a
// download through a real `main_window`. A second copy would have been a
// second thing to keep right, and the member that mattered -- `refuse_start`
// -- had just been added to only one of them.
