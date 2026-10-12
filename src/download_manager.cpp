#include "download_manager.h"
#include "media_remux.h"

#include <QDir>

#include <QStandardPaths>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>

download_manager::download_manager(QObject *parent) : QObject(parent) {
	m_dir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
}

void download_manager::add_source(download_source *source) {
	if (!source || m_sources.contains(source))
		return;
	source->setParent(this);
	m_sources.push_back(source);
	connect(source, &download_source::progressed, this, &download_manager::on_progress);
	connect(source, &download_source::finished,   this, &download_manager::on_finished);
}

download_source *download_manager::source_by_id(const QString &id) const {
	for (download_source *s : m_sources)
		if (s->id() == id)
			return s;
	return nullptr;
}

download_source *download_manager::source_for(const QUrl &url) const {
	for (download_source *s : m_sources)
		if (s->accepts(url))
			return s;
	return nullptr;
}

download_job *download_manager::find(int id) {
	for (download_job &j : m_jobs)
		if (j.id == id)
			return &j;
	return nullptr;
}

int download_manager::live_count(const QString &source_id) const {
	int n = 0;
	for (const download_job &j : m_jobs)
		if (j.source_id == source_id && m_live.contains(j.id))
			++n;
	return n;
}

bool download_manager::has_consent(const QString &source_id) const {
	return m_consented.contains(source_id);
}

void download_manager::set_consent(const QString &source_id, bool granted) {
	if (granted)
		m_consented.insert(source_id);
	else
		m_consented.remove(source_id);
	pump();
}

download_manager::pair_names
download_manager::names_for_pair(const QString &picture_name,
                                 const QString &sound_name) {
	auto split = [](const QString &name, QString *stem, QString *ext) {
		const int dot = name.lastIndexOf('.');
		*stem = dot > 0 ? name.left(dot) : name;
		*ext  = dot > 0 ? name.mid(dot + 1).toLower() : QString();
	};
	QString stem, picture_ext, sound_stem, sound_ext;
	split(picture_name, &stem, &picture_ext);
	split(sound_name, &sound_stem, &sound_ext);
	// **The container both halves fit, unconverted.** H.264 with AAC is MP4's
	// own pair and VP9 with Opus is WebM's; anything mixed goes to Matroska,
	// which takes all of them. Chosen by extension because that is what
	// yt-dlp reports, and a copy into the wrong container fails rather than
	// converting -- which `media_remux` reports, keeping the picture.
	QString combined_ext = QStringLiteral("mkv");
	if (picture_ext == "mp4" && (sound_ext == "m4a" || sound_ext == "mp4"))
		combined_ext = QStringLiteral("mp4");
	else if (picture_ext == "webm" && sound_ext == "webm")
		combined_ext = QStringLiteral("webm");
	auto with = [](const QString &s, const QString &part, const QString &e) {
		return e.isEmpty() ? s + part : s + part + '.' + e;
	};
	return { with(stem, ".picture", picture_ext),
	         with(stem, ".sound", sound_ext),
	         stem + '.' + combined_ext };
}

int download_manager::enqueue_pair(const QUrl &picture, const QUrl &sound,
                                   const QString &node_id, QString *error,
                                   const QMap<QString, QString> &headers,
                                   const QString &picture_name,
                                   const QString &sound_name,
                                   const QString &combined_name,
                                   const download_hints &picture_hints,
                                   const download_hints &sound_hints) {
	// The same pair asked for again is the one already running.
	if (!picture_hints.separate)
		for (const download_job &j : m_jobs)
			if (j.url == picture && j.sound_job && !j.terminal())
				return j.id;
	// **Its own sound, always.** The same address is otherwise one download,
	// so a pair asked for while that sound was saving alone took over that
	// job -- and the combine then removed the file the person had asked for.
	download_hints own = sound_hints;
	own.separate = true;
	const int sound_id = enqueue(sound, node_id, error, headers, sound_name, own);
	if (!sound_id)
		return 0;
	const int picture_id = enqueue(picture, node_id, error, headers, picture_name,
	                               picture_hints);
	if (!picture_id) {
		cancel(sound_id);
		return 0;
	}
	if (download_job *s = find(sound_id))
		s->picture_job = picture_id;
	if (download_job *p = find(picture_id)) {
		p->sound_job     = sound_id;
		p->combined_name = combined_name;
	}
	return picture_id;
}

