#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QString>

// One entry as KeePassXC returns it. Never persisted by us -- it lives only as
// long as the fill that requested it.
struct credential {
	QString name;
	QString login;
	QString password;
	// **The vault's own id for this entry, and the only thing that can turn a
	// save into an update.** `set_login_request` takes a uuid, documented as
	// "empty means create", and until this field existed nothing could ever
	// fill it in: `parse_logins` dropped the uuid KeePassXC sends, so every
	// save was an add. Changing a stored password therefore left the vault
	// with two entries for the site, identical but for the password, and the
	// next fill asked the person to choose between them with nothing on
	// screen to say which was current.
	//
	// Last in the struct, so that every existing brace-initialiser still
	// means what it did and only gains a field. `-Wextra` asks for that field
	// to be written out, which is why the three-field sites now name it.
	QString uuid;
};

// The wire half of the KeePassXC-Browser protocol (architecture doc sec 13.1),
// with no socket and no crypto in it.
//
// That split is deliberate. The message shapes, the nonce discipline and the
// association state machine are where the protocol bugs live, and they are all
// pure functions of their input -- so they can be tested without libsodium, a
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

// The one message sent in the clear -- it is what establishes the shared key.
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
// configured policy, which is the point (sec 13.1): we ask, we do not configure.
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

// One request that has been sent and not yet answered.
//
// **A reply carries no request id.** Its only identifying fields are the
// cleartext `action` and the `nonce`, so the action alone is what the bridge
// used to match on -- and two requests of the same action in flight at once
// then answered each other's callers. Two tabs reaching a login form in the
// same second is all that takes: tab A asks, tab B asks, A's reply arrives
// and is delivered under B's tag, and B's form is filled with A's site's
// credentials. The nonce is what tells them apart, because the bridge
// increments it per message and so no two in-flight requests share one.
struct pending_request {
	QString    action;
	QByteArray nonce;   // the nonce this request was sealed with
	int        tag = 0;
};

enum class reply_match {
	none_pending,   // nothing of that action was outstanding
	ambiguous,      // several were, and the nonce picks none of them
	// Matched -- `*index` is the request this reply answers. Which of the
	// three rules below placed it is reported rather than collapsed, because
	// it is the one thing about this protocol's nonces that no test here can
	// settle: `try_keepass` prints it, and a run against a real vault then
	// says whether the specified relationship holds in practice.
	by_incremented_nonce,
	by_echoed_nonce,
	by_being_alone,
};

inline bool matched(reply_match m) {
	return m == reply_match::by_incremented_nonce ||
	        m == reply_match::by_echoed_nonce || m == reply_match::by_being_alone;
}

// Which outstanding request a reply answers.
//
// Three rules, tried in order, and the order is the whole of the design:
//
//  1. `increment_nonce(request) == reply`, which is what the protocol
//     specifies and what keepassxc-browser verifies.
//  2. `request == reply`, in case a server echoes the nonce instead. Which of
//     the two a real KeePassXC sends is not established here -- `try_keepass`
//     reports it, see `keepass_bridge::last_reply_match`.
//  3. Exactly one candidate, whatever its nonce says. This is the single
//     request case, which is nearly every case, and it deliberately keeps
//     working no matter how the nonce is spelled -- including not at all. A
//     strict check that could refuse every reply is not worth having in a
//     path that cannot be tested without a vault.
//
// `ambiguous` therefore only arises with two or more of one action in flight
// and a nonce matching none of them, and the caller must deliver nothing: a
// reply handed to the wrong tag is a password typed into the wrong site,
// which is worse than a fill that does not happen.
reply_match match_reply(const QList<pending_request> &pending, const QString &action,
                        const QByteArray &reply_nonce, int *index);

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

// Which stored entry a save should UPDATE rather than add beside, by matching
// the login being saved against what the vault already returned for this site.
// Empty means add a new entry, which is also what an unrecognised login means.
//
// **Two stored entries sharing one login answer empty, deliberately.** There
// is nothing to choose between them from here, and the two mistakes are not
// comparable: adding a third entry is untidy and recoverable, while updating
// the wrong one overwrites a password that may be the one still in use.
QString uuid_for_login(const QList<credential> &known, const QString &login);

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
