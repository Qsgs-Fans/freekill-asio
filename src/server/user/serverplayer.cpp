// SPDX-License-Identifier: GPL-3.0-or-later

#include "server/user/serverplayer.h"
#include "core/player.h"
#include "server/user/user_manager.h"
#include "server/server.h"
#include "server/gamelogic/roomthread.h"
#include "server/room/room_manager.h"
#include "server/room/roombase.h"
#include "server/room/room.h"
#include "server/room/lobby.h"
#include "server/io/dbthread.hpp"
#include "network/client_socket.h"
#include "network/router.h"

#include "core/c-wrapper.h"
#include "core/util.h"

namespace asio = boost::asio;
using namespace std::chrono;

static int nextConnId = 1000;

ServerPlayer::ServerPlayer() {
  m_router = std::make_unique<Router>(this, nullptr, Router::TYPE_SERVER);

  m_router->set_notification_got_callback([this](const Packet &p) { onNotificationGot(p); });
  m_router->set_reply_ready_callback([this] { onReplyReady(); });

  roomId = 0;

  connId = nextConnId++;
  if (nextConnId >= 0x7FFFFF00) nextConnId = 1000;

  ttl = max_ttl;
  m_thinking = false;

  gameTime = 0;
  auto now = system_clock::now();
  gameTimerStartTimestamp = duration_cast<seconds>(now.time_since_epoch()).count();
}

ServerPlayer::~ServerPlayer() {
  // spdlog::debug("[MEMORY] ServerPlayer {} (connId={} state={}) destructed", id, connId, getStateString());
  auto room = getRoom().lock();
  if (room) {
    room->removePlayer(*this);
  }
}

std::string ServerPlayer::getScreenName() const { return screenName; }

void ServerPlayer::setScreenName(const std::string &name) {
  this->screenName = name;
}

std::string ServerPlayer::getAvatar() const { return avatar; }

void ServerPlayer::setAvatar(const std::string &avatar) {
  this->avatar = avatar;
}

bool ServerPlayer::isOnline() const {
  return m_router->getSocket() != nullptr;
}

bool ServerPlayer::insideGame() {
  auto room = dynamic_pointer_cast<Room>(getRoom().lock());
  return room != nullptr && room->isStarted() && !room->hasObserver(*this);
}

void ServerPlayer::setState(Player::State state) {
  auto old_state = getState();
  Player::setState(state);

  if (old_state != state) {
    // QT祖宗之法不可变
    onStateChanged();
  }
}

void ServerPlayer::setReady(bool ready) {
  Player::setReady(ready);
  onReadyChanged();
}

bool ServerPlayer::isRunned() const {
  return runned;
}

void ServerPlayer::setRunned(bool run) {
  runned = run;
}

int ServerPlayer::getConnId() const { return connId; }

std::weak_ptr<RoomBase> ServerPlayer::getRoom() const {
  auto &room_manager = Server::instance().room_manager();
  if (roomId == 0) {
    return room_manager.lobby();
  }
  return room_manager.findRoom(roomId);
}

void ServerPlayer::setRoom(RoomBase &room) {
  roomId = room.getId();
}

Router &ServerPlayer::router() const {
  return *m_router;
}

// std::string_view ServerPlayer::getPeerAddress() const {
//   auto p = server->findPlayer(getId());
//   if (!p || p->getState() != ServerPlayer::Online)
//     return "";
//   return p->getSocket()->peerAddress();
// }

std::string_view ServerPlayer::getUuid() const {
  return uuid_str;
}

void ServerPlayer::setUuid(const std::string &uuid) {
  uuid_str = uuid;
}

void ServerPlayer::doRequest(const std::string_view &command,
                       const std::string_view &jsonData, int timeout, int64_t timestamp) {
  if (getState() != ServerPlayer::Online)
    return;

  int type = Router::TYPE_REQUEST | Router::SRC_SERVER | Router::DEST_CLIENT;
  m_router->request(type, command, jsonData, timeout, timestamp);
}

std::string ServerPlayer::waitForReply(int timeout) {
  std::string ret;
  if (getState() != ServerPlayer::Online) {
    ret = "__cancel";
  } else {
    ret = m_router->waitForReply(timeout);
  }
  return ret;
}

void ServerPlayer::doNotify(const std::string_view &command, const std::string_view &data) {
  if (!isOnline())
    return;

  // spdlog::debug("[TX](id={} connId={} state={} Room={}): {} {}", id, connId, getStateString(), roomId, command, toHex(data));
  int type =
      Router::TYPE_NOTIFICATION | Router::SRC_SERVER | Router::DEST_CLIENT;

  // 包体至少得传点东西，传个null吧
  m_router->notify(type, command, data == "" ? "\xF6" : data);
}