void download_manager::settle_pair(int picture_id) {
	download_job *p = find(picture_id);
	if (!p || !p->sound_job)
		return;
	const download_job *s = find(p->sound_job);
	// Landed: its source has let go and it was held open rather than closed.
	// Queued jobs are not live either, which is why the status is asked too.
	const bool landed = p->status == download_state::running &&
	                    !m_live.contains(p->id);
	// The picture is held open until this decides, so it is the one row that
	// says what happened to the pair.
	if (!s || s->status == download_state::failed ||
	    s->status == download_state::cancelled) {
		if (landed) {
			p->status = download_state::done;
			p->detail = QString("Saved without its sound: the sound %1.")
			                .arg(s && s->status == download_state::failed
			                       ? "failed (" + s->error + ")"
			                       : QStringLiteral("was cancelled"));
		}
		return;
	}
	if (!s->complete() || !landed || p->detail.startsWith("Combining"))
		return;
	const QString out = QDir(QFileInfo(p->path).absolutePath())
	                      .filePath(p->combined_name);
	p->detail = QStringLiteral("Combining picture and sound…");
	const int sound_id = s->id;
	// The tracklist rides along in the same ffmpeg run.
	const QString meta = write_chapters(out, p->chapters);
	auto *mux = new media_remux(this);
	connect(mux, &media_remux::finished, this,
	         [this, mux, picture_id, sound_id, meta](bool ok, const QString &path,
	                                                 const QString &message) {
		mux->deleteLater();
		if (!meta.isEmpty())
			QFile::remove(meta);
		if (download_job *p = find(picture_id)) {
			p->status = download_state::done;
			p->path   = path;
			p->detail = !ok ? message
			  : meta.isEmpty() ? QStringLiteral("Picture and sound, combined.")
			  : QStringLiteral("Picture and sound, combined, with the tracklist "
			                   "as chapters.");
		}
		// Its file is inside the combined one now, and a row pointing at a
		// file that is gone is a row nobody can act on.
		if (ok)
			forget(sound_id);
		emit changed();
		persist_history();
	});
	mux->start_mux(p->path, s->path, out, meta);
	emit changed();
}

QString download_manager::write_chapters(const QString &path,
                                         const QString &chapters) {
	if (chapters.isEmpty())
		return QString();
	const QString meta = path + QStringLiteral(".chapters.txt");
	QFile f(meta);
	if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
	    f.write(chapters.toUtf8()) < 0 || !f.flush()) {
		QFile::remove(meta);
		return QString();
	}
	return meta;
}

void download_manager::add_chapters(int id) {
	download_job *j = find(id);
	if (!j)
		return;
	const QString in   = j->path;
	const QString meta = write_chapters(in, j->chapters);
	if (meta.isEmpty()) {
		j->status = download_state::done;
		j->detail = QStringLiteral("Saved; the tracklist could not be written "
		                            "beside it, so it has no chapters.");
		return;
	}
	// A copy beside it, then put in its place: the name stays the one the
	// download was given, and a failure leaves the original as it was.
	const QFileInfo fi(in);
	const QString out = fi.dir().filePath(fi.completeBaseName() +
	                                      QStringLiteral(".chaptered.") + fi.suffix());
	j->detail = QStringLiteral("Adding the tracklist as chapters…");
	auto *step = new media_remux(this);
	connect(step, &media_remux::finished, this,
	         [this, step, id, in, out, meta](bool ok, const QString &,
	                                         const QString &message) {
		step->deleteLater();
		QFile::remove(meta);
		// `in` is gone on success, so the copy is the only file and takes its
		// name; where it cannot, it is kept as it is rather than lost.
		const bool placed = ok && QFile::rename(out, in);
		if (download_job *j = find(id)) {
			j->status = download_state::done;
			j->path   = (ok && !placed) ? out : in;
			j->detail = placed ? QStringLiteral("Saved, with the tracklist as "
			                                    "chapters.")
			          : ok ? QStringLiteral("Saved with chapters, under a "
			                                "different name: the original name "
			                                "could not be taken.")
			               : "Saved without chapters: " + message;
		}
		emit changed();
		persist_history();
	});
	step->start_chapters(in, meta, out);
}

