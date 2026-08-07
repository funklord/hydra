// SPDX-License-Identifier: GPL-3.0-or-later
#include "media_dialog.h"

#include "empty_state.h"
#include "download_manager.h"
#include "mse_tap.h"
#include "player_launcher.h"
#include "local_proxy.h"
#include "hls_assembler.h"

#include <QDir>

#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLocale>
#include <QEvent>
#include <QLabel>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {

QString kind_label(media_kind k) {
	switch (k) {
		case media_kind::hls:     return "HLS stream";
		case media_kind::dash:    return "DASH stream";
		case media_kind::direct:  return "File";
		case media_kind::segment: return "Segment";
	}
	return "Unknown";
}

}  // namespace

media_dialog::media_dialog(media_detector *detector, player_launcher *players,
                            download_manager *downloads, local_proxy *proxy,
                            mse_tap *tap, QWidget *parent)
  : QDialog(parent), m_detector(detector), m_players(players),
    m_downloads(downloads), m_proxy(proxy), m_tap(tap) {
	setWindowTitle("Media on this page");
	resize(720, 340);

	auto *outer = new QVBoxLayout(this);

	m_list = new QTreeWidget(this);
	m_list->setColumnCount(4);
	m_list->setHeaderLabels({"Type", "Name", "Host", ""});
	m_list->setRootIsDecorated(false);
	m_list->header()->setStretchLastSection(false);
	m_list->header()->setSectionResizeMode(1, QHeaderView::Stretch);
	outer->addWidget(m_list, 1);

	// **The empty state goes over the rows area, not under it.** It used to be
	// one of `m_status`'s texts, which left a dialog-sized black table with the
	// explanation stranded beneath it -- the tab tree and the downloads dialog
	// both centre theirs in the space that is empty, and this was the odd one
	// out. Nobody had seen it because the media dialog needs a loaded page to
	// photograph, so the capture pass had always skipped it -- `try_look` runs
	// against a local fixture now, so it does not.
	//
	// The placement is `empty_state`'s business; see its header for why the
	// overlay is in the viewport and why both its pointers are guarded.
	m_empty = new empty_state(m_list, this);

	m_status = new QLabel(this);
	m_status->setWordWrap(true);
	outer->addWidget(m_status);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	outer->addWidget(buttons);
}

void media_dialog::set_site(const QString &site_host, const QString &node_id,
                            const stream_context &ctx) {
	m_site    = site_host;
	m_node_id = node_id;
	m_ctx     = ctx;
	repopulate();
}

void media_dialog::repopulate() {
	m_list->clear();
	const QList<media_item> items = m_detector->items_for(m_site);

	for (const media_item &item : items) {
		auto *row = new QTreeWidgetItem(m_list);
		row->setText(0, kind_label(item.kind));
		row->setText(1, item.label);
		row->setText(2, item.url.host());

		// Both actions on every row; the list is already primary-first (§11.3).
		auto *cell    = new QWidget(m_list);
		auto *layout  = new QHBoxLayout(cell);
		layout->setContentsMargins(0, 0, 0, 0);
		auto *watch   = new QPushButton("▶ Watch", cell);
		auto *save    = new QPushButton("⬇ Download", cell);
		layout->addWidget(watch);
		layout->addWidget(save);
		connect(watch, &QPushButton::clicked, this, [this, item] { this->watch(item); });
		connect(save,  &QPushButton::clicked, this, [this, item] { this->save(item); });
		m_list->setItemWidget(row, 3, cell);
	}
	// What the tap can see but no URL can name (§11.6). These rows are not
	// streams anyone can fetch — the bytes exist only inside the player — so
	// the one thing offered is to record them.
	const QList<mse_stream> playing = m_tap ? m_tap->streams_for(m_site)
	                                        : QList<mse_stream>{};
	for (const mse_stream &s : playing) {
		if (s.bytes <= 0)
			continue;
		auto *row = new QTreeWidgetItem(m_list);
		row->setText(0, "Playing");
		row->setText(1, QString("%1 · %2 buffered")
		                    .arg(s.mime, QLocale().formattedDataSize(s.bytes)));
		row->setText(2, m_site);

		auto *cell   = new QWidget(m_list);
		auto *layout = new QHBoxLayout(cell);
		layout->setContentsMargins(0, 0, 0, 0);
		auto *rec = new QPushButton("⏺ Capture", cell);
		rec->setToolTip("Record what the player is fed. The page reloads first, "
		                 "so the recording starts from the beginning.");
		layout->addWidget(rec);
		connect(rec, &QPushButton::clicked, this, [this] {
			emit capture_requested();
			accept();
		});
		m_list->setItemWidget(row, 3, cell);
	}

	// Type, host and the button cell take the width they need; Name is the
	// stretch column and absorbs whatever is left.
	//
	// **Host was the one column nobody sized**, so it kept a default width and
	// elided `hls.cdnvideo11...` while Name had several hundred pixels to
	// spare. Which host served a stream is exactly the thing somebody is
	// looking at this dialog to find out -- a media host is not the page's
	// host, and that difference is the whole point of the column.
	//
	// Sized to contents rather than stretched, because a host is bounded in a
	// way a name is not: it is a domain, and the longest one here is shorter
	// than most of the names beside it.
	m_list->resizeColumnToContents(0);
	m_list->resizeColumnToContents(2);
	m_list->resizeColumnToContents(3);

	if (!items.isEmpty()) {
		// **Counts the rows, not `items`.** `playing` adds rows of its own
		// below these, so counting `items` announced "1 item" over a list of
		// three -- a number that disagrees with what is on screen is worse
		// than no number, because the reader has to work out which is lying.
		const int rows = m_list->topLevelItemCount();
		m_status->setText(QString("%1 item%2. The first row is the stream this "
		                          "page looks to be playing.")
		                      .arg(rows).arg(rows == 1 ? "" : "s"));
	} else if (!playing.isEmpty()) {
		// The badge said this page is playing; the list must not then look
		// empty, which is how it read before these rows existed.
		m_status->setText("This page is playing video, but no stream URL could "
		                  "be found — the address is hidden, or the bytes never "
		                  "travel as one. Capture records it as it plays.");
	} else {
		// Said once, in the space it is about. The status line stays empty
		// here rather than repeating it two inches lower.
		m_empty->set_text("Nothing detected yet.\n\nMany sites only request "
		                   "the manifest when their player starts, so try "
		                   "pressing play first.");
		m_status->clear();
	}
}

