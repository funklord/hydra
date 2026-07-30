// SPDX-License-Identifier: GPL-3.0-or-later
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

}  // namespace keepass_protocol