int download_manager::enqueue(const QUrl &url, const QString &node_id,
                               QString *error,
                               const QMap<QString, QString> &headers,
                               const QString &file_name,
                               const download_hints &hints) {
	// **The same address twice is one download, not two.**
	//
	// Reported from use: clicking a magnet link sometimes produced two rows
	// in the downloads list and sometimes one. There was no check here at
	// all -- a source accepted the url and a job was built, however many
	// times it was asked -- so two calls made two jobs of the same transfer.
	// Why the engine's external-url handler fires twice for some links and
	// once for others is a separate question and still open; this is the
	// half that can be answered where the jobs are made.
	//
	// **Only while the existing one is still going.** `is_terminal` is done,
	// failed or cancelled, and a job in one of those states is history: asking
	// for that address again is a retry somebody meant, and refusing it would
	// make a failed download unrepeatable.
	//
	// The existing id rather than 0, because 0 is this function's failure
	// value and the caller turns it into "Nothing here can download that".
	// A second click is not an error -- it is a request to see the download
	// that is already running, which is what the caller does with an id.
	if (!hints.separate)
		for (const download_job &j : m_jobs)
			if (j.url == url && !j.terminal())
				return j.id;

	// First source that accepts wins. If none does, report the most specific
	// reason offered rather than a generic refusal -- the sources know why.
	download_source *chosen = nullptr;
	QString first_reason;
	for (download_source *s : m_sources) {
		QString why_not;
		if (hints.known_media ? s->accepts_known_media(url, &why_not)
		                : s->accepts(url, &why_not)) {
			chosen = s;
			break;
		}
		if (first_reason.isEmpty() && !why_not.isEmpty())
			first_reason = why_not;
	}
	if (!chosen) {
		if (error)
			*error = first_reason.isEmpty()
			             ? QString("Nothing here can download that address.")
			             : first_reason;
		return 0;
	}

	const source_capabilities caps = chosen->capabilities();

	download_job job;
	job.id                   = m_next_id++;
	job.url                  = url;
	job.node_id              = node_id;
	job.source_id            = chosen->id();
	job.status               = download_state::queued;
	job.public_participation = caps.public_participation;
	job.headers              = headers;
	job.file_name            = file_name;
	job.chunk_size           = hints.chunk_size;
	job.chapters             = hints.chapters;

	m_jobs.push_back(job);
	emit changed();

	// Ask before the first publicly-observable job of this source, not after.
	if (caps.public_participation && !has_consent(job.source_id))
		emit consent_required(job.source_id, caps.participation_note, job.id);

	pump();
	return job.id;
}

int download_manager::adopt(download_source *source, const QUrl &url,
                             const QString &node_id) {
	if (!source)
		return 0;
	if (!m_sources.contains(source))
		add_source(source);

	download_job job;
	job.id                   = m_next_id++;
	job.url                  = url;
	job.node_id              = node_id;
	job.source_id            = source->id();
	job.status               = download_state::running;
	job.public_participation = source->capabilities().public_participation;

	m_jobs.push_back(job);
	// Live from the outset: the transport is running, so finished() must be
	// able to retire it and sweep() must never try to start it.
	m_live.insert(job.id);
	emit changed();
	return job.id;
}

void download_manager::pump() {
	// A source may fail synchronously inside start(), which lands in
	// on_finished() and calls back in here mid-iteration. Rather than reason
	// about what that does to the loop, coalesce: the inner call asks for
	// another sweep and the outer one performs it.
	if (m_pumping) {
		m_pump_again = true;
		return;
	}
	m_pumping = true;
	do {
		m_pump_again = false;
		sweep();
	} while (m_pump_again);
	m_pumping = false;
	if (m_history_dirty) {
		m_history_dirty = false;
		persist_history();
	}
}