void media_dialog::assemble_then(const media_item &item, bool play_it) {
	if (!m_assembler)
		m_assembler = new hls_assembler(this);

	const QString out = play_it
	  ? m_scratch.filePath("stream.ts")
	  : QDir(m_downloads->directory()).filePath(
	        item.label.section('/', -1).section('.', 0, 0) + ".ts");

	connect(m_assembler, &hls_assembler::progress, this,
	         [this, out, play_it, item](qint64 bytes, int done, int total) {
		m_status->setText(QString("Assembling %1/%2 segments (%3 KiB)…")
		                      .arg(done).arg(total).arg(bytes / 1024));
		// Hand the player the growing file as soon as there is something to
		// play — that is the §11.3 tee-to-disk trick, and it is what turns a
		// live stream into a locally seekable one.
		if (play_it && done == 1) {
			const QUrl via = m_proxy ? m_proxy->publish_file(out, "video/mp2t") : QUrl();
			QString error;
			if (!m_players->play(item, &error, via))
				m_status->setText("<b>" + error.toHtmlEscaped() + "</b>");
		}
	}, Qt::UniqueConnection);

	connect(m_assembler, &hls_assembler::completed, this, [this, out, play_it] {
		m_status->setText(play_it ? "Stream assembled; playback continues locally."
		                          : QString("Saved assembled stream to %1.").arg(out));
	}, Qt::UniqueConnection);

	connect(m_assembler, &hls_assembler::failed, this, [this](const QString &e) {
		m_status->setText("<b>Assembly failed:</b> " + e.toHtmlEscaped());
	}, Qt::UniqueConnection);

	m_status->setText("Fetching manifest…");
	m_assembler->start(item.url, m_ctx, out);
}

void media_dialog::watch(const media_item &item) {
	// A player that cannot take a manifest gets an assembled progressive file
	// instead — "the app compensates in the proxy for what the player lacks".
	if (item.kind == media_kind::hls && !m_players->selected_handles_streams()) {
		assemble_then(item, true);
		return;
	}

	const QString warn = m_players->warning_for(item);
	// Hand the player a localhost URL when the proxy is up, so the CDN sees
	// the page's Referer and cookies rather than a naked request (§11.3).
	// The page's own context, overlaid with anything this particular stream
	// asked for. A learned extractor names the headers its CDN checks, and
	// they are useless if they stop here.
	stream_context ctx = m_ctx;
	for (auto it = item.headers.cbegin(); it != item.headers.cend(); ++it) {
		const QString k = it.key().toLower();
		if (k == "referer" || k == "referrer") ctx.referer    = it.value();
		else if (k == "user-agent")            ctx.user_agent = it.value();
		else if (k == "cookie")                ctx.cookies    = it.value();
		else                                   ctx.extra.insert(it.key(), it.value());
	}
	const QUrl via = m_proxy ? m_proxy->publish(item.url, ctx) : QUrl();
	QString error;
	if (!m_players->play(item, &error, via)) {
		m_status->setText("<b>" + error.toHtmlEscaped() + "</b>");
		return;
	}
	m_status->setText(warn.isEmpty()
	                      ? QString("Opened in %1%2.").arg(m_players->selected(),
	                            via.isValid() ? " via the local proxy" : "")
	                      : "<b>Warning:</b> " + warn.toHtmlEscaped());
}

void media_dialog::save(const media_item &item) {
	// HLS is saveable now: fetch the segments and concatenate them (§11.2).
	if (item.kind == media_kind::hls) {
		assemble_then(item, false);
		return;
	}

	QString error;
	// A learned stream carries the headers its CDN checks; without them the
	// download is refused where Watch would have succeeded.
	const int id = m_downloads->enqueue(item.url, m_node_id, &error, item.headers);
	// Where it lands, as the user will actually find it.
	//
	// On Android naming the directory is worse than useless: Qt downloads into
	// app-private storage, whose path is long, unopenable by any file manager,
	// and not where the finished file ends up anyway -- it is copied into the
	// shared Downloads collection when it completes (§19).
#ifdef Q_OS_ANDROID
	const QString where = QStringLiteral("Queued. It will appear in Downloads "
	                                      "when it finishes.");
#else
	const QString where =
	  QString("Queued download to %1.").arg(m_downloads->directory());
#endif
	m_status->setText(id ? where : "<b>" + error.toHtmlEscaped() + "</b>");
}
