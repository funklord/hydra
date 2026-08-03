// SPDX-License-Identifier: GPL-3.0-or-later
#include "keepass_bridge.h"
#include "box_crypto.h"
#include "credential_store.h"

#include <QJsonDocument>
#include <QLocalSocket>
#include <QProcessEnvironment>
#include <QUuid>

namespace {

QString b64(const QByteArray &raw) {
	return QString::fromLatin1(raw.toBase64());
}

QByteArray unb64(const QString &s) {
	return QByteArray::fromBase64(s.toLatin1());
}

}  // namespace

keepass_bridge::keepass_bridge(QObject *parent) : QObject(parent) {
	m_client_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
}

bool keepass_bridge::supported() {
	return unavailable_reason().isEmpty();
}

QString keepass_bridge::unavailable_reason() {
#ifdef Q_OS_ANDROID
	// Not a build option and not a missing feature: KeePassXC's browser
	// integration is a Unix socket belonging to a desktop application, and there
	// is no such thing to connect to on a phone. Saying "unavailable" without
	// saying that leaves a menu item that looks broken, and offering it at all
	// would send someone looking for a KeePassXC to start.
	//
	// The platform's own answer is the system autofill service, which fills
	// WebView forms without this browser implementing anything -- §13.2's shell
	// bridge is the desktop mechanism, not the only one that may fill a form.
	return QStringLiteral(
		"KeePassXC's browser integration is a desktop socket. On Android the "
		"system autofill service fills forms instead.");
#else
	if (!box_crypto::available())
		return QStringLiteral("Built without libsodium — the KeePassXC protocol "
		                       "is end-to-end encrypted and needs it.");
	return QString();
#endif
}

QString keepass_bridge::socket_path() {
	const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
	QString dir = env.value("XDG_RUNTIME_DIR");
	if (dir.isEmpty())
		dir = "/tmp";
	return dir + "/org.keepassxc.KeePassXC.BrowserServer";
}

bool keepass_bridge::connected() const {
	return m_socket && m_socket->state() == QLocalSocket::ConnectedState;
}

void keepass_bridge::start() {
	if (!supported()) {
		emit error("Built without libsodium — the KeePassXC protocol is "
		           "end-to-end encrypted and cannot run without it.");
		return;
	}
	if (connected())
		return;

	if (!box_crypto::keypair(&m_our_public, &m_our_secret)) {
		emit error("Could not generate a key pair.");
		return;
	}
	m_nonce = box_crypto::random_nonce();

	if (!m_socket) {
		m_socket = new QLocalSocket(this);
		connect(m_socket, &QLocalSocket::readyRead, this, &keepass_bridge::on_readable);
		connect(m_socket, &QLocalSocket::errorOccurred, this, [this] {
			emit error("KeePassXC socket: " + m_socket->errorString() +
			           " — is KeePassXC running with browser integration enabled?");
		});
	}

	m_socket->connectToServer(socket_path());
	if (!m_socket->waitForConnected(1500)) {
		emit error("Could not reach KeePassXC. Start it and enable browser "
		           "integration in its settings.");
		return;
	}

	// The one message in the clear; it is what establishes the shared key.
	send_plain(keepass_protocol::change_public_keys(m_client_id, b64(m_our_public),
	                                                b64(m_nonce)));
}

void keepass_bridge::disconnect_now() {
	if (m_socket)
		m_socket->disconnectFromServer();
	m_handshaken = false;
	m_their_public.clear();
}

QByteArray keepass_bridge::next_nonce() {
	m_nonce = keepass_protocol::increment_nonce(m_nonce);
	return m_nonce;
}

bool keepass_bridge::send_plain(const QJsonObject &msg) {
	if (!connected())
		return false;
	const QByteArray raw = QJsonDocument(msg).toJson(QJsonDocument::Compact);
	return m_socket->write(raw) == raw.size();
}

bool keepass_bridge::send_encrypted(const QString &action, const QJsonObject &inner) {
	if (!m_handshaken) {
		emit error("Not connected to KeePassXC yet.");
		return false;
	}
	const QByteArray plain = QJsonDocument(inner).toJson(QJsonDocument::Compact);
	const QByteArray nonce = next_nonce();
	QByteArray cipher;
	if (!box_crypto::seal(plain, nonce, m_their_public, m_our_secret, &cipher)) {
		emit error("Encryption failed.");
		return false;
	}
	return send_plain(keepass_protocol::envelope(action, m_client_id, b64(nonce),
	                                             b64(cipher)));
}

void keepass_bridge::associate() {
	if (m_id_key_b64.isEmpty())
		m_id_key_b64 = b64(box_crypto::random_bytes(32));
	send_encrypted("associate",
	                keepass_protocol::associate_request(b64(m_our_public), m_id_key_b64));
}

