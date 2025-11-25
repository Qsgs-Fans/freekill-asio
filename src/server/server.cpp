// SPDX-License-Identifier: GPL-3.0-or-later

#include "server/server.h"
#include "server/room/room_manager.h"
#include "server/room/room.h"
#include "server/room/lobby.h"
#include "server/user/user_manager.h"
#include "server/user/auth.h"
#include "server/user/player.h"
#include "network/server_socket.h"
#include "network/client_socket.h"
#include "network/router.h"
#include "network/http_listener.h"
#include "server/gamelogic/roomthread.h"

#include "server/admin/shell.h"

#include "server/io/dbthread.hpp"

#include "core/c-wrapper.h"
#include "core/util.h"
#include "core/packman.h"

#include <nlohmann/json.hpp>

namespace asio = boost::asio;
using asio::awaitable;
using asio::detached;
using asio::use_awaitable;
using asio::redirect_error;

using json = nlohmann::json;

static std::unique_ptr<Server> server_instance = nullptr;

Server &Server::instance() {
  if (!server_instance) {
    server_instance = std::unique_ptr<Server>(new Server);
  }
  return *server_instance;
}

Server::Server() : m_socket { nullptr } {
  m_user_manager = std::make_unique<UserManager>();
  m_room_manager = std::make_unique<RoomManager>();

  db = std::make_unique<Sqlite3>();

  reloadConfig();
  refreshMd5();

  using namespace std::chrono;
  start_timestamp =
    duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

Server::~Server() {
}

awaitable<void> Server::heartbeat() {
  using namespace std::chrono_literals;
  for (;;) {
    heartbeat_timer->expires_after(30s);
    boost::system::error_code ec;
    co_await heartbeat_timer->async_wait(redirect_error(use_awaitable, ec));
    if (ec) {
      spdlog::error(ec.message());
      break;
    }
    std::vector<std::shared_ptr<Player>> to_delete;
    for (auto &[_, p] : m_user_manager->getPlayers()) {
      if (p->isOnline() && p->ttl <= 0) {
        to_delete.push_back(p);
      }
    }

    for (auto &p : to_delete) {
      p->emitKicked();
    }

    for (auto &[_, p] : m_user_manager->getPlayers()) {
      if (p->isOnline()) {
        p->ttl--;
        p->doNotify("Heartbeat", "");
      }
    }
  }
}

void Server::listen(io_context &io_ctx, tcp::endpoint end, udp::endpoint uend) {
  main_io_ctx = &io_ctx;

  m_socket = std::make_unique<ServerSocket>(io_ctx, end, uend);
  m_socket->set_new_connection_callback([this](std::shared_ptr<ClientSocket> p) {
    m_user_manager->processNewConnection(p);
  });
  m_socket->start();

  heartbeat_timer = std::make_unique<asio::steady_timer>(io_ctx);
  asio::co_spawn(io_ctx, heartbeat(), detached);

  m_shell = std::make_unique<Shell>();
  m_shell->start();

  auto gamedb = std::make_unique<Sqlite3>("./server/game.db", "./server/gamedb_init.sql");  // 初始化
  this->gamedb = std::make_unique<DbThread>(*main_io_ctx, std::move(gamedb));
  this->gamedb->start();

  // FIXME 此处仅供测试用
  // (new HttpListener(tcp::endpoint { tcp::v6(), 9000 }))->start();
}

void Server::stop() {
  main_io_ctx->stop();
}

void Server::destroy() {
  // spdlog::debug("[MEMORY] server_instance destructed");
  server_instance = nullptr;
}

auto Server::context() -> decltype(*main_io_ctx) {
  return *main_io_ctx;
}

UserManager &Server::user_manager() {
  return *m_user_manager;
}

RoomManager &Server::room_manager() {
  return *m_room_manager;
}

Sqlite3 &Server::database() {
  return *db;
}

DbThread &Server::gameDatabase() {
  return *gamedb;
}

Shell &Server::shell() {
  return *m_shell;
}

void Server::sendEarlyPacket(ClientSocket &client, const std::string_view &type, const std::string_view &msg) {
  auto buf = Cbor::encodeArray({
    -2,
    Router::TYPE_NOTIFICATION | Router::SRC_SERVER | Router::DEST_CLIENT,
    type,
    msg,
  });
  client.send(std::make_shared<std::string>(buf));
}

RoomThread &Server::createThread() {
  auto thr = std::make_unique<RoomThread>(*main_io_ctx);
  auto id = thr->id();
  m_threads[id] = std::move(thr);
  return *m_threads[id];
}

void Server::removeThread(int threadId) {
  auto it = m_threads.find(threadId);
  if (it != m_threads.end()) {
    m_threads.erase(threadId);
  }
}

std::weak_ptr<RoomThread> Server::getThread(int threadId) {
  if (!m_threads.contains(threadId)) return {};
  return m_threads[threadId];
}

RoomThread &Server::getAvailableThread() {
  for (const auto &it : m_threads) {
    auto &thr = it.second;
    if (thr->isOutdated()) continue;
    if (thr->isFull()) continue;
    return *thr;
  }

  return createThread();
}

const std::unordered_map<int, std::shared_ptr<RoomThread>> &
  Server::getThreads() const
{
  return m_threads;
}

void Server::broadcast(const std::string_view &command, const std::string_view &jsonData) {
  for (auto &[_, p] : user_manager().getPlayers()) {
    p->doNotify(command, jsonData);
  }
}

void ServerConfig::loadConf(const char* jsonStr) {
  json root;

  try {
    root = nlohmann::json::parse(jsonStr);
  } catch (const std::exception& e) {
    spdlog::error("JSON parse error: {}", e.what());
    return;
  }

  static auto loadStrVec = [&](auto& dst, const char* key) {
    if (root.contains(key) && root[key].is_array()) {
      dst.clear();
      for (auto& v : root[key]) if (v.is_string()) dst.push_back(v.get<std::string>());
    }
  };

  loadStrVec(banWords, "banWords");
  loadStrVec(hiddenPacks, "hiddenPacks");
  loadStrVec(disabledFeatures, "disabledFeatures");

  description         = root.value("description", description);
  iconUrl             = root.value("iconUrl", iconUrl);
  capacity            = root.value("capacity", capacity);
  tempBanTime         = root.value("tempBanTime", tempBanTime);
  motd                = root.value("motd", motd);
  roomCountPerThread  = root.value("roomCountPerThread", roomCountPerThread);
  maxPlayersPerDevice = root.value("maxPlayersPerDevice", maxPlayersPerDevice);
  enableWhitelist     = root.value("enableWhitelist", enableWhitelist);

  // 兼容一下之前的配置信息
  if (root.value("enableBots", true) == false &&
    std::ranges::find(disabledFeatures, "AddRobot") == disabledFeatures.end())
    disabledFeatures.push_back("AddRobot");

  if (root.value("enableChangeRoom", true) == false &&
    std::ranges::find(disabledFeatures, "ChangeRoom") == disabledFeatures.end())
    disabledFeatures.push_back("ChangeRoom");
}

void Server::reloadConfig() {
  std::string jsonStr = "{}";

  std::ifstream file("freekill.server.config.json", std::ios::binary);
  if (file.is_open()) {
    file.seekg(0, std::ios::end);
    size_t size = file.tellg();
    jsonStr.resize(size);
    file.seekg(0, std::ios::beg);
    file.read(&jsonStr[0], size);
    file.close();
  }

  m_config = std::make_unique<ServerConfig>();
  m_config->loadConf(jsonStr.c_str());
}

const ServerConfig &Server::config() const { return *m_config; }

bool Server::checkBanWord(const std::string_view &str) {
  auto arr = m_config->banWords;
  for (auto &s : arr) {
    if (str.find(s) != std::string_view::npos) {
      return false;
    }
  }
  return true;
}

void Server::temporarilyBan(int playerId) {
  auto player = m_user_manager->findPlayer(playerId).lock();
  if (!player) return;

  auto socket = player->getRouter().getSocket();
  std::string addr;
  if (!socket) {
    static constexpr const char *sql_find =
      "SELECT lastLoginIp FROM userinfo WHERE id={};";
    auto result = db->select(fmt::format(sql_find, playerId));
    if (result.empty())
      return;

    auto obj = result[0];
    addr = obj["lastLoginIp"];
  } else {
    addr = socket->peerAddress();
  }
  temp_banlist.push_back(addr);

  auto time = m_config->tempBanTime;
  using namespace std::chrono;
  auto timer = std::make_shared<asio::steady_timer>(*main_io_ctx, seconds(time * 60));
  // Server不会析构，先不weak
  timer->async_wait([this, addr, timer](const std::error_code& ec){
    if (!ec) {
      auto it = std::ranges::find(temp_banlist, addr);
      if (it != temp_banlist.end())
        temp_banlist.erase(it);
    } else {
      spdlog::error("error in tempBan timer: {}", ec.message());
    }
  });
  player->emitKicked();
}

bool Server::isTempBanned(const std::string_view &addr) const {
  return (std::ranges::find(temp_banlist, addr) != temp_banlist.end());
}

int Server::isMuted(int playerId) const {
  auto result = db->select(fmt::format("SELECT expireAt, type FROM tempmute WHERE uid={};", playerId));
  if (result.empty())
    return 0; // 0为未被禁言

  auto obj = result[0];
  auto expireAt = std::stoll(obj["expireAt"]);
  auto now = std::chrono::duration_cast<std::chrono::seconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();

  if (now > expireAt) {
    db->exec(fmt::format("DELETE FROM tempmute WHERE uid={};", playerId));
    return 0;
  }

  int type = 1;
  if (obj.find("type") != obj.end() && !obj["type"].empty()) {
    type = std::stoi(obj["type"]);
  }

  return type; // 1为完全禁言，2为禁止$开头
}

void Server::beginTransaction() {
  transaction_mutex.lock();
  db->exec("BEGIN;");
}

void Server::endTransaction() {
  db->exec("COMMIT;");
  transaction_mutex.unlock();
}

const std::string &Server::getMd5() const {
  return md5;
}

void Server::refreshMd5() {
  if (!main_io_ctx) {
    return _refreshMd5();
  }
  asio::dispatch(*main_io_ctx, [this] { _refreshMd5(); });
}

void Server::_refreshMd5() {
  md5 = calcFileMD5();

  PackMan::instance().refreshSummary();

  auto &rm = room_manager();
  for (auto &[_, room] : rm.getRooms()) {
    if (!room->isOutdated()) continue;

    if (!room->isStarted()) {
      for (auto pConnId : room->getPlayers()) {
        auto p = m_user_manager->findPlayerByConnId(pConnId).lock();
        if (p) p->emitKicked();
        // _checkAbandon无论如何都会通过asio::post延迟进行的
      }
    } else {
      // const char * 会给末尾加0 手造二进制数据的话必须考虑
      using namespace std::string_view_literals;
      static constexpr const auto log =
        "\xA2"                      // map(2)
        "\x44" "type"               // key(0) : bytes(4)
        "\x4D" "#RoomOutdated"      // value(0) : bytes(13)
        "\x45" "toast"              // key(1) : bytes(5)
        "\xF5"sv;                   // value(1): true
      room->doBroadcastNotify(room->getPlayers(), "GameLog", log);
    }
  }

  std::vector<int> to_rm;
  for (auto &[id, thread] : m_threads) {
    if (thread->isOutdated() && thread->getRefCount() == 0)
      to_rm.push_back(id);
  }
  for (auto id : to_rm) {
    removeThread(id);
  }

  std::vector<int> to_kick;
  for (auto &[pConnId, _] : rm.lobby().lock()->getPlayers()) {
    to_kick.push_back(pConnId);
  }
  for (auto pConnId : to_kick) {
    auto p = m_user_manager->findPlayerByConnId(pConnId).lock();
    if (p) p->emitKicked();
  }
}

int64_t Server::getUptime() const {
  using namespace std::chrono;
  auto now =
    duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();

  return now - start_timestamp;
}

bool Server::nameIsInWhiteList(const std::string_view &name) const {
  if (!m_config->enableWhitelist) return true;
  auto obj = db->select(fmt::format(
    "SELECT name FROM whitelist WHERE name='{}';", name));
  return !obj.empty();
}
