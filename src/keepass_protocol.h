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

// Create or update an entry. `uuid` empty means create; non-empty updates that
// entry (the bridge's `save_login` documents the same split). The association
// proof travels the same way `get_logins_request` sends it -- a one-element
// `keys` array of {id, key} -- rather than the bare `id` field one secondhand
// source showed for this action, because the `keys` shape is the one already
// verified end to end in this codebase (project.md, "get-logins against a
// real vault") and an extra, ignored field is a smaller risk than an unverified
// one. `submitUrl` is set equal to `url`: KeePassXC can store a different form
// target than the page url, but the bridge's callers only ever have the one
// url the save happened on. `group`/`groupUuid` are omitted -- there is no
// group picker here, and the protocol treats both as optional.
QJsonObject set_login_request(const QString &url, const QString &login,
                              const QString &password, const QString &uuid,
                              const QString &assoc_id, const QString &id_key_b64);

// generate-password takes no arguments -- KeePassXC generates per its own
// configured policy, which is the point (§13.1): we ask, we do not configure.
//
// **Unverified:** whether the real client sends this as a normal encrypted,
// enveloped action (like `associate` or `get-logins`) or as a bare top-level
// message the way `change-public-keys` is sent in the clear could not be
// established here -- there is no live KeePassXC in this environment, and the
// secondhand descriptions found disagreed with each other. `keepass_bridge`
// sends it through the same encrypted envelope as every other post-handshake
// action; see its comment for why that is the safer default either way.
QJsonObject generate_password_request();

// The outer envelope every encrypted message travels in.
QJsonObject envelope(const QString &action, const QString &client_id,
                     const QString &nonce_b64, const QString &encrypted_b64);

// --- Replies -------------------------------------------------------------
// KeePassXC reports failures inside the message rather than by transport
// error, so every parse checks for that first.
bool is_error(const QJsonObject &reply, QString *message);

// The numeric code beside that message, 0 when there is none. Exposed because
// one of them is not a failure at all -- see below.
int error_code(const QJsonObject &reply);

// **"No logins found" is an answer, not an error.** KeePassXC reports a url it
// has no entry for as error 15, and treating it like the rest means a
// `get-logins` for any site not in the vault -- which is most sites -- never
// produces a reply at all, so whoever asked waits until the page navigates.
// Measured against a real KeePassXC, once a stored pairing made the request
// reachable without a human.
constexpr int no_logins_found = 15;

bool parse_associate(const QJsonObject &reply, QString *assoc_id);
QList<credential> parse_logins(const QJsonObject &reply);

// Whether a set-login request was accepted. There is nothing else worth
// returning: KeePassXC's reply carries no entry id or other detail we use, so
// unlike `parse_associate` there is no out-parameter.
bool parse_set_login(const QJsonObject &reply);

// Empty on failure. Two reply shapes are handled, because which one a given
// KeePassXC sends is one of the facts this could not verify offline (see
// `generate_password_request` above): a bare "password" field, or an
// "entries" array -- the same shape `parse_logins` reads -- whose first
// element carries one.
QString parse_generated_password(const QJsonObject &reply);

}  // namespace keepass_protocol
