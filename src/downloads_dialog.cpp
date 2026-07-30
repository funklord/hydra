// SPDX-License-Identifier: GPL-3.0-or-later
#include "downloads_dialog.h"
#include "download_manager.h"

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

downloads_dialog::downloads_dialog(download_manager *downloads, QWidget *parent)
	: QDialog(parent), m_downloads(downloads) {
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
	for (QPushButton *b : { m_pause, m_resume, m_cancel, m_folder }) {
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
		for (QPushButton *b : { m_pause, m_resume, m_cancel, m_folder })
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
