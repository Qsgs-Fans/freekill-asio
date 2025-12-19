// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/c-wrapper.h"
#include "core/packman.h"
#include "server/user/auth.h"
#include "server/user/user_manager.h"
#include "server/user/serverplayer.h"
#include "server/server.h"
#include "network/client_socket.h"
#include "network/router.h"

#include <openssl/bn.h>
#include <openssl/rsa.h>
#include <openssl/pem.h>
#include <openssl/sha.h>
#include <openssl/md5.h>

#include <nlohmann/json.hpp>
using json = nlohmann::json;

#include "3rdparty/semver.hpp"

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
  if (!std::filesystem::is_directory("server")) {
    throw std::runtime_error("server/ is not a directory so I can't generate key pairs. Quitting!");
  }
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

  std::string public_key;
  std::ifstream file("server/rsa_pub");
  if (file) {
    std::ostringstream ss;
    ss << file.rdbuf();
    public_key = ss.str();
  }

  auto bin = json::to_cbor(
    json::binary(
      std::vector<std::uint8_t>(public_key.begin(), public_key.end()))
  );
  public_key_cbor = std::string(bin.begin(), bin.end());
}

AuthManager::~AuthManager() noexcept {
}

std::string_view AuthManager::getPublicKeyCbor() const {
  return public_key_cbor;
}

void AuthManager::processNewConnection(std::shared_ptr<ClientSocket> conn, Packet &packet) {
  conn->timerSignup->cancel();
  auto &server = Server::instance();
  auto &user_manager = server.user_manager();

  p_ptr->client = conn;

  if (!loadSetupData(packet)) { return; }
  if (!checkVersion()) { return; }
  if (!checkIfUuidNotBanned()) { return; }
  if (!checkMd5()) { return; }

  auto obj = checkPassword();
  if (obj.empty()) return;

  int id = atoi(obj["id"].c_str());
  updateUserLoginData(id);
  user_manager.createNewPlayer(conn, p_ptr->name, obj["avatar"], id, p_ptr->uuid);
}

static struct cbor_callbacks callbacks = cbor_empty_callbacks;
static std::once_flag callbacks_flag;
static void init_callbacks() {
  callbacks.string = [](void *u, cbor_data data, uint64_t sz) {
    static_cast<AuthManagerPrivate *>(u)->handle(data, sz);
  };
  callbacks.byte_string = [](void *u, cbor_data data, uint64_t sz) {
    static_cast<AuthManagerPrivate *>(u)->handle(data, sz);
  };
}

bool AuthManager::loadSetupData(const Packet &packet) {
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
    res = cbor_stream_decode(
      (cbor_data)data.data() + consumed,
      data.size() - consumed,
      &callbacks,
      p_ptr.get()
    );
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
  semver::range_set range;
  semver::parse(">=0.5.17 <0.6.0", range);

  auto client = p_ptr->client.lock();
  if (!client) return false;

  const char *errmsg;

  auto &ver = p_ptr->version;
  semver::version version;
  if (semver::parse(ver, version) && range.contains(version)) {
    return true;
  } else {
    errmsg = R"(["server supports version %1, please update","0.5.17+"])";
  }

  Server::instance().sendEarlyPacket(*client, "ErrorDlg", errmsg);
  client->disconnectFromHost();
  return false;
}


bool AuthManager::checkIfUuidNotBanned() {
  auto &server = Server::instance();
  auto &db = server.database();
  auto uuid_str = p_ptr->uuid;
  if (!Sqlite3::checkString(uuid_str)) return false;

  auto result2 = db.select(
    fmt::format("SELECT * FROM banuuid WHERE uuid='{}';", uuid_str));

  if (result2.empty()) return true;

  if (auto client = p_ptr->client.lock(); client) {
    Server::instance().sendEarlyPacket(*client, "ErrorDlg", "you have been banned!");
    spdlog::info("Refused banned UUID: {}", uuid_str);
    client->disconnectFromHost();
  }
  return false;
}

