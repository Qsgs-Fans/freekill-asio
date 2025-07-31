// SPDX-License-Identifier: GPL-3.0-or-later

#include "server/server.h"
#include "server/room/room_manager.h"
#include "server/room/room.h"
#include "server/room/lobby.h"
#include "server/user/user_manager.h"
#include "server/user/auth.h"
#include "network/server_socket.h"
#include "network/client_socket.h"
#include "network/router.h"
#include "server/gamelogic/roomthread.h"
#include "server/rpc-lua/rpc-lua.h"

#include "server/cli/shell.h"

#include "core/c-wrapper.h"

#include <cjson/cJSON.h>
#include <thread>

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

  main_thread_id = std::this_thread::get_id();

  reloadConfig();

  db = std::unique_ptr<Sqlite3>(new Sqlite3);

  /*
  db = new Sqlite3;
  md5 = calcFileMD5();

  // 启动心跳包线程
  auto heartbeatThread = QThread::create([=]() {
    while (true) {
      for (auto p : this->players.values()) {
        if (p->getState() == Player::Online) {
          p->alive = false;
          p->doNotify("Heartbeat", "");
        }
      }

      for (int i = 0; i < 30; i++) {
        if (!this->isListening) {
          return;
        }
        QThread::sleep(1);
      }

      for (auto p : this->players.values()) {
        if (p->getState() == Player::Online && !p->alive) {
          emit p->kicked();
        }
      }
    }
  });
  heartbeatThread->setParent(this);
  heartbeatThread->setObjectName("Heartbeat");
  heartbeatThread->start();
  */
}

Server::~Server() {
  /*
  isListening = false;
  // 虽然都是子对象 但析构顺序要抠一下
  for (auto p : findChildren<Player *>()) {
    delete p;
  }
  // 得先清理threads及其Rooms 因为其中某些析构函数要调用sql
  for (auto thr : findChildren<RoomThread *>()) {
    delete thr;
  }
  delete db;

  */
  // server_instance = nullptr;
}

void Server::listen(asio::io_context &io_ctx, asio::ip::tcp::endpoint end, asio::ip::udp::endpoint uend) {
  main_io_ctx = &io_ctx;
  m_socket = std::make_unique<ServerSocket>(io_ctx, end, uend);

  m_shell = std::make_unique<Shell>();
  m_shell->start();

  // connect(m_socket, SIGNAL(new_connection), user_manager, SLOT(processNewConnection))
  m_socket->set_new_connection_callback(
    std::bind(&UserManager::processNewConnection, m_user_manager.get(), std::placeholders::_1));

  m_socket->listen();
  m_socket->listen_udp();
}

void Server::stop() {
  main_io_ctx->stop();
}

void Server::destroy() {
  server_instance = nullptr;
}

asio::io_context &Server::context() {
  return *main_io_ctx;
}

UserManager &Server::user_manager() {
  return *m_user_manager;
}

RoomManager &Server::room_manager() {
  return *m_room_manager;
}

