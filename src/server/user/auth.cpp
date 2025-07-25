#include "server/user/auth.h"
#include "server/user/user_manager.h"
#include "server/server.h"
#include "network/client_socket.h"
#include "network/router.h"
#include <openssl/bn.h>
#include <openssl/rsa.h>
#include <openssl/pem.h>

struct AuthManagerPrivate {
  AuthManagerPrivate();
  ~AuthManagerPrivate() {
    RSA_free(rsa);
  }

  void reset() {
    current_idx = 0;
    name = "";
    password = "";
    password_decrypted = "";
    md5 = "";
    version = "unknown";
    uuid = "";
  }
  
  bool is_valid() {
    return current_idx == 5;
  }

  void handle(cbor_data data, size_t sz) {
    auto sv = std::string_view { (char *)data, sz };
    switch (current_idx) {
      case 0:
        name = sv;
        break;
      case 1:
        password = sv;
        break;
      case 2:
        md5 = sv;
        break;
      case 3:
        version = sv;
        break;
      case 4:
        uuid = sv;
        break;
    }
    current_idx++;
  }

  RSA *rsa;

  // setup message
  std::weak_ptr<ClientSocket> client;
  std::string_view name;
  std::string_view password;
  std::string_view password_decrypted;
  std::string_view md5;
  std::string_view version;
  std::string_view uuid;

  // parsing
  int current_idx;
};

AuthManagerPrivate::AuthManagerPrivate() {
  rsa = RSA_new();
  if (!std::filesystem::exists("server/rsa_pub")) {
    BIGNUM *bne = BN_new();
    BN_set_word(bne, RSA_F4);
    RSA_generate_key_ex(rsa, 2048, bne, NULL);

    BIO *bp_pub = BIO_new_file("server/rsa_pub", "w+");
    PEM_write_bio_RSAPublicKey(bp_pub, rsa);
    BIO *bp_pri = BIO_new_file("server/rsa", "w+");
    PEM_write_bio_RSAPrivateKey(bp_pri, rsa, NULL, NULL, 0, NULL, NULL);

    BIO_free_all(bp_pub);
    BIO_free_all(bp_pri);
    chmod("server/rsa", 0600);
    BN_free(bne);
  }

  FILE *keyFile = fopen("server/rsa_pub", "r");
  PEM_read_RSAPublicKey(keyFile, &rsa, NULL, NULL);
  fclose(keyFile);
  keyFile = fopen("server/rsa", "r");
  PEM_read_RSAPrivateKey(keyFile, &rsa, NULL, NULL);
  fclose(keyFile);
}

AuthManager::AuthManager() {
  p_ptr = std::make_unique<AuthManagerPrivate>();
  /*
  db = parent->getDatabase();
  */

  std::string public_key;
  std::ifstream file("server/rsa_pub");
  if (file) {
    std::ostringstream ss;
    ss << file.rdbuf();
    public_key = ss.str();
  }

  cbor_item_t *cb = cbor_build_bytestring((cbor_data)public_key.c_str(), public_key.size());
  cbor_serialize_alloc(cb, &public_key_cbor_buf, &public_key_cbor_bufsize);
  cbor_decref(&cb);
}

AuthManager::~AuthManager() noexcept {
  // delete p_ptr;
  free(public_key_cbor_buf);
}

std::string_view AuthManager::getPublicKeyCbor() const {
  return { (char *)public_key_cbor_buf, public_key_cbor_bufsize };
}

void AuthManager::processNewConnection(std::shared_ptr<ClientSocket> conn, Packet &packet) {
  // client->timerSignup.stop();
  auto &server = Server::instance();
  auto &user_manager = server.user_manager();

  p_ptr->client = conn;

  if (!loadSetupData(packet)) { return; }
  if (!checkVersion()) { return; }
  // if (!checkIfUuidNotBanned()) { return; }
  // if (!checkMd5()) { return; }

  // auto obj = checkPassword();
  // if (obj.isEmpty()) return;

  // int id = obj["id"].toInt();
  // updateUserLoginData(id);
  // user_manager->createNewPlayer(conn, p_ptr->name, obj["avatar"], id, p_ptr->uuid);
  user_manager.createNewPlayer(conn, "player", "liubei", 1, "12345678");
}

static struct cbor_callbacks callbacks = cbor_empty_callbacks;
static std::once_flag callbacks_flag;
static void init_callbacks() {
  callbacks.string = [](void *u, cbor_data data, size_t sz) {
    static_cast<AuthManagerPrivate *>(u)->handle(data, sz);
  };
  callbacks.byte_string = [](void *u, cbor_data data, size_t sz) {
    static_cast<AuthManagerPrivate *>(u)->handle(data, sz);
  };
}

