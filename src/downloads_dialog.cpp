// SPDX-License-Identifier: GPL-3.0-or-later
#include "downloads_dialog.h"
#include "download_manager.h"
#include "local_proxy.h"
#include "media_detector.h"
#include "player_launcher.h"

#include <QApplication>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {

enum column { col_name = 0, col_source, col_progress, col_size, col_status };

// Percent for the bar, and the text drawn over it.
constexpr int role_percent = Qt::UserRole + 1;
constexpr int role_job_id  = Qt::UserRole + 2;

QString human_bytes(qint64 n) {
	if (n < 0)
		return "—";
	static const char *unit[] = { "B", "KiB", "MiB", "GiB", "TiB" };
	double v = double(n);
	int u = 0;
	while (v >= 1024.0 && u < 4) {
		v /= 1024.0;
		++u;
	}
	return QString::number(v, 'f', u == 0 ? 0 : 1) + " " + unit[u];
}

QString state_label(download_state s) {
	switch (s) {
		case download_state::queued:    return "Queued";
		case download_state::resolving: return "Starting";
		case download_state::running:   return "Downloading";
		case download_state::paused:    return "Paused";
		case download_state::seeding:   return "Complete — seeding";
		case download_state::done:      return "Complete";
		case download_state::failed:    return "Failed";
		case download_state::cancelled: return "Cancelled";
	}
	return QString();
}

QString content_type_for(const QString &path) {
	static const QHash<QString, QString> by_ext = {
		{"mp4", "video/mp4"},   {"m4v", "video/mp4"},   {"mkv", "video/x-matroska"},
		{"webm", "video/webm"}, {"avi", "video/x-msvideo"}, {"mov", "video/quicktime"},
		{"ts", "video/mp2t"},   {"mpg", "video/mpeg"},  {"mpeg", "video/mpeg"},
		{"mp3", "audio/mpeg"},  {"m4a", "audio/mp4"},   {"flac", "audio/flac"},
		{"ogg", "audio/ogg"},   {"opus", "audio/opus"}, {"wav", "audio/wav"},
	};
	return by_ext.value(QFileInfo(path).suffix().toLower(),
		                   "application/octet-stream");
}

// Draws a real progress bar in the progress column. A downloads list without
// one is legible but wrong-feeling, and this is the cheapest way to get the
// platform's own bar rather than an approximation of it.
class progress_delegate : public QStyledItemDelegate {
public:
	using QStyledItemDelegate::QStyledItemDelegate;

	// Rows must be tall enough for the bar's centred text; at the default
	// height it is clipped top and bottom and reads as a rendering fault.
	QSize sizeHint(const QStyleOptionViewItem &option,
		              const QModelIndex &index) const override {
		QSize s = QStyledItemDelegate::sizeHint(option, index);
		s.setHeight(qMax(s.height(), 22));
		return s;
	}

	void paint(QPainter *painter, const QStyleOptionViewItem &option,
		          const QModelIndex &index) const override {
		const QVariant pct = index.data(role_percent);
		if (!pct.isValid()) {
			QStyledItemDelegate::paint(painter, option, index);
			return;
		}
		QStyleOptionProgressBar bar;
		// Inherit palette, layout direction and font metrics from the item
		// rather than leaving them defaulted, then take the state over.
		static_cast<QStyleOption &>(bar) = option;
		// State_Horizontal is not decoration: Qt styles read it to decide the
		// bar's *orientation*. Without it the bar is drawn vertical, and in a
		// row-shaped rect that shows up as a fill creeping bottom-to-top
		// across the full width instead of left-to-right.
		bar.state            = QStyle::State_Enabled | QStyle::State_Horizontal;
		bar.rect             = option.rect.adjusted(2, 1, -2, -1);
		bar.minimum          = 0;
		bar.maximum          = 100;
		bar.progress         = pct.toInt();
		// The label is drawn by hand rather than by the style. Several styles
		// lay their own text out inside the groove's contents rect, which at
		// row height clips the digits top and bottom and reads as a rendering
		// fault; drawing it over the finished bar is exact.
		bar.textVisible      = false;
		// An indeterminate job — a magnet with no metadata yet — has no
		// meaningful percentage. Qt draws min == max == 0 as a busy bar, which
		// is exactly the honest thing to show.
		if (pct.toInt() < 0) {
			bar.minimum = bar.maximum = 0;
			bar.progress = 0;
		}
		QApplication::style()->drawControl(QStyle::CE_ProgressBar, &bar, painter);

		const QString label = index.data(Qt::DisplayRole).toString();
		if (!label.isEmpty()) {
			painter->save();
			painter->setPen(option.palette.color(QPalette::Active, QPalette::Text));
			painter->drawText(option.rect, Qt::AlignCenter, label);
			painter->restore();
		}
	}
};

}  // namespace