bool ServerPlayer::thinking() {
  std::lock_guard<std::mutex> locker { m_thinking_mutex };
  return m_thinking;
}

void ServerPlayer::setThinking(bool t) {
  std::lock_guard<std::mutex> locker { m_thinking_mutex };
  m_thinking = t;
}

void ServerPlayer::onNotificationGot(const Packet &packet) {
  if (packet.command == "Heartbeat") {
    ttl = max_ttl;
    return;
  }

  // spdlog::debug("[RX](id={} connId={} state={} Room={}): {} {}", id, connId, getStateString(), roomId, packet.command, toHex(packet.cborData));
  auto room = getRoom().lock();
  if (room) room->handlePacket(*this, packet);
}

void ServerPlayer::onDisconnected() {
  spdlog::info("ServerPlayer {} disconnected{}", getId(),
               m_router->getSocket() != nullptr ? "" : " (pseudo)");

  m_router->setSocket(nullptr);
  setState(Player::Offline);
  if (insideGame() && !isDied()) {
    setRunned(true);
  }

  auto &server = Server::instance();
  auto &um = server.user_manager();
  if (um.getPlayers().size() <= 10) {
    server.broadcast("ServerMessage", fmt::format("{} logged out", screenName));
  }

  auto room_ = getRoom().lock();

  if (!insideGame()) {
    um.deletePlayer(*this);
  } else if (thinking()) {
    auto room = dynamic_pointer_cast<Room>(room_);
    if (!room) return;
    auto thread = room->thread().lock();
    if (thread) thread->wakeUp(room->getId(), "player_disconnect");
  }
}

Router &ServerPlayer::getRouter() { return *m_router; }

void ServerPlayer::kick() {
  auto weak = weak_from_this();
  if (m_router->getSocket() != nullptr) {
    m_router->getSocket()->disconnectFromHost();
  }

  auto p = weak.lock();
  if (p) p->getRouter().setSocket(nullptr);
}

void ServerPlayer::emitKicked() {
  auto &main_ctx = Server::instance().context();
  if (main_ctx.stopped()) {
    // 此时应当是Server析构中 踢人那一段
    kick();
    return;
  }

  auto f = asio::dispatch(main_ctx, asio::use_future([weak = weak_from_this()] {
    auto c = weak.lock();
    if (c) c->kick();
  }));
  f.wait();
}

void ServerPlayer::reconnect(std::shared_ptr<ClientSocket> client) {
  auto &server = Server::instance();
  if (server.user_manager().getPlayers().size() <= 10) {
    server.broadcast("ServerMessage", fmt::format("{} backed", screenName));
  }

  m_router->setSocket(client);
  setState(Player::Online);
  setRunned(false);
  ttl = max_ttl;

  auto room = dynamic_pointer_cast<Room>(getRoom().lock());
  if (room) {
    Server::instance().user_manager().setupPlayer(*this, true);
    room->pushRequest(fmt::format("{},reconnect", getId()));
  } else {
    // 懒得处理掉线玩家在大厅了！踢掉得了
    doNotify("ErrorMsg", "Unknown Error");
    emitKicked();
  }
}

void ServerPlayer::startGameTimer() {
  gameTime = 0;
  auto now = system_clock::now();
  gameTimerStartTimestamp = duration_cast<seconds>(now.time_since_epoch()).count();
}

void ServerPlayer::pauseGameTimer() {
  auto now = system_clock::now();
  auto timestamp = duration_cast<seconds>(now.time_since_epoch()).count();
  gameTime += (timestamp - gameTimerStartTimestamp);
}

void ServerPlayer::resumeGameTimer() {
  auto now = system_clock::now();
  gameTimerStartTimestamp = duration_cast<seconds>(now.time_since_epoch()).count();
}

int ServerPlayer::getGameTime() {
  auto now = system_clock::now();
  auto timestamp = duration_cast<seconds>(now.time_since_epoch()).count();
  return gameTime + (getState() == ServerPlayer::Online ? (timestamp - gameTimerStartTimestamp) : 0);
}

void ServerPlayer::onReplyReady() {
  if (!insideGame()) return;

  auto room = dynamic_pointer_cast<Room>(getRoom().lock());
  if (!room) return;
  auto thread = room->thread().lock();
  if (thread) {
    thread->wakeUp(room->getId(), "reply");
  }
}