bool AuthManager::loadSetupData(Packet &packet) {
  std::call_once(callbacks_flag, init_callbacks);
  auto data = packet.cborData;
  cbor_decoder_result res;
  int consumed = 0;

  if (packet._len != 4 || packet.requestId != -2 ||
    packet.type != (Router::TYPE_NOTIFICATION | Router::SRC_CLIENT | Router::DEST_SERVER) ||
    packet.command != "Setup")
  {
    goto FAIL;
  }

  p_ptr->reset();
  // 一个array带5个bytes 懒得判那么细了解析出5个就行
  for (int i = 0; i < 6; i++) {
    res = cbor_stream_decode((cbor_data)data.data() + consumed, data.size() - consumed, &callbacks, p_ptr.get());
    if (res.status != CBOR_DECODER_FINISHED) {
      break;
    }
    consumed += res.read;
  }

  if (!p_ptr->is_valid()) {
    goto FAIL;
  }

  return true;

FAIL:
  spdlog::warn("Invalid setup string: version={}", p_ptr->version); 
  if (auto client = p_ptr->client.lock()) {
    Server::instance().sendEarlyPacket(*client, "ErrorDlg", "INVALID SETUP STRING");
    client->disconnectFromHost();
  }

  return false;
}

bool AuthManager::checkVersion() {
  /* TODO 服务端需要配置一个allowed_version的选项；目前返true了事
  auto client_ver = QVersionNumber::fromString(p_ptr->version);
  auto ver = QVersionNumber::fromString(FK_VERSION);
  int cmp = QVersionNumber::compare(ver, client_ver);
  if (cmp != 0) {
    auto errmsg = QString();
    if (cmp < 0) {
      errmsg = QStringLiteral("[\"server is still on version %%2\",\"%1\"]")
                      .arg(FK_VERSION, "1");
    } else {
      errmsg = QStringLiteral("[\"server is using version %%2, please update\",\"%1\"]")
                      .arg(FK_VERSION, "1");
    }

    server->sendEarlyPacket(p_ptr->client, "ErrorDlg", errmsg.toUtf8());
    p_ptr->client->disconnectFromHost();
    return false;
  }
  */
  return true;
}