bool AuthManager::checkMd5() {
  auto &server = Server::instance();
  auto md5_str = p_ptr->md5;

  if (server.getMd5() != md5_str) {
    if (auto client = p_ptr->client.lock()) {
      server.sendEarlyPacket(*client, "ErrorMsg", "MD5 check failed!");
      server.sendEarlyPacket(*client, "UpdatePackage", PackMan::instance().summary());
      client->disconnectFromHost();
    }
    return false;
  }

  return true;
}

std::map<std::string, std::string> AuthManager::queryUserInfo(const std::string_view &password) {
  auto &server = Server::instance();
  auto &db = server.database();

  auto sql_find = fmt::format("SELECT * FROM userinfo WHERE name='{}';", p_ptr->name);
  auto sql_count_uuid =
    fmt::format("SELECT COUNT() AS cnt FROM uuidinfo WHERE uuid='{}';", p_ptr->uuid);

  auto result = db.select(sql_find);
  if (!result.empty()) return result[0];

  // 以下为注册流程

  auto result2 = db.select(sql_count_uuid);
  auto num = atoi(result2[0]["cnt"].c_str());
  if (num >= server.config().maxPlayersPerDevice) {
    return {};
  }

  char saltbuf[9];
  {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint32_t> dis;
    snprintf(saltbuf, 9, "%08x", dis(gen));
  }

  auto pw = std::string(password) + saltbuf;
  unsigned char hash[SHA256_DIGEST_LENGTH];
  SHA256((const u_char *)pw.data(), pw.size(), hash);

  std::string passwordHash;
  passwordHash.reserve(64);
  for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
    char buf[3];
    snprintf(buf, sizeof(buf), "%02x", hash[i]);
    passwordHash += buf;
  }

  auto sql_reg = fmt::format(
    "INSERT INTO userinfo "
    "(name, password, salt, avatar, lastLoginIp, banned) "
    "VALUES ('{}','{}','{}','{}','{}',{});",

    p_ptr->name,
    passwordHash,
    saltbuf,
    "liubei",
    p_ptr->client.lock()->peerAddress(),
    "FALSE"
  );

  db.exec(sql_reg);

  result = db.select(sql_find);
  auto obj = result[0];

  using namespace std::chrono;
  auto now = system_clock::now();
  auto timestamp = duration_cast<seconds>(now.time_since_epoch()).count();
  auto info_update = fmt::format(
    "INSERT INTO usergameinfo (id, registerTime) VALUES ({}, {});",
    obj["id"], timestamp
  );
  db.exec(info_update);

  return result[0];
}

std::string AuthManager::getBanExpire(std::map<std::string, std::string> &info) {
  auto &server = Server::instance();
  auto &db = server.database();

  auto result = db.select(fmt::format(
    "SELECT uid, expireAt FROM tempban WHERE uid={};",
    info["id"]
  ));
  if (result.empty()) return "forever";

  using namespace std::chrono;
  int64_t expire = atoll(result[0]["expireAt"].c_str());
  auto tp = system_clock::time_point(seconds(expire));

  if (tp <= system_clock::now()) {
    db.exec(fmt::format(
      "DELETE FROM tempban WHERE uid={};",
      info["id"]
    ));
    db.exec(fmt::format(
      "UPDATE userinfo SET banned=0 WHERE id={};",
      info["id"]
    ));
    return "expired";
  }

  std::time_t now_time_t = system_clock::to_time_t(tp);
  std::tm local_tm = *std::localtime(&now_time_t);

  return fmt::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}.",
               local_tm.tm_year + 1900, local_tm.tm_mon + 1, local_tm.tm_mday,
               local_tm.tm_hour, local_tm.tm_min, local_tm.tm_sec);

}