void keepass_bridge::set_association(const QString &id, const QString &id_key_b64) {
	m_assoc_id   = id;
	m_id_key_b64 = id_key_b64;
}

bool keepass_bridge::restore_pairing() {
	QString id, key;
	if (!credential_store::load(&id, &key))
		return false;
	set_association(id, key);
	return true;
}

bool keepass_bridge::forget_pairing() {
	m_assoc_id.clear();
	m_id_key_b64.clear();
	return credential_store::clear();
}

bool keepass_bridge::pairing_is_stored() { return credential_store::has_pairing(); }

void keepass_bridge::test_association() {
	if (m_assoc_id.isEmpty()) {
		emit associated_changed(false, "No stored pairing.");
		return;
	}
	send_encrypted("test-associate",
	                keepass_protocol::test_associate_request(m_assoc_id, m_id_key_b64));
}

void keepass_bridge::request_logins(const QString &url, int request_tag) {
	if (!associated()) {
		emit error("Not associated with KeePassXC.");
		return;
	}
	m_pending_tag = request_tag;
	send_encrypted("get-logins",
	                keepass_protocol::get_logins_request(url, m_assoc_id, m_id_key_b64));
}

void keepass_bridge::on_readable() {
	m_buffer += m_socket->readAll();
	// The server writes one JSON object per message with no framing, so parse
	// greedily and keep any trailing partial object for the next read.
	while (!m_buffer.isEmpty()) {
		QJsonParseError err{};
		const QJsonDocument doc = QJsonDocument::fromJson(m_buffer, &err);
		if (err.error == QJsonParseError::NoError && doc.isObject()) {
			m_buffer.clear();
			handle(doc.object());
			return;
		}
		// Try to split at an object boundary before giving up and waiting.
		const int split = m_buffer.indexOf("}{");
		if (split < 0)
			return;   // incomplete; wait for more bytes
		const QByteArray head = m_buffer.left(split + 1);
		m_buffer.remove(0, split + 1);
		const QJsonDocument one = QJsonDocument::fromJson(head);
		if (one.isObject())
			handle(one.object());
	}
}

void keepass_bridge::handle(const QJsonObject &reply) {
	const QString action = reply.value("action").toString();

	if (action == "change-public-keys") {
		m_their_public = unb64(reply.value("publicKey").toString());
		m_handshaken   = !m_their_public.isEmpty();
		if (!m_handshaken) {
			emit error("KeePassXC did not return a public key.");
			return;
		}
		emit ready();
		return;
	}

	// Everything else arrives encrypted under the shared key.
	const QString cipher_b64 = reply.value("message").toString();
	const QString nonce_b64  = reply.value("nonce").toString();
	QJsonObject inner = reply;
	if (!cipher_b64.isEmpty()) {
		QByteArray plain;
		if (!box_crypto::open(unb64(cipher_b64), unb64(nonce_b64), m_their_public,
		                      m_our_secret, &plain)) {
			emit error("Could not decrypt a reply from KeePassXC.");
			return;
		}
		inner = QJsonDocument::fromJson(plain).object();
	}

	QString err;
	if (keepass_protocol::is_error(inner, &err)) {
		if (action == "test-associate")
			// The stored pairing is deliberately *not* dropped here. A refused
			// test-associate means "this KeePassXC does not accept it now",
			// which is what a locked database, a different database, or a
			// vault that has not been opened yet all look like from out here.
			// Throwing the pairing away on that would make a locked vault cost
			// the user their pairing, and re-pairing is the one step that needs
			// a human. Forgetting stays an explicit act.
			emit associated_changed(false, err);
		else
			emit error(err);
		return;
	}

	if (action == "associate") {
		QString id;
		if (keepass_protocol::parse_associate(inner, &id)) {
			m_assoc_id = id;
			// Stored here rather than by the caller, at the one moment the
			// pairing is known good, so no path can pair and forget to save.
			// Failing to store is not failing to pair: this run works either
			// way, and the difference only shows up next launch, so it is said
			// rather than treated as an error.
			const bool kept = credential_store::save(m_assoc_id, m_id_key_b64);
			emit associated_changed(
			    true, kept ? QStringLiteral("Paired with KeePassXC.")
			               : QStringLiteral(
			                     "Paired with KeePassXC, but the pairing could "
			                     "not be stored and will have to be confirmed "
			                     "again next time."));
		} else {
			emit associated_changed(false, "KeePassXC declined the pairing.");
		}
	} else if (action == "test-associate") {
		emit associated_changed(true, "Existing pairing accepted.");
	} else if (action == "get-logins") {
		emit logins(m_pending_tag, keepass_protocol::parse_logins(inner));
	}
}