downloads_dialog::downloads_dialog(download_manager *downloads,
                                    player_launcher *players, local_proxy *proxy,
                                    QWidget *parent)
  : QDialog(parent), m_downloads(downloads), m_players(players), m_proxy(proxy) {
	setWindowTitle("Downloads");
	resize(880, 460);
	// A downloads window is something you leave open beside the browser.
	setModal(false);
	setWindowFlag(Qt::Window);

	auto *outer = new QVBoxLayout(this);

	m_list = new QTreeWidget(this);
	m_list->setColumnCount(5);
	m_list->setHeaderLabels({"Name", "Source", "Progress", "Size", "Status"});
	m_list->setRootIsDecorated(true);
	m_list->setUniformRowHeights(false);
	m_list->setAlternatingRowColors(true);
	m_list->setSelectionBehavior(QAbstractItemView::SelectRows);
	m_list->header()->setSectionResizeMode(col_name, QHeaderView::Stretch);
	// Source and Size must never be elided: a truncated "⇅ public" marker
	// defeats the entire point of showing it, and a truncated size is noise.
	m_list->header()->setSectionResizeMode(col_source, QHeaderView::ResizeToContents);
	m_list->header()->setSectionResizeMode(col_size, QHeaderView::ResizeToContents);
	m_list->header()->setSectionResizeMode(col_status, QHeaderView::ResizeToContents);
	m_list->header()->setSectionResizeMode(col_progress, QHeaderView::Fixed);
	m_list->header()->resizeSection(col_progress, 170);
	m_list->setItemDelegateForColumn(col_progress, new progress_delegate(this));
	connect(m_list, &QTreeWidget::itemSelectionChanged,
	         this, &downloads_dialog::update_buttons);
	outer->addWidget(m_list, 1);

	m_note = new QLabel(this);
	m_note->setWordWrap(true);
	m_note->setVisible(false);
	outer->addWidget(m_note);

	// A separate label for what a button press just did. Sharing one with the
	// standing note above does not work: refresh() rewrites that every 200 ms,
	// so anything an action wrote there was erased before it could be read —
	// pressing Watch appeared to do nothing at all.
	m_action = new QLabel(this);
	m_action->setWordWrap(true);
	m_action->setVisible(false);
	outer->addWidget(m_action);

	auto *row = new QHBoxLayout;
	m_pause  = new QPushButton("&Pause", this);
	m_resume = new QPushButton("&Resume", this);
	m_cancel = new QPushButton("&Cancel", this);
	m_folder = new QPushButton("Open &Folder", this);
	m_watch  = new QPushButton("&Watch", this);
	m_watch->setToolTip("Play this while it is still downloading");
	for (QPushButton *b : { m_watch, m_pause, m_resume, m_cancel, m_folder }) {
		b->setEnabled(false);
		row->addWidget(b);
	}
	row->addStretch(1);
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::hide);
	row->addWidget(buttons);
	outer->addLayout(row);

	connect(m_pause,  &QPushButton::clicked, this, &downloads_dialog::act_pause);
	connect(m_resume, &QPushButton::clicked, this, &downloads_dialog::act_resume);
	connect(m_cancel, &QPushButton::clicked, this, &downloads_dialog::act_cancel);
	connect(m_folder, &QPushButton::clicked, this, &downloads_dialog::act_open_folder);
	connect(m_watch,  &QPushButton::clicked, this, &downloads_dialog::act_watch);

	// changed() fires on every chunk of every transfer. Repainting the tree at
	// that rate is pure waste, so bursts are collapsed into one refresh.
	m_coalesce = new QTimer(this);
	m_coalesce->setSingleShot(true);
	m_coalesce->setInterval(200);
	connect(m_coalesce, &QTimer::timeout, this, &downloads_dialog::refresh);
	connect(m_downloads, &download_manager::changed,
	         this, &downloads_dialog::schedule_refresh);

	refresh();
}

void downloads_dialog::schedule_refresh() {
	if (!m_coalesce->isActive())
		m_coalesce->start();
}