Sqlite3 &Server::getDatabase() {
  return *db;
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
  client.send({ buf.c_str(), buf.size() });
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

RoomThread *Server::getThread(int threadId) {
  auto it = m_threads.find(threadId);
  if (it != m_threads.end()) {
    return it->second.get();
  }
  return nullptr;
}

RoomThread &Server::getAvailableThread() {
  for (auto &it : m_threads) {
    // TODO 判满
    auto &thr = it.second;
    return *thr;
  }

  return createThread();
}

std::thread::id Server::mainThreadId() const {
  return main_thread_id;
}

/*

void Server::broadcast(const QByteArray &command, const QByteArray &jsonData) {
  for (Player *p : players.values()) {
    p->doNotify(command, jsonData);
  }
}
*/

void ServerConfig::loadConf(const char* jsonStr) {
  cJSON* root = cJSON_Parse(jsonStr);
  if (!root) {
    spdlog::error("JSON parse error: {}", cJSON_GetErrorPtr());
    return;
  }

  cJSON* item = nullptr;

  if ((item = cJSON_GetObjectItem(root, "banWords")) && cJSON_IsArray(item)) {
    int size = cJSON_GetArraySize(item);
    banWords.clear();
    for (int i = 0; i < size; ++i) {
      cJSON* word = cJSON_GetArrayItem(item, i);
      if (cJSON_IsString(word) && word->valuestring) {
        banWords.push_back(word->valuestring);
      }
    }
  }

  if ((item = cJSON_GetObjectItem(root, "description")) && cJSON_IsString(item) && item->valuestring) {
    description = item->valuestring;
  }

  if ((item = cJSON_GetObjectItem(root, "iconUrl")) && cJSON_IsString(item) && item->valuestring) {
    iconUrl = item->valuestring;
  }

  if ((item = cJSON_GetObjectItem(root, "capacity")) && cJSON_IsNumber(item)) {
    capacity = static_cast<int>(item->valuedouble);
  }

  if ((item = cJSON_GetObjectItem(root, "tempBanTime")) && cJSON_IsNumber(item)) {
    tempBanTime = static_cast<int>(item->valuedouble);
  }

  if ((item = cJSON_GetObjectItem(root, "motd")) && cJSON_IsString(item) && item->valuestring) {
    motd = item->valuestring;
  }

  if ((item = cJSON_GetObjectItem(root, "hiddenPacks")) && cJSON_IsArray(item)) {
    int size = cJSON_GetArraySize(item);
    hiddenPacks.clear();
    for (int i = 0; i < size; ++i) {
      cJSON* pack = cJSON_GetArrayItem(item, i);
      if (cJSON_IsString(pack) && pack->valuestring) {
        hiddenPacks.push_back(pack->valuestring);
      }
    }
  }

  if ((item = cJSON_GetObjectItem(root, "enableBots")) && cJSON_IsBool(item)) {
    enableBots = cJSON_IsTrue(item);
  }

  if ((item = cJSON_GetObjectItem(root, "roomCountPerThread")) && cJSON_IsNumber(item)) {
    roomCountPerThread = static_cast<int>(item->valuedouble);
  }

  if ((item = cJSON_GetObjectItem(root, "maxPlayersPerDevice")) && cJSON_IsNumber(item)) {
    maxPlayersPerDevice = static_cast<int>(item->valuedouble);
  }

  cJSON_Delete(root);
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
  // auto arr = getConfig("banwords").toArray();
  // if (arr.isEmpty()) {
  //   return true;
  // }
  // for (auto v : arr) {
  //   auto s = v.toString().toUpper();
  //   if (str.toUpper().indexOf(s) != -1) {
  //     return false;
  //   }
  // }
  return true;
}

/*
void Server::temporarilyBan(int playerId) {
  auto player = findPlayer(playerId);
  if (!player) return;

  auto socket = player->getSocket();
  QString addr;
  if (!socket) {
    QString sql_find = QString("SELECT * FROM userinfo \
        WHERE id=%1;").arg(playerId);
    auto result = db->select(sql_find);
    if (result.isEmpty())
      return;

    auto obj = result[0];
    addr = obj["lastLoginIp"];
  } else {
    addr = socket->peerAddress();
  }
  temp_banlist.append(addr);

  auto time = getConfig("tempBanTime").toInt();
  QTimer::singleShot(time * 60000, this, [=]() {
      temp_banlist.removeOne(addr);
      });
  emit player->kicked();
}

void Server::beginTransaction() {
  transaction_mutex.lock();
  db->exec("BEGIN;");
}

void Server::endTransaction() {
  db->exec("COMMIT;");
  transaction_mutex.unlock();
}

const QString &Server::getMd5() const {
  return md5;
}

void Server::refreshMd5() {
  md5 = calcFileMD5();
  for (auto room : rooms) {
    if (room->isOutdated()) {
      if (!room->isStarted()) {
        for (auto p : room->getPlayers()) {
          p->doNotify("ErrorMsg", "room is outdated");
          p->kicked();
        }
      } else {
        room->doBroadcastNotify(room->getPlayers(), "GameLog", QCborMap {
          { "type", "#RoomOutdated" },
          { "toast", true },
        }.toCborValue().toCbor());
      }
    }
  }
  for (auto thread : findChildren<RoomThread *>()) {
    if (thread->isOutdated() && thread->findChildren<Room *>().isEmpty())
      thread->deleteLater();
  }
  for (auto p : lobby()->getPlayers()) {
    emit p->kicked();
  }
}

qint64 Server::getUptime() const {
  if (!uptime_counter.isValid()) return 0;
  return uptime_counter.elapsed();
}

bool Server::nameIsInWhiteList(const QString &name) const {
  if (!hasWhitelist) return true;
  return whitelist.length() > 0 && whitelist.contains(name);
}
*/
