// SPDX-License-Identifier: GPL-3.0-or-later

#include "server/user/player.h"
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

Player::Player() {
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

Player::~Player() {
  // spdlog::debug("[MEMORY] Player {} (connId={} state={}) destructed", id, connId, getStateString());
  auto room = getRoom().lock();
  if (room) {
    room->removePlayer(*this);
  }

  // 这段现在阶段应该必定由um.deletePlayer触发，就不管了
  // auto &um = Server::instance().user_manager();
  // if (um.findPlayer(getId()) == this)
  //   um.removePlayer(getId());

  // um.removePlayerByConnId(connId);
}

int Player::getId() const { return id; }

void Player::setId(int id) { this->id = id; }

std::string_view Player::getScreenName() const { return screenName; }

void Player::setScreenName(const std::string &name) {
  this->screenName = name;
}

std::string_view Player::getAvatar() const { return avatar; }

void Player::setAvatar(const std::string &avatar) {
  this->avatar = avatar;
}

int Player::getTotalGameTime() const { return totalGameTime; }

void Player::addTotalGameTime(int toAdd) {
  totalGameTime += toAdd;
}

Player::State Player::getState() const { return state; }

std::string_view Player::getStateString() const {
  switch (state) {
  case Online:
    return "online";
  case Trust:
    return "trust";
  case Run:
    return "run";
  case Leave:
    return "leave";
  case Robot:
    return "robot";
  case Offline:
    return "offline";
  default:
    return "invalid";
  }
}


bool Player::isOnline() const {
  return m_router->getSocket() != nullptr;
}

bool Player::insideGame() {
  auto room = dynamic_pointer_cast<Room>(getRoom().lock());
  return room != nullptr && room->isStarted() && !room->hasObserver(*this);
}

void Player::setState(Player::State state) {
  auto old_state = this->state;
  this->state = state;

  if (old_state != state) {
    // QT祖宗之法不可变
    onStateChanged();
  }
}

bool Player::isReady() const { return ready; }

void Player::setReady(bool ready) {
  this->ready = ready;
  onReadyChanged();
}

std::vector<int> Player::getGameData() {
  return { totalGames, winCount, runCount };
}

void Player::setGameData(int total, int win, int run) {
  totalGames = total;
  winCount = win;
  runCount = run;
}

std::string_view Player::getLastGameMode() const {
  return lastGameMode;
}

void Player::setLastGameMode(const std::string &mode) {
  lastGameMode = mode;
}

bool Player::isDied() const {
  return died;
}

void Player::setDied(bool died) {
  this->died = died;
}

bool Player::isRunned() const {
  return runned;
}

void Player::setRunned(bool run) {
  runned = run;
}

int Player::getConnId() const { return connId; }

std::weak_ptr<RoomBase> Player::getRoom() const {
  auto &room_manager = Server::instance().room_manager();
  if (roomId == 0) {
    return room_manager.lobby();
  }
  return room_manager.findRoom(roomId);
}

void Player::setRoom(RoomBase &room) {
  roomId = room.getId();
}

Router &Player::router() const {
  return *m_router;
}

// std::string_view Player::getPeerAddress() const {
//   auto p = server->findPlayer(getId());
//   if (!p || p->getState() != Player::Online)
//     return "";
//   return p->getSocket()->peerAddress();
// }

std::string_view Player::getUuid() const {
  return uuid_str;
}

void Player::setUuid(const std::string &uuid) {
  uuid_str = uuid;
}

void Player::doRequest(const std::string_view &command,
                       const std::string_view &jsonData, int timeout, int64_t timestamp) {
  if (getState() != Player::Online)
    return;

  int type = Router::TYPE_REQUEST | Router::SRC_SERVER | Router::DEST_CLIENT;
  m_router->request(type, command, jsonData, timeout, timestamp);
}

std::string Player::waitForReply(int timeout) {
  std::string ret;
  if (getState() != Player::Online) {
    ret = "__cancel";
  } else {
    ret = m_router->waitForReply(timeout);
  }
  return ret;
}

void Player::doNotify(const std::string_view &command, const std::string_view &data) {
  if (!isOnline())
    return;

  // spdlog::debug("[TX](id={} connId={} state={} Room={}): {} {}", id, connId, getStateString(), roomId, command, toHex(data));
  int type =
      Router::TYPE_NOTIFICATION | Router::SRC_SERVER | Router::DEST_CLIENT;

  // 包体至少得传点东西，传个null吧
  m_router->notify(type, command, data == "" ? "\xF6" : data);
}

bool Player::thinking() {
  std::lock_guard<std::mutex> locker { m_thinking_mutex };
  return m_thinking;
}

void Player::setThinking(bool t) {
  std::lock_guard<std::mutex> locker { m_thinking_mutex };
  m_thinking = t;
}

void Player::onNotificationGot(const Packet &packet) {
  if (packet.command == "Heartbeat") {
    ttl = max_ttl;
    return;
  }

  // spdlog::debug("[RX](id={} connId={} state={} Room={}): {} {}", id, connId, getStateString(), roomId, packet.command, toHex(packet.cborData));
  auto room = getRoom().lock();
  if (room) room->handlePacket(*this, packet);
}

void Player::onDisconnected() {
  spdlog::info("Player {} disconnected{}", id,
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

Router &Player::getRouter() { return *m_router; }

void Player::kick() {
  auto weak = weak_from_this();
  if (m_router->getSocket() != nullptr) {
    m_router->getSocket()->disconnectFromHost();
  }

  auto p = weak.lock();
  if (p) p->getRouter().setSocket(nullptr);
}

void Player::emitKicked() {
  auto &main_ctx = Server::instance().context();
  auto f = asio::dispatch(main_ctx, asio::use_future([weak = weak_from_this()] {
    auto c = weak.lock();
    if (c) c->kick();
  }));
  f.wait();
}

void Player::reconnect(std::shared_ptr<ClientSocket> client) {
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
    room->pushRequest(fmt::format("{},reconnect", id));
  } else {
    // 懒得处理掉线玩家在大厅了！踢掉得了
    doNotify("ErrorMsg", "Unknown Error");
    emitKicked();
  }
}

void Player::startGameTimer() {
  gameTime = 0;
  auto now = system_clock::now();
  gameTimerStartTimestamp = duration_cast<seconds>(now.time_since_epoch()).count();
}

void Player::pauseGameTimer() {
  auto now = system_clock::now();
  auto timestamp = duration_cast<seconds>(now.time_since_epoch()).count();
  gameTime += (timestamp - gameTimerStartTimestamp);
}

void Player::resumeGameTimer() {
  auto now = system_clock::now();
  gameTimerStartTimestamp = duration_cast<seconds>(now.time_since_epoch()).count();
}

int Player::getGameTime() {
  auto now = system_clock::now();
  auto timestamp = duration_cast<seconds>(now.time_since_epoch()).count();
  return gameTime + (getState() == Player::Online ? (timestamp - gameTimerStartTimestamp) : 0);
}

void Player::onReplyReady() {
  if (!insideGame()) return;

  auto room = dynamic_pointer_cast<Room>(getRoom().lock());
  if (!room) return;
  auto thread = room->thread().lock();
  if (thread) {
    thread->wakeUp(room->getId(), "reply");
  }
}

void Player::onStateChanged() {
  if (!insideGame()) return;

  auto room = dynamic_pointer_cast<Room>(getRoom().lock());
  if (!room) return;

  auto thread = room->thread().lock();
  if (thread) thread->setPlayerState(connId, id, room->getId());

  room->doBroadcastNotify(room->getPlayers(), "NetStateChanged",
                          Cbor::encodeArray({ id, getStateString() }));

  auto state = getState();
  if (state == Player::Online) {
    resumeGameTimer();
  } else {
    pauseGameTimer();
  }
}

void Player::onReadyChanged() {
  auto room = dynamic_pointer_cast<Room>(getRoom().lock());
  if (!room) return;

  room->doBroadcastNotify(room->getPlayers(), "ReadyChanged",
                          Cbor::encodeArray({ id, ready }));
}

void Player::saveState(std::string_view jsonData, std::function<void()> &&cb) {
  if (id < 0) return;

  auto room_base = getRoom().lock();
  if (!room_base) return;
  auto room = dynamic_pointer_cast<Room>(room_base);
  if (!room) return;
  std::string mode { room->getGameMode() };

  if (!Sqlite3::checkString(mode)) {
    spdlog::error("Invalid mode string for saveState: {}", mode);
    return;
  }

  auto hexData = toHex(jsonData);
  auto &gamedb = Server::instance().gameDatabase();
  auto sql = fmt::format("REPLACE INTO gameSaves (uid, mode, data) VALUES ({},'{}',X'{}')", id, mode, hexData);

  gamedb.async_exec(sql, cb);
}

void Player::getSaveState(std::function<void(std::string)> &&cb) {
  auto room_base = getRoom().lock();
  auto room = dynamic_pointer_cast<Room>(room_base);
  if (!room) {
    return cb("{}");
  }
  std::string mode { room->getGameMode() };

  if (!Sqlite3::checkString(mode)) {
    spdlog::error("Invalid mode string for readSaveState: {}", mode);
    return cb("{}");
  }

  auto sql = fmt::format("SELECT data FROM gameSaves WHERE uid = {} AND mode = '{}'", id, mode);

  Server::instance().gameDatabase().async_select(sql, [cb](Sqlite3::QueryResult result) {
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
}

void Player::saveGlobalState(std::string_view key, std::string_view jsonData, std::function<void()> &&cb) {
  if (id < 0) return;

  if (!Sqlite3::checkString(key)) {
    spdlog::error("Invalid key string for saveGlobalState: {}", std::string(key));
    return;
  }

  auto hexData = toHex(jsonData);
  auto &gamedb = Server::instance().gameDatabase();
  auto sql = fmt::format("REPLACE INTO globalSaves (uid, key, data) VALUES ({},'{}',X'{}')", id, key, hexData);
  
  gamedb.async_exec(sql, cb);
}

void Player::getGlobalSaveState(std::string_view key, std::function<void(std::string)> &&cb) {
  if (!Sqlite3::checkString(key)) {
    spdlog::error("Invalid key string for getGlobalSaveState: {}", std::string(key));
    return cb("{}");
  }

  auto sql = fmt::format("SELECT data FROM globalSaves WHERE uid = {} AND key = '{}'", id, key);

  Server::instance().gameDatabase().async_select(sql, [cb](Sqlite3::QueryResult result) {
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
}