void download_manager::sweep() {
	for (download_job &j : m_jobs) {
		if (j.status != download_state::queued)
			continue;
		download_source *s = source_by_id(j.source_id);
		if (!s)
			continue;
		const source_capabilities caps = s->capabilities();
		if (caps.public_participation && !has_consent(j.source_id))
			continue;   // held until the user is told and agrees
		if (live_count(j.source_id) >= qMax(1, caps.max_concurrent))
			continue;

		download_request req;
		req.id        = j.id;
		req.url       = j.url;
		req.directory = m_dir;
		req.node_id   = j.node_id;
		req.headers   = j.headers;
		req.file_name = j.file_name;
		req.chunk_size = j.chunk_size;

		QString error;
		m_live.insert(j.id);
		if (!s->start(req, &error)) {
			m_live.remove(j.id);
			j.status = download_state::failed;
			j.error  = error;
			// Terminal, so it belongs in the history like any other
			// finish -- see `m_history_dirty`, which `pump()` writes once
			// after the loop rather than once per refusal.
			m_history_dirty = true;
			emit changed();
			continue;
		}
		// A source may have moved the job along synchronously inside start();
		// only claim "running" if it did not say otherwise.
		if (j.status == download_state::queued)
			j.status = download_state::running;
		emit changed();
	}
}

void download_manager::on_progress(int id, const download_progress &p) {
	download_job *j = find(id);
	if (!j || j->terminal())
		return;
	j->received = p.received;
	if (p.total >= 0)
		j->total = p.total;
	if (!p.path.isEmpty())
		j->path = p.path;
	if (!p.files.isEmpty())
		j->files = p.files;
	j->detail = p.detail;
	j->status = p.state;
	emit changed();
}

void download_manager::on_finished(int id, bool ok, const QString &message) {
	m_live.remove(id);
	if (download_job *j = find(id)) {
		if (j->status == download_state::cancelled) {
			// A cancellation is the user's decision and final. A source that
			// reports success afterwards is describing what it managed before
			// stopping, not undoing the cancel -- and this used to overwrite it,
			// so cancelling a capture showed as "Complete". HTTP never exposed
			// it because aborting a reply reports failure; a capture reports
			// success whenever any bytes were written.
		} else if (ok && j->sound_job) {
			// Not done until its sound is with it.
			j->status = download_state::running;
			j->detail = QStringLiteral("Waiting for its sound…");
		} else if (ok && !j->chapters.isEmpty() && !j->picture_job &&
		           media_remux::available()) {
			// Not done until its tracklist is in it. A sound fetched for a
			// picture gets the chapters through the combine instead.
			j->status = download_state::running;
			add_chapters(id);
		} else if (ok) {
			j->status = download_state::done;
		} else {
			j->status = download_state::failed;
			j->error  = message;
		}
	}
	if (const download_job *j = find(id)) {
		const int picture = j->sound_job ? j->id : j->picture_job;
		if (picture)
			settle_pair(picture);
	}
	emit changed();
	persist_history();
	pump();
}

void download_manager::cancel(int id) {
	download_job *j = find(id);
	if (!j || j->terminal())
		return;
	// Half a pair is not wanted without the other half.
	if (j->sound_job)
		cancel(j->sound_job);
	j = find(id);   // the list is unchanged, but re-asked after a call that emits
	if (m_live.contains(id)) {
		j->status = download_state::cancelled;
		if (download_source *s = source_by_id(j->source_id))
			s->cancel(id);           // finished() cleans up and pumps
	} else {
		j->status = download_state::cancelled;
		emit changed();
		persist_history();
	}
}

bool download_manager::forget(int id) {
	for (int i = 0; i < m_jobs.size(); ++i) {
		if (m_jobs[i].id != id)
			continue;
		if (!m_jobs[i].terminal())
			return false;   // a running download does not vanish from the list
		m_jobs.removeAt(i);
		emit changed();
		persist_history();
		return true;
	}
	return false;
}

int download_manager::forget_finished() {
	int removed = 0;
	// Backwards, so removing one does not shift an index still to be
	// checked. terminal() is the same test forget() uses, so a live job --
	// queued or running -- is left exactly as Remove leaves it.
	for (int i = m_jobs.size() - 1; i >= 0; --i) {
		if (m_jobs[i].terminal()) {
			m_jobs.removeAt(i);
			++removed;
		}
	}
	if (removed > 0) {
		emit changed();
		persist_history();
	}
	return removed;
}

static QString state_name(download_state s) {
	switch (s) {
	case download_state::done:      return QStringLiteral("done");
	case download_state::failed:    return QStringLiteral("failed");
	case download_state::cancelled: return QStringLiteral("cancelled");
	default:                        return QString();
	}
}