std::map<std::string, std::string> AuthManager::checkPassword() {
  auto &server = Server::instance();
  auto &um = server.user_manager();
  bool passed = false;
  std::string error_msg = "";

  // p_ptr的数据
  auto client = p_ptr->client.lock();
  auto name = p_ptr->name;

  // 密码相关数据
  std::string decrypted_pw;
  std::string passwordHash;

  // 数据库查询结果
  std::map<std::string, std::string> obj;

  if (!client) {
    goto FAIL;
  }

  if (name.empty() || !Sqlite3::checkString(name)
    || !server.checkBanWord(name)) {

    error_msg = "invalid user name";
    goto FAIL;
  }

  if (!server.nameIsInWhiteList(name)) {
    error_msg = "user name not in whitelist";
    goto FAIL;
  }

  {
    char buf[4096] = {0};
    RSA_private_decrypt(
      RSA_size(p_ptr->rsa), (const u_char *)p_ptr->password.data(),
      (u_char *)buf, p_ptr->rsa, RSA_PKCS1_PADDING
    );
    decrypted_pw = std::string { buf };
  }

  if (decrypted_pw.size() > 32) {
    // TODO: 先不加密吧，把CBOR搭起来先
    // auto aes_bytes = decrypted_pw.first(32);

    // tell client to install aes key
    // server->sendEarlyPacket(client, "InstallKey", "");
    // client->installAESKey(aes_bytes);
    decrypted_pw = decrypted_pw.substr(32);
  } else {
    error_msg = "unknown password error";
    goto FAIL;
  }

  obj = queryUserInfo(decrypted_pw);
  if (obj.empty()) {
    error_msg = "cannot register more new users on this device";
    goto FAIL;
  }

  // check ban account
  if (obj["banned"] != "0") {
    auto expiry = getBanExpire(obj);
    if (expiry == "expired") {
      // 无事发生
    } else if (expiry == "forever") {
      passed = false;
      error_msg = "you have been banned!";
      goto FAIL;
    } else {
      passed = false;
      error_msg = fmt::format("[\"you have been banned! expire at %1\", \"{}\"]", expiry);
      goto FAIL;
    }
  }

  // check if password is the same
  decrypted_pw += obj["salt"];
  unsigned char hash[SHA256_DIGEST_LENGTH];
  SHA256((const u_char *)decrypted_pw.data(), decrypted_pw.size(), hash);

  passwordHash.reserve(64);
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

  if (auto player = um.findPlayer(atoi(obj["id"].c_str())).lock(); player) {

    if (player->insideGame()) {
      updateUserLoginData(player->getId());
      player->reconnect(client);
      passed = true;
      return {};
    } else if (player->isOnline()) {
      player->doNotify("ErrorDlg", "others logged in again with this name");
      player->emitKicked();
    } else {
      // 又不在游戏内，又不在线，又正常被findPlayer
      // 这不就是卡死了 针对卡死的我们直接删除然后继续走认证
      // error_msg = "others logged in with this name";
      // passed = false;
      um.deletePlayer(*player);
    }
  }

FAIL:
  if (!passed) {
    if (auto c = p_ptr->client.lock(); c) {
      spdlog::info("{} lost connection: {}", c->peerAddress(), error_msg);
      server.sendEarlyPacket(*c, "ErrorDlg", error_msg);
      c->disconnectFromHost();
    }
    return {};
  }

  return obj;
}

void AuthManager::updateUserLoginData(int id) {
  auto &server = Server::instance();
  auto &db = server.database();
  auto client = p_ptr->client.lock();
  if (!client) return;

  server.beginTransaction();

  auto sql_update = fmt::format(
    "UPDATE userinfo SET lastLoginIp='{}' WHERE id={};", client->peerAddress(), id);
  db.exec(sql_update);

  auto uuid_update = fmt::format(
    "REPLACE INTO uuidinfo (id, uuid) VALUES ({}, '{}');", id, p_ptr->uuid);
  db.exec(uuid_update);

  // 来晚了，有很大可能存在已经注册但是表里面没数据的人
  db.exec(fmt::format("INSERT OR IGNORE INTO usergameinfo (id) VALUES ({});", id));

  using namespace std::chrono;
  auto now = system_clock::now();
  auto timestamp = duration_cast<seconds>(now.time_since_epoch()).count();
  auto info_update = fmt::format(
    "UPDATE usergameinfo SET lastLoginTime={} where id={};", timestamp, id);
  db.exec(info_update);

  server.endTransaction();
}

