// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/c-wrapper.h"
#include "core/packman.h"
#include "server/user/auth.h"
#include "server/user/user_manager.h"
#include "server/user/player.h"
#include "server/server.h"
#include "network/client_socket.h"
#include "network/router.h"
#include <openssl/bn.h>
#include <openssl/rsa.h>
#include <openssl/pem.h>
#include <openssl/sha.h>
#include <openssl/md5.h>
#include <lua5.4/lua.h>
#include <random>
#include <chrono>

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

  public_key_cbor.clear();
  public_key_cbor.reserve(550);
  u_char buf[10]; size_t buflen;
  buflen = cbor_encode_uint(public_key.size(), buf, 10);
  buf[0] += 0x40;

  public_key_cbor = std::string { (char*)buf, buflen } + public_key;
}

AuthManager::~AuthManager() noexcept {
  // delete p_ptr;
}

std::string_view AuthManager::getPublicKeyCbor() const {
  return public_key_cbor;
}

void AuthManager::processNewConnection(std::shared_ptr<ClientSocket> conn, Packet &packet) {
  // client->timerSignup.stop();
  auto &server = Server::instance();
  auto &user_manager = server.user_manager();

  p_ptr->client = conn;

  if (!loadSetupData(packet)) { return; }
  if (!checkVersion()) { return; }
  if (!checkIfUuidNotBanned()) { return; }
  if (!checkMd5()) { return; }

  auto obj = checkPassword();
  if (obj.empty()) return;

  int id = std::stoi(obj["id"]);
  updateUserLoginData(id);
  user_manager.createNewPlayer(conn, p_ptr->name, obj["avatar"], id, p_ptr->uuid);
  // user_manager.createNewPlayer(conn, "player", "liubei", 1, "12345678");
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


bool AuthManager::checkIfUuidNotBanned() {
  auto &server = Server::instance();
  auto uuid_str = p_ptr->uuid;
  Sqlite3::QueryResult result2 = { {} };
  if (Sqlite3::checkString(uuid_str)) {
    result2 = db->select(fmt::format("SELECT * FROM banuuid WHERE uuid='{}';", uuid_str));
  }

  if (!result2.empty()) {
    if (auto client = p_ptr->client.lock()) {
      Server::instance().sendEarlyPacket(*client, "ErrorDlg", "you have been banned!");
      spdlog::info("Refused banned UUID:%s", uuid_str);
      client->disconnectFromHost();
    }
    return false;
  }

  return true;
}

bool AuthManager::checkMd5() {
  auto &server = Server::instance();
  auto md5_str = p_ptr->md5;
  //因为server还没有md5，先搁置
  /* 
  if (server.getMd5() != md5_str) {
    if (auto client = p_ptr->client.lock()) {
      server.sendEarlyPacket(*client, "ErrorMsg", "MD5 check failed!");
      server.sendEarlyPacket(*client, "UpdatePackage", Pacman->getPackSummary().toUtf8());
      client->disconnectFromHost();
    }
    return false;
  }
  */
  return true;
}

std::map<std::string, std::string> AuthManager::queryUserInfo(const std::string &password) {
  auto &server = Server::instance();
  auto pw = password;
  auto sql_find = fmt::format("SELECT * FROM userinfo WHERE name='{}';", p_ptr->name);
  auto sql_count_uuid = fmt::format("SELECT COUNT() AS cnt FROM uuidinfo WHERE uuid='{}';", p_ptr->uuid);

  auto result = db->select(sql_find);
  if (result.empty()) {
    auto result2 = db->select(sql_count_uuid);
    auto num = std::stoi(result2[0]["cnt"]);
    // if (num >= std::stoi(server.getConfig("maxPlayersPerDevice"))) {
    //   return {};
    // }
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dis;
    uint64_t salt_val = dis(gen);
    std::string salt = std::to_string(salt_val);
    pw.append(salt);
    // 计算密码哈希
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char *)pw.data(), pw.size(), hash);
    
    std::string passwordHash;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
      char buf[3];
      snprintf(buf, sizeof(buf), "%02x", hash[i]);
      passwordHash += buf;
    }
    auto sql_reg = fmt::format("INSERT INTO userinfo (name,password,salt,\
avatar,lastLoginIp,banned) VALUES ('{}','{}','{}','{}','{}',{});", p_ptr->name, passwordHash, salt, "liubei", p_ptr->client.lock()->peerAddress(), "FALSE");
    db->exec(sql_reg);
    result = db->select(sql_find); // refresh result
    auto obj = result[0];

    // 获取当前时间戳
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

    auto info_update = fmt::format("INSERT INTO usergameinfo (id, registerTime) VALUES ({}, {});", std::stoi(obj["id"]), timestamp);
    db->exec(info_update);
  }
  return result[0];
}

