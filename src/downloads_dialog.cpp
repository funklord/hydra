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
		bar.rect             = option.rect.adjusted(2, 1, -2, -1);
		bar.minimum          = 0;
		bar.maximum          = 100;
		bar.progress         = pct.toInt();
		// The label is drawn by hand rather than by the style. Several styles
		// lay their own text out inside the groove's contents rect, which at
		// row height clips the digits top and bottom and reads as a rendering
		// fault; drawing it over the finished bar is exact.
		bar.textVisible      = false;
		bar.state            = QStyle::State_Enabled;
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

		const int pct = (j.total > 0)
		                    ? int((100 * j.received) / j.total)
		                    : (j.complete() ? 100 : -1);
		row->setData(col_progress, role_percent, pct);
		row->setText(col_progress, pct < 0 ? QString("…")
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
			for (const QString &f : j.files) {
				auto *kid = new QTreeWidgetItem(row);
				kid->setText(col_name, f);
				kid->setFirstColumnSpanned(false);
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
	// Multi-file: take the first playable entry. The feature is usually the
	// largest, but per-file sizes are not in the job model.
	for (const QString &f : j.files) {
		if (!is_playable(f))
			continue;
		if (rel)
			*rel = f;
		return true;
	}
	return false;
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

	// Ask the source to fetch the front first. Without this a torrent downloads
	// in whatever order the swarm offers, and the beginning of the file may be
	// the last thing to arrive — which is the difference between "watchable
	// now" and "watchable when it finishes".
	src->prioritize_streaming(id, rel, true);

	if (!m_proxy->listening()) {
		m_note->setVisible(true);
		m_note->setText("<b>Cannot watch:</b> the local proxy is not listening.");
		return;
	}

	// Serve it through the proxy rather than handing the player the path, so
	// the readable prefix is enforced on every range request. A player pointed
	// straight at a sparse file reads holes as zeros and renders them.
	const int job_id = id;
	const QUrl local = m_proxy->publish_file(
		path, content_type_for(path),
		[src, job_id, rel] { return src->contiguous_bytes(job_id, rel); });
	if (!local.isValid())
		return;

	media_item item;
	item.kind  = media_kind::direct;
	item.url   = QUrl::fromLocalFile(path);
	item.label = QFileInfo(path).fileName();

	QString error;
	if (!m_players->play(item, &error, local)) {
		m_note->setVisible(true);
		m_note->setText("<b>Cannot watch:</b> " + error.toHtmlEscaped());
	}
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