void downloads_dialog::refresh() {
	bool any_public = false;

	for (const download_job &j : m_downloads->jobs()) {
		QTreeWidgetItem *row = m_rows.value(j.id, nullptr);
		if (!row) {
			row = new QTreeWidgetItem(m_list);
			row->setData(0, role_job_id, j.id);
			m_rows.insert(j.id, row);
		}

		// Before a source knows a filename there is nothing better than the
		// address itself — and it must be the *whole* address, since stripping
		// the query off a magnet link leaves the useless string "magnet:".
		// Elision is the view's job, not ours.
		const QString name = j.path.isEmpty() ? j.url.toString()
		                                      : QFileInfo(j.path).fileName();
		row->setText(col_name, name);
		row->setToolTip(col_name, j.url.toString());

		// Source label comes from the source itself, never from a test on the
		// URL — the seam's rule (§11.4) applies to the UI as much as the model.
		download_source *src = m_downloads->source_by_id(j.source_id);
		QString source_text = src ? src->display_name() : j.source_id;
		if (j.public_participation) {
			any_public = true;
			// The visible difference the §11.4 decision requires. It is on the
			// row, not in a one-time dialog, because the transfer keeps
			// announcing for as long as it exists.
			source_text += "  ⇅ public";
			row->setForeground(col_source, QBrush(QColor(0xb0, 0x60, 0x00)));
			row->setToolTip(col_source,
			                 src ? src->capabilities().participation_note
			                     : QString("Your address is visible to others."));
		}
		row->setText(col_source, source_text);

		// A job with no known total draws a busy bar, which says "working".
		// That is right for a magnet resolving metadata and wrong for one that
		// has failed or been cancelled — an animation on a dead transfer reads
		// as activity that is not happening.
		int pct;
		if (j.total > 0)
			pct = int((100 * j.received) / j.total);
		else if (j.complete())
			pct = 100;
		else if (j.terminal())
			pct = 0;      // stopped, and nothing is known about how far it got
		else
			pct = -1;     // genuinely indeterminate, and genuinely still going
		row->setData(col_progress, role_percent, pct);
		row->setText(col_progress,
		              pct < 0 ? QString("…")
		                      : (j.terminal() && !j.complete() && j.total <= 0)
		                            ? QString("—")
		                            : QString::number(pct) + "%");

		row->setText(col_size, j.total > 0
		                            ? human_bytes(j.received) + " / " + human_bytes(j.total)
		                            : human_bytes(j.received));

		QString status = state_label(j.status);
		if (j.status == download_state::failed && !j.error.isEmpty())
			status += " — " + j.error;
		else if (!j.detail.isEmpty())
			status += " — " + j.detail;
		row->setText(col_status, status);
		row->setToolTip(col_status, j.node_id.isEmpty()
		                                 ? QString("Not associated with a tab")
		                                 : QString("From tab %1").arg(j.node_id));

		// Multi-file jobs list their files as children, once the file list is
		// known — for a magnet that is only after metadata arrives.
		if (!j.files.isEmpty() && row->childCount() != j.files.size()) {
			while (row->childCount())
				delete row->takeChild(0);
			QString chosen;
			const bool any = find_playable(j, &chosen);
			for (const download_file &f : j.files) {
				auto *kid = new QTreeWidgetItem(row);
				kid->setText(col_name, f.path);
				kid->setText(col_size, human_bytes(f.size));
				// Mark the one Watch would play, so the choice is visible
				// rather than something the user has to infer.
				if (any && f.path == chosen) {
					kid->setText(col_status, "would be played");
					QFont bold = kid->font(col_name);
					bold.setBold(true);
					kid->setFont(col_name, bold);
				}
			}
		}
	}

	// With nothing current the action buttons are all dead, which reads as a
	// broken window rather than an empty selection.
	if (!m_list->currentItem() && m_list->topLevelItemCount())
		m_list->setCurrentItem(m_list->topLevelItem(0));

	m_note->setVisible(any_public);
	if (any_public)
		m_note->setText(
		  "<b>⇅ public</b> — those transfers announce your IP address to "
		  "other people and are not private. Hydra does not tunnel them; use "
		  "a system-level VPN or proxy if you need that.");

	update_buttons();
}

int downloads_dialog::selected_job() const {
	QTreeWidgetItem *item = m_list->currentItem();
	if (!item)
		return 0;
	// A child row is a file inside a job; the job is its parent.
	if (item->parent())
		item = item->parent();
	return item->data(0, role_job_id).toInt();
}