std::map<std::string, std::string> AuthManager::checkPassword() {
  auto &server = Server::instance();
  auto client = p_ptr->client;
  auto name = std::string_view(p_ptr->name);
  auto password = p_ptr->password;
  bool passed = false;
  const char *error_msg = nullptr;
  std::map<std::string, std::string> obj;
  int id;
  std::string salt;
  std::string passwordHash;
  auto players = server.user_manager().getPlayers();

  unsigned char buf[4096] = {0};
  RSA_private_decrypt(RSA_size(p_ptr->rsa), (const unsigned char *)password.data(),
                      buf, p_ptr->rsa, RSA_PKCS1_PADDING);
  auto decrypted_pw =
      std::string { (const char *)buf, strlen((const char *)buf) };

  if (decrypted_pw.length() > 32) {
    // TODO: 先不加密吧，把CBOR搭起来先
    // auto aes_bytes = decrypted_pw.first(32);

    // tell client to install aes key
    // server->sendEarlyPacket(client, "InstallKey", "");
    // client->installAESKey(aes_bytes);
    decrypted_pw = decrypted_pw.substr(0, 32);
  } else {
    // FIXME
    // decrypted_pw = "\xFF";
    error_msg = "unknown password error";
    goto FAIL;
  }

  if (name.empty() || !Sqlite3::checkString(name) || !server.checkBanWord(name)) {
    error_msg = "invalid user name";
    goto FAIL;
  }
  // TODO: 白名单先搁置
  /* 
  if (!server->nameIsInWhiteList(name)) {
    error_msg = "user name not in whitelist";
    goto FAIL;
  }
 */
  obj = queryUserInfo(decrypted_pw);
  if (obj.empty()) {
    error_msg = "cannot register more new users on this device";
    goto FAIL;
  }

  // check ban account
  id = std::stoi(obj["id"]);
  passed = std::stoi(obj["banned"]) == 0;
  if (!passed) {
    error_msg = "you have been banned!";
    goto FAIL;
  }

  // check if password is the same
  salt = obj["salt"];
  decrypted_pw.append(salt);
  // 计算密码哈希
  unsigned char hash[SHA256_DIGEST_LENGTH];
  SHA256((const unsigned char *)decrypted_pw.data(), decrypted_pw.size(), hash);

  for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
    char buf[3];
    snprintf(buf, sizeof(buf), "%02x", hash[i]);
    passwordHash += buf;
  }
  passed = (passwordHash == obj["password"]);
  if (!passed) {
    error_msg = "username or password error";
    goto FAIL;
  }

  if (players.contains(id)) {
    auto player = players.at(id);
    // 顶号机制，如果在线的话就让他变成不在线
    if (player->getState() == Player::Online || player->getState() == Player::Robot) {
      player->doNotify("ErrorDlg", "others logged in again with this name");
      // FIXME: emit kicked()
      // player->set_kicked_callback();
    }

    if (player->getState() == Player::Offline) {
      updateUserLoginData(id);
      // FIXME: player->reconnect()
      // player->reconnect(client);
      passed = true;
      return {};
    } else {
      error_msg = "others logged in with this name";
      passed = false;
    }
  }

FAIL:
  if (!passed) {
    if (auto client_locked = p_ptr->client.lock()) {
      spdlog::info("{}lost connection:{}", client_locked->peerAddress(), error_msg);
      server.sendEarlyPacket(*client_locked, "ErrorDlg", error_msg);
      client_locked->disconnectFromHost();
    }
    return {};
  }

  return obj;
}

void AuthManager::updateUserLoginData(int id) {
  auto &server = Server::instance();
  std::mutex transaction_mutex;

  // server.beginTransaction();
  transaction_mutex.lock();
  db->exec("BEGIN;");

  auto sql_update =
    fmt::format("UPDATE userinfo SET lastLoginIp='{}' WHERE id={};", p_ptr->client.lock()->peerAddress(), id);
  db->exec(sql_update);

  auto uuid_update = fmt::format("REPLACE INTO uuidinfo (id, uuid) VALUES ({}, '{}');", id, p_ptr->uuid);
  db->exec(uuid_update);

  // 来晚了，有很大可能存在已经注册但是表里面没数据的人
  db->exec(fmt::format("INSERT OR IGNORE INTO usergameinfo (id) VALUES ({});", id));

  // 获取当前时间戳
  auto now = std::chrono::system_clock::now();
  auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
  
  auto info_update = fmt::format("UPDATE usergameinfo SET lastLoginTime={1} where id={0};", id, timestamp);
  db->exec(info_update);

  // server.endTransaction();
  db->exec("COMMIT;");
  transaction_mutex.unlock();
}

