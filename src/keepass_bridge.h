// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "keepass_protocol.h"

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>

class QLocalSocket;

// Client for KeePassXC's BrowserServer (architecture doc §13.1).
//
// The point of this design is what it does *not* do: it never parses .kdbx,
// never sees the master password, and stores no vault of its own. KeePassXC is
// already an unlocked local daemon in the user's session, so we become a
// first-class client of it and add no new crypto we own.
//
// Transport is the Unix domain socket directly — the keepassxc-proxy helper
// exists only to bridge stdio for sandboxed browser extensions, and a native
// app skips it.
class keepass_bridge : public QObject {
	Q_OBJECT
public:
	explicit keepass_bridge(QObject *parent = nullptr);

	// False when this build cannot talk to a KeePassXC at all. Two ways that
	// happens, and they are different enough that the UI should not merge them:
	// no libsodium, or a platform where the browser-integration socket does not
	// exist. `unavailable_reason()` is empty exactly when `supported()` is true.
	static bool supported();
	static QString unavailable_reason();

	// The socket path KeePassXC listens on.
	static QString socket_path();

	bool connected() const;
	bool associated() const { return !m_assoc_id.isEmpty(); }

	// Connect and perform the change-public-keys handshake.
	void start();
	void disconnect_now();

	// Ask KeePassXC to associate. The user confirms and names the connection
	// inside KeePassXC; the returned id and key are what we keep.
	void associate();

	// Restore a saved pairing and verify it still holds (§13.1).
	void set_association(const QString &id, const QString &id_key_b64);
	QString association_id() const { return m_assoc_id; }
	QString association_key() const { return m_id_key_b64; }
	void test_association();

	// KeePassXC does its own URL matching and may prompt per site.
	void request_logins(const QString &url, int request_tag);

signals:
	void ready();                                   // handshake done
	void associated_changed(bool ok, const QString &message);
	void logins(int request_tag, const QList<credential> &entries);
	void error(const QString &message);

private:
	void on_readable();
	void handle(const QJsonObject &reply);
	bool send_encrypted(const QString &action, const QJsonObject &inner);
	bool send_plain(const QJsonObject &msg);
	QByteArray next_nonce();

	QLocalSocket *m_socket = nullptr;
	QByteArray m_our_public, m_our_secret, m_their_public;
	QByteArray m_nonce;
	QString    m_client_id;
	QString    m_assoc_id;
	QString    m_id_key_b64;
	int        m_pending_tag = 0;
	bool       m_handshaken  = false;
	QByteArray m_buffer;
};
