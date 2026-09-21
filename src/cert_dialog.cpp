#include "cert_dialog.h"

#include <QDateTime>
#include <QDialogButtonBox>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

cert_dialog::cert_dialog(const QString &host,
                          const QList<web_view_backend::certificate_offer> &offered,
                          QWidget *parent)
    : QDialog(parent) {
	setWindowTitle("Identify yourself?");
	setObjectName("cert_dialog");
	auto *column = new QVBoxLayout(this);

	auto *asking = new QLabel(this);
	asking->setObjectName("cert_site");
	asking->setWordWrap(true);
	asking->setText(QString("<b>%1</b> is asking for a certificate that "
	                         "identifies you.")
	                    .arg(host.isEmpty() ? QStringLiteral("This site") : host));
	column->addWidget(asking);

	auto *what = new QLabel(this);
	what->setObjectName("cert_explain");
	what->setWordWrap(true);
	what->setText("Sending one tells the site who you are, by name, before you "
	               "have typed anything. Sites that do not need it will not ask "
	               "again if you decline.");
	column->addWidget(what);

	m_list = new QListWidget(this);
	m_list->setObjectName("cert_list");
	// **Wrapped, not scrolled sideways.** Photographing this found the issuer
	// and expiry line running past the right edge behind a horizontal
	// scrollbar, which hides exactly the two facts the row exists to show: who
	// vouched for the certificate and whether it is still valid. A person
	// deciding whether to identify themselves should not have to scroll to find
	// that out.
	m_list->setWordWrap(true);
	m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	setMinimumWidth(460);
	for (const auto &c : offered) {
		// Subject first, because it is the one a person recognises as theirs.
		// Issuer and expiry follow, since a certificate from an unexpected
		// issuer, or one already expired, is the case worth noticing.
		// **Say "expired" where it has, rather than printing the date and
		// leaving the arithmetic to the reader.** The line above already
		// called an already-expired certificate "the case worth noticing",
		// and the only way to notice it was to compare a date against today
		// in your head -- which nobody does while a site is waiting. Found by
		// looking at the picture try_look takes of this window: one of the two
		// rows was three weeks dead and read exactly like the live one.
		//
		// Parsed rather than carried, because the seam hands this over as
		// text: `qtwebengine_view` writes `expiryDate().toString(Qt::ISODate)`
		// into a QString field. ISO-8601 round-trips exactly, so reading it
		// back is safe -- and a value that does not parse is left alone and
		// printed as it arrived, since a certificate whose expiry this cannot
		// read is not thereby expired.
		// **Instants, not dates, because the two are in different zones.**
		// `qtwebengine_view` writes `expiryDate().toString(Qt::ISODate)` and
		// that date is UTC, so it arrives as `...Z`; comparing its *date* part
		// against `QDate::currentDate()`, which is local, is off by a day
		// whenever the two disagree about which day it is. Measured here: the
		// local offset is +2h, `...Z` parses with `Qt::UTC` and a bare
		// `2026-09-01` parses as local, and QDateTime compares them as moments
		// rather than as calendar days. A certificate that expired an hour ago
		// should say so in either zone.
		const QDateTime until = QDateTime::fromString(c.valid_until,
		                                               Qt::ISODate);
		const bool expired = until.isValid()
		                      && until < QDateTime::currentDateTime();
		m_list->addItem(QString("%1\n    issued by %2 — %3 %4")
		                    .arg(c.subject.isEmpty()
		                             ? QStringLiteral("(unnamed certificate)")
		                             : c.subject,
		                          c.issuer.isEmpty()
		                             ? QStringLiteral("an unnamed authority")
		                             : c.issuer,
		                          expired ? QStringLiteral("EXPIRED")
		                                   : QStringLiteral("valid until"),
		                          c.valid_until.isEmpty()
		                             ? QStringLiteral("an unstated date")
		                             : c.valid_until));
	}
	// **Nothing selected to begin with.** A pre-selected row plus a Send button
	// is one keystroke away from identifying somebody who was reading the
	// question rather than answering it.
	m_list->setCurrentRow(-1);
	column->addWidget(m_list);

	auto *buttons = new QDialogButtonBox(this);
	QPushButton *send = buttons->addButton("&Send", QDialogButtonBox::AcceptRole);
	QPushButton *none = buttons->addButton("&Don't send",
	                                        QDialogButtonBox::RejectRole);
	// Declining is the default: return answers it, escape answers it, and the
	// window manager's close button answers it.
	none->setDefault(true);
	send->setEnabled(false);
	column->addWidget(buttons);

	// Send stays unavailable until there is something to send, so the button
	// cannot promise an action it has no argument for.
	connect(m_list, &QListWidget::currentRowChanged, this,
	        [send](int row) { send->setEnabled(row >= 0); });
	connect(m_list, &QListWidget::itemDoubleClicked, this, [this] {
		if (m_list->currentRow() >= 0) { m_send = true; accept(); }
	});
	connect(send, &QPushButton::clicked, this, [this] { m_send = true; accept(); });
	connect(none, &QPushButton::clicked, this, [this] { m_send = false; reject(); });
}

int cert_dialog::chosen() const {
	return m_send ? m_list->currentRow() : -1;
}
