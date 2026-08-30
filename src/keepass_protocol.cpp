#include "keepass_protocol.h"

#include <QJsonArray>

namespace keepass_protocol {

QByteArray increment_nonce(const QByteArray &nonce) {
	QByteArray out = nonce;
	// Little-endian increment with carry. Getting the carry wrong produces a
	// nonce reuse, which is the one failure this protocol cannot tolerate, so
	// it is written plainly rather than cleverly.
	quint16 carry = 1;
	for (int i = 0; i < out.size(); ++i) {
		carry += static_cast<quint8>(out.at(i));
		out[i] = static_cast<char>(carry & 0xFF);
		carry >>= 8;
	}
	return out;
}

QJsonObject change_public_keys(const QString &client_id, const QString &public_key_b64,
                                const QString &nonce_b64) {
	QJsonObject o;
	o.insert("action", "change-public-keys");
	o.insert("publicKey", public_key_b64);
	o.insert("nonce", nonce_b64);
	o.insert("clientID", client_id);
	return o;
}

QJsonObject associate_request(const QString &our_key_b64, const QString &id_key_b64) {
	QJsonObject o;
	o.insert("action", "associate");
	o.insert("key", our_key_b64);
	o.insert("idKey", id_key_b64);
	return o;
}

QJsonObject test_associate_request(const QString &assoc_id, const QString &id_key_b64) {
	QJsonObject o;
	o.insert("action", "test-associate");
	o.insert("id", assoc_id);
	o.insert("key", id_key_b64);
	return o;
}

QJsonObject get_logins_request(const QString &url, const QString &assoc_id,
                               const QString &id_key_b64) {
	QJsonObject key;
	key.insert("id", assoc_id);
	key.insert("key", id_key_b64);
	QJsonArray keys;
	keys.append(key);

	QJsonObject o;
	o.insert("action", "get-logins");
	o.insert("url", url);
	o.insert("keys", keys);
	return o;
}

QJsonObject get_databasehash_request() {
	QJsonObject o;
	o.insert("action", "get-databasehash");
	return o;
}

QJsonObject set_login_request(const QString &url, const QString &login,
                              const QString &password, const QString &uuid,
                              const QString &assoc_id, const QString &id_key_b64) {
	QJsonObject key;
	key.insert("id", assoc_id);
	key.insert("key", id_key_b64);
	QJsonArray keys;
	keys.append(key);

	QJsonObject o;
	o.insert("action", "set-login");
	o.insert("url", url);
	o.insert("submitUrl", url);
	o.insert("login", login);
	o.insert("password", password);
	o.insert("keys", keys);
	// Empty means create; the header explains why this is the whole switch.
	if (!uuid.isEmpty())
		o.insert("uuid", uuid);
	return o;
}

QJsonObject generate_password_request() {
	QJsonObject o;
	o.insert("action", "generate-password");
	return o;
}

QJsonObject envelope(const QString &action, const QString &client_id,
                     const QString &nonce_b64, const QString &encrypted_b64) {
	QJsonObject o;
	o.insert("action", action);
	o.insert("message", encrypted_b64);
	o.insert("nonce", nonce_b64);
	o.insert("clientID", client_id);
	return o;
}

bool is_error(const QJsonObject &reply, QString *message) {
	// Two shapes in the wild: an explicit error field, or success == "false".
	if (reply.contains("error")) {
		if (message) {
			const int code = reply.value("errorCode").toString().toInt();
			*message = reply.value("error").toString();
			if (code)
				*message += QString(" (code %1)").arg(code);
		}
		return true;
	}
	const QJsonValue success = reply.value("success");
	if (success.isString() && success.toString() != "true") {
		if (message)
			*message = "KeePassXC reported failure.";
		return true;
	}
	return false;
}

int error_code(const QJsonObject &reply) {
	// The wire carries it as a string, which is why this is not a plain
	// toInt() on the value: a numeric read of a JSON string is 0, and 0 is the
	// same answer this gives for "no error at all".
	return reply.value("errorCode").toString().toInt();
}

bool parse_associate(const QJsonObject &reply, QString *assoc_id) {
	QString err;
	if (is_error(reply, &err))
		return false;
	const QString id = reply.value("id").toString();
	if (id.isEmpty())
		return false;
	if (assoc_id)
		*assoc_id = id;
	return true;
}

QList<credential> parse_logins(const QJsonObject &reply) {
	QList<credential> out;
	QString err;
	if (is_error(reply, &err))
		return out;
	for (const QJsonValue &v : reply.value("entries").toArray()) {
		const QJsonObject e = v.toObject();
		credential c;
		c.name     = e.value("name").toString();
		c.login    = e.value("login").toString();
		c.password = e.value("password").toString();
		if (!c.login.isEmpty() || !c.password.isEmpty())
			out.push_back(c);
	}
	return out;
}

bool parse_set_login(const QJsonObject &reply) {
	QString err;
	return !is_error(reply, &err);
}

QString parse_generated_password(const QJsonObject &reply) {
	QString err;
	if (is_error(reply, &err))
		return QString();
	// The direct field, if this KeePassXC sends it that way.
	const QString direct = reply.value("password").toString();
	if (!direct.isEmpty())
		return direct;
	// Otherwise the entries-array shape, sharing parse_logins' reading of it.
	for (const QJsonValue &v : reply.value("entries").toArray()) {
		const QString pw = v.toObject().value("password").toString();
		if (!pw.isEmpty())
			return pw;
	}
	return QString();
}

}  // namespace keepass_protocol