void downloads_dialog::update_buttons() {
	const int id = selected_job();
	const download_job *job = nullptr;
	for (const download_job &j : m_downloads->jobs())
		if (j.id == id)
			job = &j;

	if (!job) {
		for (QPushButton *b : { m_watch, m_pause, m_resume, m_cancel, m_folder })
			b->setEnabled(false);
		return;
	}

	download_source *src = m_downloads->source_by_id(job->source_id);
	const bool resumable = src && src->capabilities().resumable;

	m_pause->setEnabled(resumable && !job->terminal() &&
	                     job->status != download_state::paused);
	m_resume->setEnabled(resumable && job->status == download_state::paused);
	// Cancel covers a seeding job too: it is complete but still working, and
	// stopping it is exactly what a user would expect Cancel to do there.
	m_cancel->setEnabled(!job->terminal());
	m_folder->setEnabled(!job->path.isEmpty());

	// Watch is offered when the source says a partial file is usable and the
	// job contains something worth playing. Whether that is a torrent or an
	// ordinary download is not asked and does not matter.
	const bool streamable = src && src->capabilities().streamable;
	m_watch->setEnabled(streamable && m_players && m_proxy &&
	                     find_playable(*job, nullptr));
}

// Which file in a job is worth playing. This is a media judgement rather than a
// transport one, so it belongs above the seam: the source knows how bytes
// arrive, not which of them is a film.
bool downloads_dialog::find_playable(const download_job &j, QString *rel) const {
	static const QStringList playable = {
		"mp4", "mkv", "webm", "avi", "mov", "m4v", "ts", "mpg", "mpeg",
		"mp3", "m4a", "flac", "ogg", "opus", "wav",
	};
	auto is_playable = [](const QString &p) {
		return playable.contains(QFileInfo(p).suffix().toLower());
	};

	if (rel)
		rel->clear();
	if (j.files.isEmpty())
		return is_playable(j.path);          // the job's own path is the file

	// The **largest** playable file, not the first. Releases routinely ship a
	// short sample clip that sorts ahead of the feature, and picking by order
	// would play the sample. Ties and unknown sizes fall back to order, which
	// is the best available answer rather than a wrong one.
	const download_file *best = nullptr;
	for (const download_file &f : j.files) {
		if (!is_playable(f.path))
			continue;
		if (!best || f.size > best->size)
			best = &f;
	}
	if (!best)
		return false;
	if (rel)
		*rel = best->path;
	return true;
}

void downloads_dialog::act_pause() {
	if (const int id = selected_job())
		m_downloads->pause(id);
}

void downloads_dialog::act_resume() {
	if (const int id = selected_job())
		m_downloads->unpause(id);
}

void downloads_dialog::act_cancel() {
	if (const int id = selected_job())
		m_downloads->cancel(id);
}

void downloads_dialog::act_watch() {
	const int id = selected_job();
	const download_job *job = nullptr;
	for (const download_job &j : m_downloads->jobs())
		if (j.id == id)
			job = &j;
	if (!job || !m_players || !m_proxy)
		return;

	download_source *src = m_downloads->source_by_id(job->source_id);
	if (!src)
		return;

	// The file to play, and where it actually lives. For a single-file job the
	// job's own path is the file; for a multi-file one the entries are relative
	// to the download directory.
	QString rel;
	if (!find_playable(*job, &rel))
		return;
	const QString path = rel.isEmpty()
	                         ? job->path
	                         : QDir(m_downloads->directory()).filePath(rel);
	if (path.isEmpty())
		return;

	if (!m_proxy->listening()) {
		m_action->setVisible(true);
		m_action->setText("<b>Cannot watch:</b> the local proxy is not listening.");
		return;
	}

	// Ask the source to fetch the front first. Without this a torrent downloads
	// in whatever order the swarm offers, and the beginning of the file may be
	// the last thing to arrive — which is the difference between "watchable
	// now" and "watchable when it finishes".
	src->prioritize_streaming(id, rel, true);

	m_watch_job   = id;
	m_watch_rel   = rel;
	m_watch_path  = path;
	m_watch_ticks = 0;
	m_watch->setEnabled(false);

	if (!m_watch_wait) {
		m_watch_wait = new QTimer(this);
		m_watch_wait->setInterval(500);
		connect(m_watch_wait, &QTimer::timeout, this,
		         &downloads_dialog::try_launch_watch);
	}
	try_launch_watch();          // it may already be ready
}

