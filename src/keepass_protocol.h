// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QString>

// One entry as KeePassXC returns it. Never persisted by us — it lives only as
// long as the fill that requested it.
struct credential {
	QString name;
	QString login;
	QString password;
};

// The wire half of the KeePassXC-Browser protocol (architecture doc §13.1),
// with no socket and no crypto in it.
//
// That split is deliberate. The message shapes, the nonce discipline and the
// association state machine are where the protocol bugs live, and they are all
// pure functions of their input — so they can be tested without libsodium, a
// running KeePassXC, or a socket. The bridge is then a thin transport that
// encrypts what these produce.
//
// We are a native app, so we connect to the BrowserServer socket directly; the
// keepassxc-proxy helper exists only to bridge stdio for sandboxed browser
// extensions and is skipped.
namespace keepass_protocol {

// Per-message nonces: the nonce is incremented for each message, little-endian
// with carry, exactly as libsodium's sodium_increment does.
QByteArray increment_nonce(const QByteArray &nonce);

// The one message sent in the clear — it is what establishes the shared key.
QJsonObject change_public_keys(const QString &client_id, const QString &public_key_b64,
                                const QString &nonce_b64);

// Inner messages, encrypted before sending.
QJsonObject associate_request(const QString &our_key_b64, const QString &id_key_b64);
QJsonObject test_associate_request(const QString &assoc_id, const QString &id_key_b64);
QJsonObject get_logins_request(const QString &url, const QString &assoc_id,
                               const QString &id_key_b64);
QJsonObject get_databasehash_request();

// The outer envelope every encrypted message travels in.
QJsonObject envelope(const QString &action, const QString &client_id,
                     const QString &nonce_b64, const QString &encrypted_b64);

// --- Replies -------------------------------------------------------------
// KeePassXC reports failures inside the message rather than by transport
// error, so every parse checks for that first.
bool is_error(const QJsonObject &reply, QString *message);

bool parse_associate(const QJsonObject &reply, QString *assoc_id);
QList<credential> parse_logins(const QJsonObject &reply);

}  // namespace keepass_protocol