/*
bool AuthManager::checkIfUuidNotBanned() {
  auto uuid_str = p_ptr->uuid;
  Sqlite3::QueryResult result2 = { {} };
  if (Sqlite3::checkString(uuid_str)) {
    result2 = db->select(QStringLiteral("SELECT * FROM banuuid WHERE uuid='%1';").arg(uuid_str));
  }

  if (!result2.isEmpty()) {
    server->sendEarlyPacket(p_ptr->client, "ErrorDlg", "you have been banned!");
    qInfo() << "Refused banned UUID:" << uuid_str;
    p_ptr->client->disconnectFromHost();
    return false;
  }

  return true;
}

bool AuthManager::checkMd5() {
  auto md5_str = p_ptr->md5;
  if (server->getMd5() != md5_str) {
    server->sendEarlyPacket(p_ptr->client, "ErrorMsg", "MD5 check failed!");
    server->sendEarlyPacket(p_ptr->client, "UpdatePackage", Pacman->getPackSummary().toUtf8());
    p_ptr->client->disconnectFromHost();
    return false;
  }
  return true;
}

QMap<QString, QString> AuthManager::queryUserInfo(const QByteArray &password) {
  auto db = server->getDatabase();
  auto pw = password;
  auto sql_find = QStringLiteral("SELECT * FROM userinfo WHERE name='%1';")
    .arg(p_ptr->name);
  auto sql_count_uuid = QStringLiteral("SELECT COUNT() AS cnt FROM uuidinfo WHERE uuid='%1';")
    .arg(p_ptr->uuid);

  auto result = db->select(sql_find);
  if (result.isEmpty()) {
    auto result2 = db->select(sql_count_uuid);
    auto num = result2[0]["cnt"].toInt();
    if (num >= server->getConfig("maxPlayersPerDevice").toInt()) {
      return {};
    }
    auto salt_gen = QRandomGenerator::securelySeeded();
    auto salt = QByteArray::number(salt_gen(), 16);
    pw.append(salt);
    auto passwordHash =
      QCryptographicHash::hash(pw, QCryptographicHash::Sha256).toHex();
    auto sql_reg = QString("INSERT INTO userinfo (name,password,salt,\
avatar,lastLoginIp,banned) VALUES ('%1','%2','%3','%4','%5',%6);")
      .arg(p_ptr->name).arg(QString(passwordHash))
      .arg(salt).arg("liubei").arg(p_ptr->client->peerAddress())
      .arg("FALSE");
    db->exec(sql_reg);
    result = db->select(sql_find); // refresh result
    auto obj = result[0];

    auto info_update = QString("INSERT INTO usergameinfo (id, registerTime) VALUES (%1, %2);").arg(obj["id"].toInt()).arg(QDateTime::currentSecsSinceEpoch());
    db->exec(info_update);
  }
  return result[0];
}

QMap<QString, QString> AuthManager::checkPassword() {
  auto client = p_ptr->client;
  auto name = p_ptr->name;
  auto password = p_ptr->password;
  bool passed = false;
  const char *error_msg = nullptr;
  QMap<QString, QString> obj;
  int id;
  QByteArray salt;
  QByteArray passwordHash;
  auto players = server->getPlayers();

  unsigned char buf[4096] = {0};
  RSA_private_decrypt(RSA_size(p_ptr->rsa), (const unsigned char *)password.data(),
                      buf, p_ptr->rsa, RSA_PKCS1_PADDING);
  auto decrypted_pw =
      QByteArray::fromRawData((const char *)buf, strlen((const char *)buf));

  if (decrypted_pw.length() > 32) {
    // TODO: 先不加密吧，把CBOR搭起来先
    // auto aes_bytes = decrypted_pw.first(32);

    // tell client to install aes key
    // server->sendEarlyPacket(client, "InstallKey", "");
    // client->installAESKey(aes_bytes);
    decrypted_pw.remove(0, 32);
  } else {
    // FIXME
    // decrypted_pw = "\xFF";
    error_msg = "unknown password error";
    goto FAIL;
  }

  if (name.isEmpty() || !Sqlite3::checkString(name) || !server->checkBanWord(name)) {
    error_msg = "invalid user name";
    goto FAIL;
  }

  if (!server->nameIsInWhiteList(name)) {
    error_msg = "user name not in whitelist";
    goto FAIL;
  }

  obj = queryUserInfo(decrypted_pw);
  if (obj.isEmpty()) {
    error_msg = "cannot register more new users on this device";
    goto FAIL;
  }

  // check ban account
  id = obj["id"].toInt();
  passed = obj["banned"].toInt() == 0;
  if (!passed) {
    error_msg = "you have been banned!";
    goto FAIL;
  }

  // check if password is the same
  salt = obj["salt"].toLatin1();
  decrypted_pw.append(salt);
  passwordHash =
    QCryptographicHash::hash(decrypted_pw, QCryptographicHash::Sha256).toHex();
  passed = (passwordHash == obj["password"]);
  if (!passed) {
    error_msg = "username or password error";
    goto FAIL;
  }

  if (players.value(id)) {
    auto player = players.value(id);
    // 顶号机制，如果在线的话就让他变成不在线
    if (player->getState() == Player::Online || player->getState() == Player::Robot) {
      player->doNotify("ErrorDlg", "others logged in again with this name");
      emit player->kicked();
    }

    if (player->getState() == Player::Offline) {
      updateUserLoginData(id);
      player->reconnect(client);
      passed = true;
      return {};
    } else {
      error_msg = "others logged in with this name";
      passed = false;
    }
  }

FAIL:
  if (!passed) {
    qInfo() << client->peerAddress() << "lost connection:" << error_msg;
    server->sendEarlyPacket(client, "ErrorDlg", error_msg);
    client->disconnectFromHost();
    return {};
  }

  return obj;
}

void AuthManager::updateUserLoginData(int id) {
  server->beginTransaction();
  auto sql_update =
    QStringLiteral("UPDATE userinfo SET lastLoginIp='%1' WHERE id=%2;")
    .arg(p_ptr->client->peerAddress())
    .arg(id);
  db->exec(sql_update);

  auto uuid_update = QString("REPLACE INTO uuidinfo (id, uuid) VALUES (%1, '%2');")
    .arg(id).arg(p_ptr->uuid);
  db->exec(uuid_update);

  // 来晚了，有很大可能存在已经注册但是表里面没数据的人
  db->exec(QStringLiteral("INSERT OR IGNORE INTO usergameinfo (id) VALUES (%1);").arg(id));
  auto info_update = QStringLiteral("UPDATE usergameinfo SET lastLoginTime=%2 where id=%1;").arg(id).arg(QDateTime::currentSecsSinceEpoch());
  db->exec(info_update);
  server->endTransaction();
}
*/