void downloads_dialog::try_launch_watch() {
	const download_job *job = nullptr;
	for (const download_job &j : m_downloads->jobs())
		if (j.id == m_watch_job)
			job = &j;
	download_source *src = job ? m_downloads->source_by_id(job->source_id) : nullptr;
	if (!job || !src) {
		m_watch_wait->stop();
		return;
	}

	// How much of the *front of this file* is genuinely readable. -1 means the
	// source writes front-to-back and the file's own size is the truth.
	qint64 have = src->contiguous_bytes(m_watch_job, m_watch_rel);
	if (have < 0)
		have = QFileInfo(m_watch_path).size();

	// Enough for a demuxer to find its headers and a keyframe, and enough that
	// playback does not stall a second later. Sequential order was requested
	// above, so this fills from the front rather than at random.
	static constexpr qint64 k_lead = 1024 * 1024;
	const bool ready = job->complete() || have >= k_lead;

	if (!ready) {
		if (++m_watch_ticks > 120) {         // a minute of getting nowhere
			m_watch_wait->stop();
			m_watch->setEnabled(true);
			m_action->setVisible(true);
			m_action->setText("<b>Cannot watch yet:</b> the start of the file is "
			                   "not arriving. It will play once more of it has "
			                   "downloaded.");
			return;
		}
		m_watch_wait->start();
		m_action->setVisible(true);
		m_action->setText(QString("Preparing to play — %1 of the start ready, "
		                           "waiting for %2.")
		                      .arg(human_bytes(have), human_bytes(k_lead)));
		return;
	}

	m_watch_wait->stop();
	m_watch->setEnabled(true);

	// Serve it through the proxy rather than handing the player the path, so
	// the readable prefix is enforced on every range request. A player pointed
	// straight at a sparse file reads holes as zeros and renders them.
	const int     job_id = m_watch_job;
	const QString rel    = m_watch_rel;

	// The eventual size of the *file*, which for a multi-file job is not the
	// job's total. Without it the proxy can only promise what has arrived, and
	// a player that reaches that point sees a complete response and stops.
	qint64 eventual = -1;
	for (const download_file &f : job->files) {
		if (f.path == rel) {
			eventual = f.size;
			break;
		}
	}
	if (eventual < 0 && job->files.isEmpty())
		eventual = job->total;          // single-file job: the job is the file

	download_manager *dm = m_downloads;
	const QUrl local = m_proxy->publish_file(
	  m_watch_path, content_type_for(m_watch_path),
	  [src, job_id, rel] { return src->contiguous_bytes(job_id, rel); },
	  [eventual, dm, job_id, rel]() -> qint64 {
		  if (eventual >= 0)
			  return eventual;
		  // Fall back to whatever the job now knows — a magnet has no sizes
		  // until metadata lands, which can be after Watch was pressed.
		  for (const download_job &j : dm->jobs()) {
			  if (j.id != job_id)
				  continue;
			  for (const download_file &f : j.files)
				  if (f.path == rel)
					  return f.size;
			  return j.files.isEmpty() ? j.total : -1;
		  }
		  return -1;
	  });
	if (!local.isValid()) {
		m_action->setVisible(true);
		m_action->setText("<b>Cannot watch:</b> the local proxy refused to "
		                   "publish the file.");
		return;
	}

	media_item item;
	item.kind  = media_kind::direct;
	item.url   = QUrl::fromLocalFile(m_watch_path);
	item.label = QFileInfo(m_watch_path).fileName();

	QString error;
	if (!m_players->play(item, &error, local)) {
		m_action->setVisible(true);
		m_action->setText("<b>Cannot watch:</b> " + error.toHtmlEscaped());
		return;
	}
	m_action->setVisible(true);
	m_action->setText(QString("Playing %1 in %2, from %3 downloaded so far.")
	                      .arg(item.label, m_players->selected(), human_bytes(have)));
}

void downloads_dialog::act_open_folder() {
	const int id = selected_job();
	for (const download_job &j : m_downloads->jobs()) {
		if (j.id != id || j.path.isEmpty())
			continue;
		// The containing folder, never the file. Swarms and web servers carry
		// whatever is in them, and the standing rule (§11.4) is that a download
		// is written to disk and not opened by us.
		const QFileInfo fi(j.path);
		const QString dir = fi.isDir() ? fi.absoluteFilePath()
		                               : fi.absolutePath();
		QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
		return;
	}
}
