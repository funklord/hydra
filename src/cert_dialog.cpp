#include "cert_dialog.h"

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
		m_list->addItem(QString("%1\n    issued by %2 — valid until %3")
		                    .arg(c.subject.isEmpty()
		                             ? QStringLiteral("(unnamed certificate)")
		                             : c.subject,
		                          c.issuer.isEmpty()
		                             ? QStringLiteral("an unnamed authority")
		                             : c.issuer,
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