static download_state state_from_name(const QString &n) {
	if (n == QLatin1String("done"))      return download_state::done;
	if (n == QLatin1String("failed"))    return download_state::failed;
	if (n == QLatin1String("cancelled")) return download_state::cancelled;
	return download_state::queued;   // a non-terminal sentinel, rejected on load
}

void download_manager::persist_history() {
	if (!m_history_path.isEmpty() && !save_history(m_history_path))
		emit save_failed();
}

bool download_manager::save_history(const QString &path) const {
	QList<const download_job *> hist;
	for (const download_job &j : m_jobs)
		if (j.terminal())
			hist << &j;
	// Bound the file: keep the most recent, so a long-lived profile does not
	// accumulate a download record without limit.
	const int cap = 200;
	const int from = hist.size() > cap ? hist.size() - cap : 0;
	QJsonArray arr;
	for (int i = from; i < hist.size(); ++i) {
		const download_job &j = *hist[i];
		QJsonObject o;
		o.insert("url",    j.url.toString());
		o.insert("source", j.source_id);
		o.insert("path",   j.path);
		o.insert("node",   j.node_id);
		o.insert("received", double(j.received));
		o.insert("total",    double(j.total));
		o.insert("status", state_name(j.status));
		if (!j.error.isEmpty())
			o.insert("error", j.error);
		o.insert("public", j.public_participation);
		arr.append(o);
	}
	// Atomic, so a crash mid-write cannot leave a half-file that fails to parse
	// and loses the whole history.
	QSaveFile f(path);
	if (!f.open(QIODevice::WriteOnly))
		return false;
	const QByteArray json = QJsonDocument(arr).toJson(QJsonDocument::Compact);
	if (f.write(json) != json.size())
		return false;   // the destructor discards; the previous file stays
	return f.commit();
}

bool download_manager::load_history(const QString &path) {
	QFile f(path);
	// An absent file is an ordinary first run and is not a reason to stop
	// writing to it; a file that is there and will not open is. Only the
	// second answers false, which is the contract the other stores here keep.
	if (!f.open(QIODevice::ReadOnly))
		return !QFileInfo::exists(path);
	// **A damaged file is not a short one**, which is the distinction
	// `policy_engine` already had to make. `QJsonDocument::fromJson` answers
	// a truncated or corrupt file with an empty array, and an empty array is
	// exactly what a profile with no finished downloads looks like -- so
	// without the parse error the two are indistinguishable and the next
	// finished download writes the empty one over the real one.
	QJsonParseError err{};
	const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
	if (err.error != QJsonParseError::NoError || !doc.isArray()) {
		qCritical("downloads: %s did not parse; leaving it alone rather than "
		           "treating it as empty", qPrintable(path));
		return false;
	}
	const QJsonArray arr = doc.array();
	for (const QJsonValue &v : arr) {
		const QJsonObject o = v.toObject();
		download_job j;
		j.status = state_from_name(o.value("status").toString());
		if (!j.terminal())
			continue;   // only history is restored, never a fake running row
		j.id        = m_next_id++;
		j.url       = QUrl(o.value("url").toString());
		j.source_id = o.value("source").toString();
		j.path      = o.value("path").toString();
		j.node_id   = o.value("node").toString();
		j.received  = qint64(o.value("received").toDouble());
		j.total     = qint64(o.value("total").toDouble(-1));
		j.error     = o.value("error").toString();
		j.public_participation = o.value("public").toBool();
		m_jobs.push_back(j);
	}
	emit changed();
	return true;
}

void download_manager::pause(int id) {
	download_job *j = find(id);
	if (!j || !m_live.contains(id))
		return;
	download_source *s = source_by_id(j->source_id);
	if (!s || !s->capabilities().pausable)
		return;
	s->pause(id);
}

void download_manager::unpause(int id) {
	download_job *j = find(id);
	if (!j)
		return;
	download_source *s = source_by_id(j->source_id);
	if (!s || !s->capabilities().pausable)
		return;
	if (m_live.contains(id)) {
		s->unpause(id);
	} else if (j->status == download_state::paused) {
		j->status = download_state::queued;
		pump();
	}
}