void ServerPlayer::onStateChanged() {
  if (!insideGame()) return;

  auto room = dynamic_pointer_cast<Room>(getRoom().lock());
  if (!room) return;

  auto thread = room->thread().lock();
  if (thread) thread->setPlayerState(connId, getId(), room->getId());

  room->doBroadcastNotify(room->getPlayers(), "NetStateChanged",
                          Cbor::encodeArray({ getId(), getStateString() }));

  auto state = getState();
  if (state == ServerPlayer::Online) {
    resumeGameTimer();
  } else {
    pauseGameTimer();
  }
}

void ServerPlayer::onReadyChanged() {
  auto room = dynamic_pointer_cast<Room>(getRoom().lock());
  if (!room) return;

  room->doBroadcastNotify(room->getPlayers(), "ReadyChanged",
                          Cbor::encodeArray({ getId(), isReady() }));
}

void ServerPlayer::saveState(std::string_view jsonData, std::function<void()> &&cb) {
  do {
    if (getId() < 0) break;

    auto room_base = getRoom().lock();
    if (!room_base) break;
    auto room = dynamic_pointer_cast<Room>(room_base);
    if (!room) break;
    std::string mode { room->getGameMode() };

    if (!Sqlite3::checkString(mode)) {
      spdlog::error("Invalid mode string for saveState: {}", mode);
      break;
    }

    auto hexData = toHex(jsonData);
    auto &gamedb = Server::instance().gameDatabase();
    auto sql = fmt::format("REPLACE INTO gameSaves (uid, mode, data) VALUES ({},'{}',X'{}')", getId(), mode, hexData);

    return gamedb.async_exec(sql, cb);
  } while (false);

  auto &main_ctx = Server::instance().context();
  asio::post(main_ctx, cb);
}

void ServerPlayer::getSaveState(std::function<void(std::string)> &&cb) {
  do {
    auto room_base = getRoom().lock();
    auto room = dynamic_pointer_cast<Room>(room_base);
    if (!room) break;
    std::string mode { room->getGameMode() };

    if (!Sqlite3::checkString(mode)) {
      spdlog::error("Invalid mode string for readSaveState: {}", mode);
      break;
    }

    auto sql = fmt::format("SELECT data FROM gameSaves WHERE uid = {} AND mode = '{}'", getId(), mode);
    auto &db = Server::instance().gameDatabase();

    return db.async_select(sql, [cb](Sqlite3::QueryResult result) {
      if (result.empty() || result[0].count("data") == 0 || result[0]["data"] == "#null") {
        return cb("{}");
      }

      const auto& data = result[0]["data"];
      if (!data.empty() && (data[0] == '{' || data[0] == '[')) {
        return cb(data);
      }

      spdlog::warn("Returned data is not valid JSON: {}", data);
      return cb("{}");
    });
  } while (false);

  auto &main_ctx = Server::instance().context();
  asio::post(main_ctx, [=] { cb("{}"); });
}

void ServerPlayer::saveGlobalState(std::string_view key, std::string_view jsonData, std::function<void()> &&cb) {
  do {
    if (getId() < 0) break;

    if (!Sqlite3::checkString(key)) {
      spdlog::error("Invalid key string for saveGlobalState: {}", std::string(key));
      break;
    }

    auto hexData = toHex(jsonData);
    auto &gamedb = Server::instance().gameDatabase();
    auto sql = fmt::format("REPLACE INTO globalSaves (uid, key, data) VALUES ({},'{}',X'{}')", getId(), key, hexData);

    return gamedb.async_exec(sql, cb);
  } while (false);

  auto &main_ctx = Server::instance().context();
  asio::post(main_ctx, cb);
}

void ServerPlayer::getGlobalSaveState(std::string_view key, std::function<void(std::string)> &&cb) {
  do {
    if (!Sqlite3::checkString(key)) {
      spdlog::error("Invalid key string for getGlobalSaveState: {}", std::string(key));
      break;
    }

    auto sql = fmt::format("SELECT data FROM globalSaves WHERE uid = {} AND key = '{}'", getId(), key);

    auto &db = Server::instance().gameDatabase();
    return db.async_select(sql, [cb](Sqlite3::QueryResult result) {
      if (result.empty() || result[0].count("data") == 0 || result[0]["data"] == "#null") {
        return cb("{}");
      }

      const auto& data = result[0]["data"];
      if (!data.empty() && (data[0] == '{' || data[0] == '[')) {
        return cb(data);
      }

      spdlog::warn("Returned data is not valid JSON: {}", data);
      return cb("{}");
    });
  } while (false);

  auto &main_ctx = Server::instance().context();
  asio::post(main_ctx, [=] { cb("{}"); });
}
