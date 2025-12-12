// SPDX-License-Identifier: GPL-3.0-or-later

#include "server/room/room.h"
#include "server/room/lobby.h"
#include "server/room/room_manager.h"
#include "server/gamelogic/roomthread.h"
#include "network/client_socket.h"
#include "network/router.h"
#include "server/server.h"
#include "server/user/serverplayer.h"
#include "server/user/user_manager.h"
#include "core/c-wrapper.h"

namespace asio = boost::asio;

using std::ranges::find;

Room::Room() {
  static int nextRoomId = 1;
  id = nextRoomId++;

  m_thread_id = 1000;

  timeout = 15;
}

Room::~Room() {
  // 标记为过期 避免封人
  md5 = "";

  auto &um = Server::instance().user_manager();
  auto &rm = Server::instance().room_manager();

  auto pClone = players;
  auto obClone = observers;
  for (auto pConnId : pClone) {
    auto p = um.findPlayerByConnId(pConnId).lock();
    if (!p) continue;

    if (p->isOnline()) {
      removePlayer(*p);
      rm.lobby().lock()->addPlayer(*p);
    } else {
      um.deletePlayer(*p);
    }
  }
  for (auto pConnId : obClone) {
    auto p = um.findPlayerByConnId(pConnId).lock();
    if (p) {
      if (p->isOnline()) {
        removeObserver(*p);
        rm.lobby().lock()->addPlayer(*p);
      } else {
        um.deletePlayer(*p);
      }
    }
  }

  auto thr = Server::instance().getThread(m_thread_id).lock();
  if (thr) {
    thr->removeRoom(id);
    thr->decreaseRefCount();
  }

  rm.lobby().lock()->updateOnlineInfo();

  // spdlog::debug("[MEMORY] Room {} destructed", id);
}

std::string &Room::getName() { return name; }

void Room::setName(const std::string_view &name) { this->name = name; }

size_t Room::getCapacity() const { return capacity; }

void Room::setCapacity(size_t capacity) { this->capacity = capacity; }

bool Room::isFull() const { return players.size() == capacity; }

const std::vector<int> &Room::getPlayers() const { return players; }
const std::vector<int> &Room::getObservers() const { return observers; }

const std::string_view Room::getSettings() const { return settings; }
const std::string_view Room::getGameMode() const { return gameMode; }
const std::string_view Room::getPassword() const { return password; }

void Room::setSettings(const std::string_view &settings) {
  // settings要失效了 先清空两个view
  gameMode = "";
  password = "";
  this->settings = settings;

  auto cbuf = (cbor_data)this->settings.data();
  auto len = this->settings.size();

  // 由于settings的内容实在无法预料到后面会变得多么复杂 这里直接分配内存了
  // 不抠那几百字节了

  struct cbor_load_result result;
  auto settings_map = cbor_load(cbuf, len, &result);
  if (result.error.code != CBOR_ERR_NONE || !cbor_isa_map(settings_map)) {
    cbor_decref(&settings_map);
    return;
  }

  auto sz = cbor_map_size(settings_map);
  auto pairs = cbor_map_handle(settings_map);
  int iter = 0;
  for (size_t i = 0; i < sz; i++) {
    auto pair = pairs[i];
    auto k = pair.key, v = pair.value;

    if (cbor_isa_string(k) && strncmp((const char *)cbor_string_handle(k), "gameMode", 8) == 0) {
      if (!cbor_isa_string(v)) continue;
      gameMode = std::string { (const char *)cbor_string_handle(v), cbor_string_length(v) };
      iter++;
    }

    if (cbor_isa_string(k) && strncmp((const char *)cbor_string_handle(k), "password", 8) == 0) {
      if (!cbor_isa_string(v)) continue;
      password = std::string { (const char *)cbor_string_handle(v), cbor_string_length(v) };
      iter++;
    }

    if (iter >= 2) break;
  }

  cbor_decref(&settings_map);
}

bool Room::isAbandoned() const {
  if (players.empty())
    return true;

  auto &um = Server::instance().user_manager();
  for (auto connId : players) {
    auto p = um.findPlayerByConnId(connId).lock();
    if (p && p->getRouter().getSocket() != nullptr)
      return false;
  }
  return true;
}

std::weak_ptr<ServerPlayer> Room::getOwner() const {
  return Server::instance().user_manager().findPlayerByConnId(m_owner_conn_id);
}

void Room::setOwner(ServerPlayer &owner) {
  // BOT不能当房主！
  if (owner.getId() < 0) return;
  m_owner_conn_id = owner.getConnId();
  doBroadcastNotify(players, "RoomOwner", Cbor::encodeArray( { owner.getId() } ));
}

void Room::addPlayer(ServerPlayer &player) {
  auto pid = player.getId();
  if (isRejected(player)) {
    player.doNotify("ErrorMsg", "rejected your demand of joining room");
    return;
  }

  // 如果要加入的房间满员了，或者已经开战了，就不能再加入
  if (isFull() || isStarted()) {
    player.doNotify("ErrorMsg", "Room is full or already started!");
    return;
  }

  auto mode = gameMode;

  // 告诉房里所有玩家有新人进来了
  doBroadcastNotify(players, "AddPlayer", Cbor::encodeArray({
    pid,
    player.getScreenName().data(),
    player.getAvatar().data(),
    player.isReady(),
    player.getTotalGameTime(),
  }));

  players.push_back(player.getConnId());
  player.setRoom(*this);
  // spdlog::debug("[ROOM_ADDPLAYER] ServerPlayer {} (connId={}, state={}) added to room {}", player.getId(), player.getConnId(), player.getStateString(), id);

  // 这集不用信号；这个信号是把玩家从大厅删除的
  // if (pid > 0)
  //   emit playerAdded(player);

  // Second, let the player enter room and add other players
  auto buf = Cbor::encodeArray({ capacity, timeout });
  buf.data()[0] += 1; // array header 2 -> 3
  player.doNotify("EnterRoom", buf + settings);

  auto &um = Server::instance().user_manager();
  for (auto connId : players) {
    if (connId == player.getConnId()) continue;
    auto p = um.findPlayerByConnId(connId).lock();
    if (!p) continue; // FIXME: 应当是出大问题了
    player.doNotify("AddPlayer", Cbor::encodeArray({
      p->getId(),
      p->getScreenName(),
      p->getAvatar(),
      p->isReady(),
      p->getTotalGameTime(),
    }));

    player.doNotify("UpdateGameData", Cbor::encodeArray({
      p->getId(),
      // TODO 把傻逼gameData数组拿下
      p->getGameData()[0],
      p->getGameData()[1],
      p->getGameData()[2],
    }));
  }

  if (m_owner_conn_id == 0) {
    setOwner(player);
  }
  auto owner = um.findPlayerByConnId(m_owner_conn_id).lock();
  if (owner)
    player.doNotify("RoomOwner", Cbor::encodeArray({ owner->getId() }));

  if (player.getLastGameMode() != mode) {
    player.setLastGameMode(std::string(mode));
    updatePlayerGameData(pid, mode);
  } else {
    doBroadcastNotify(players, "UpdateGameData", Cbor::encodeArray({
      pid,
      // TODO 把傻逼gameData数组拿下
      player.getGameData()[0],
      player.getGameData()[1],
      player.getGameData()[2],
    }));
  }
}

void Room::addRobot(ServerPlayer &player) {
  if (player.getConnId() != m_owner_conn_id || isFull())
    return;

  auto &um = Server::instance().user_manager();
  auto &robot = um.createRobot();

  addPlayer(robot);
}

void Room::createRunnedPlayer(ServerPlayer &player, std::shared_ptr<ClientSocket> socket) {
  auto &um = Server::instance().user_manager();

  auto runner = std::make_shared<ServerPlayer>();
  runner->setState(Player::Online);
  runner->getRouter().setSocket(socket);
  runner->setScreenName(std::string(player.getScreenName()));
  runner->setAvatar(std::string(player.getAvatar()));
  runner->setId(player.getId());
  auto gamedata = player.getGameData();
  runner->setGameData(gamedata[0], gamedata[1], gamedata[2]);
  runner->addTotalGameTime(player.getTotalGameTime());

  // 最后向服务器玩家列表中增加这个人
  // 原先的跑路机器人会在游戏结束后自动销毁掉
  um.addPlayer(runner);

  Server::instance().room_manager().lobby().lock()->addPlayer(*runner);

  // FIX 控制bug
  u_char buf[10];
  size_t buflen = cbor_encode_uint(runner->getId(), buf, 10);
  runner->doNotify("ChangeSelf", { (char*)buf, buflen });

  // 如果走小道的人不是单机启动玩家 且房没过期 那么直接ban
  if (!isOutdated() && !player.isDied()) {
    Server::instance().temporarilyBan(runner->getId());
  }
}

void Room::removePlayer(ServerPlayer &player) {
  // 如果是旁观者的话，就清旁观者
  if (hasObserver(player)) {
    removeObserver(player);
    return;
  }

  auto it = find(players, player.getConnId());
  if (it == players.end()) return;

  auto &um = Server::instance().user_manager();
  if (!isStarted()) {
    // 游戏还没开始的话，直接删除这名玩家
    player.setReady(false);
    players.erase(it);

    // spdlog::debug("[ROOM_REMOVEPLAYER] ServerPlayer {} (connId={}, state={}) removed from room {}", player.getId(), player.getConnId(), player.getStateString(), id);

    doBroadcastNotify(players, "RemovePlayer", Cbor::encodeArray({ player.getId() }));
  } else {
    // 首先拿到跑路玩家的socket，然后把玩家的状态设为逃跑，这样自动被机器人接管
    auto socket = player.getRouter().getSocket();
    player.setState(Player::Run);
    player.getRouter().setSocket(nullptr);

    if (!player.isDied()) {
      player.setRunned(true);
    }

    // 设完state后把房间叫起来
    if (player.thinking()) {
      auto thread = this->thread().lock();
      if (thread) thread->wakeUp(id, "player_disconnect");
    }

    // 然后基于跑路玩家的socket，创建一个新Player对象用来通信
    createRunnedPlayer(player, socket);
  }

  if (isAbandoned()) {
    m_owner_conn_id = 0;
    checkAbandoned(NoHuman);
  } else if (player.getConnId() == m_owner_conn_id) {
    for (auto pConnId : players) {
      auto new_owner = um.findPlayerByConnId(pConnId).lock();
      if (new_owner && new_owner->isOnline()) {
        setOwner(*new_owner);
        break;
      }
    }
  }
}

void Room::addObserver(ServerPlayer &player) {
  // 首先只能旁观在运行的房间，因为旁观是由Lua处理的
  if (!isStarted()) {
    player.doNotify("ErrorMsg", "Can only observe running room.");
    return;
  }

  if (isRejected(player)) {
    player.doNotify("ErrorMsg", "rejected your demand of joining room");
    return;
  }

  // 向observers中追加player，并从大厅移除player，然后将player的room设为this
  observers.push_back(player.getConnId());
  player.setRoom(*this);

  auto thread = this->thread().lock();
  thread->addObserver(player.getConnId(), id);
  pushRequest(fmt::format("{},observe", player.getId()));
}

void Room::removeObserver(ServerPlayer &player) {
  if (auto it = find(observers, player.getConnId()); it != observers.end()) {
    observers.erase(it);
  }

  if (player.getState() == ServerPlayer::Online) {
    player.doNotify("Setup", Cbor::encodeArray({
      player.getId(),
      player.getScreenName(),
      player.getAvatar(),
    }));
  }

  pushRequest(fmt::format("{},leave", player.getId()));

  auto thread = this->thread().lock();
  if (thread) thread->removeObserver(player.getId(), id);
}

bool Room::hasObserver(ServerPlayer &player) const {
  return find(observers, player.getConnId()) != observers.end();
}

int Room::getTimeout() const { return timeout; }

void Room::setTimeout(int timeout) { this->timeout = timeout; }

void Room::delay(int ms) {
  auto thread = this->thread().lock();
  if (thread) thread->delay(id, ms);
}

bool Room::isOutdated() {
  bool ret = md5 != Server::instance().getMd5();
  if (ret) md5 = "";
  return ret;
}

void Room::setOutdated() {
  md5 = "";
}

bool Room::isStarted() { return getRefCount() > 0; }

std::weak_ptr<RoomThread> Room::thread() const {
  return Server::instance().getThread(m_thread_id);
}

void Room::setThread(RoomThread &t) {
  m_thread_id = t.id();
  md5 = t.getMd5();
  t.addRoom(id);
  t.increaseRefCount();
}

void Room::checkAbandoned(CheckAbandonReason reason) {
  asio::post(Server::instance().context(), [reason, weak = weak_from_this()] {
    auto ptr = weak.lock();
    if (ptr) ptr->_checkAbandoned(reason);
  });
}

void Room::_checkAbandoned(CheckAbandonReason reason) {
  if (reason == NoRefCount) {
    auto &um = Server::instance().user_manager();
    std::vector<int> to_delete;

    for (auto pConnId : players) {
      auto p = um.findPlayerByConnId(pConnId).lock();
      if (!p || !p->isOnline()) {
        to_delete.push_back(pConnId);
      }
    }

    for (auto pConnId : to_delete) {
      auto p = um.findPlayerByConnId(pConnId).lock();
      if (p) um.deletePlayer(*p);
    }

    players.erase(std::remove_if(players.begin(), players.end(), [&](int x) {
      return find(to_delete, x) != to_delete.end();
    }), players.end());
  }

  if (!isAbandoned()) return;
  if (getRefCount() > 0) {
    auto thr = thread().lock();
    if (thr) thr->wakeUp(id, "abandon");
    return;
  }

  auto &rm = Server::instance().room_manager();
  rm.removeRoom(id);
}

static constexpr const char *findPWinRate = "SELECT win, lose, draw "
            "FROM pWinRate WHERE id = {} and mode = '{}' and role = '{}';";

static constexpr const char *updatePWinRate = ("UPDATE pWinRate "
            "SET win = {}, lose = {}, draw = {} "
            "WHERE id = {} and mode = '{}' and role = '{}';");

static constexpr const char *insertPWinRate = ("INSERT INTO pWinRate "
            "(id, mode, role, win, lose, draw) "
            "VALUES ({}, '{}', '{}', {}, {}, {});");

static constexpr const char *findGWinRate = ("SELECT win, lose, draw "
            "FROM gWinRate WHERE general = '{}' and mode = '{}' and role = '{}';");

static constexpr const char *updateGWinRate = ("UPDATE gWinRate "
            "SET win = {}, lose = {}, draw = {} "
            "WHERE general = '{}' and mode = '{}' and role = '{}';");

static constexpr const char *insertGWinRate = ("INSERT INTO gWinRate "
            "(general, mode, role, win, lose, draw) "
            "VALUES ('{}', '{}', '{}', {}, {}, {});");

static constexpr const char *findRunRate = ("SELECT run "
      "FROM runRate WHERE id = {} and mode = '{}';");

static constexpr const char *updateRunRate = ("UPDATE runRate "
            "SET run = {} WHERE id = {} and mode = '{}';");

static constexpr const char *insertRunRate = ("INSERT INTO runRate "
            "(id, mode, run) VALUES ({}, '{}', {});");

void Room::updatePlayerWinRate(int id, const std::string_view &mode, const std::string_view &role, int game_result) {
  if (!Sqlite3::checkString(mode))
    return;
  auto &db = Server::instance().database();

  int win = 0;
  int lose = 0;
  int draw = 0;

  switch (game_result) {
  case 1: win++; break;
  case 2: lose++; break;
  case 3: draw++; break;
  default: break;
  }

  auto result = db.select(fmt::format(findPWinRate, id, mode, role));

  if (result.empty()) {
    db.exec(fmt::format(insertPWinRate, id, mode, role, win, lose, draw));
  } else {
    auto obj = result[0];
    win += atoi(obj["win"].c_str());
    lose += atoi(obj["lose"].c_str());
    draw += atoi(obj["draw"].c_str());
    db.exec(fmt::format(updatePWinRate, win, lose, draw, id, mode, role));
  }

  auto &um = Server::instance().user_manager();
  auto player = um.findPlayer(id).lock();
  if (player && find(players, player->getConnId()) != players.end()) {
    player->setLastGameMode(std::string(mode));
    updatePlayerGameData(id, mode);
  }
}

void Room::updateGeneralWinRate(const std::string_view &general, const std::string_view &mode, const std::string_view &role, int game_result) {
  if (!Sqlite3::checkString(general))
    return;
  if (!Sqlite3::checkString(mode))
    return;
  auto &db = Server::instance().database();

  int win = 0;
  int lose = 0;
  int draw = 0;

  switch (game_result) {
  case 1: win++; break;
  case 2: lose++; break;
  case 3: draw++; break;
  default: break;
  }

  auto result = db.select(fmt::format(findGWinRate, general, mode, role));

  if (result.empty()) {
    db.exec(fmt::format(insertGWinRate, general, mode, role, win, lose, draw));
  } else {
    auto obj = result[0];
    win += atoi(obj["win"].c_str());
    lose += atoi(obj["lose"].c_str());
    draw += atoi(obj["draw"].c_str());
    db.exec(fmt::format(updateGWinRate, win, lose, draw, general, mode, role));
  }
}

void Room::addRunRate(int id, const std::string_view &mode) {
  int run = 1;
  auto &db = Server::instance().database();
  auto result = db.select(fmt::format(findRunRate, id, mode));

  if (result.empty()) {
    db.exec(fmt::format(insertRunRate, id, mode, run));
  } else {
    auto obj = result[0];
    run += atoi(obj["run"].c_str());
    db.exec(fmt::format(updateRunRate, run, id, mode));
  }
}

void Room::updatePlayerGameData(int id, const std::string_view &mode) {
  static constexpr const char *findModeRate =
    "SELECT win, total FROM pWinRateView WHERE id = {} and mode = '{}';";

  if (id < 0) return;

  auto &server = Server::instance();
  auto &um = server.user_manager();
  auto &db = server.database();

  auto player = um.findPlayer(id).lock();
  if (!player) return;

  auto room = dynamic_pointer_cast<Room>(player->getRoom().lock());
  if (player->getState() == ServerPlayer::Robot || !room) {
    return;
  }

  int total = 0;
  int win = 0;
  int run = 0;

  auto result = db.select(fmt::format(findRunRate, id, mode));

  if (!result.empty()) {
    run = atoi(result[0]["run"].c_str());
  }

  result = db.select(fmt::format(findModeRate, id, mode));

  if (!result.empty()) {
    total = atoi(result[0]["total"].c_str());
    win = atoi(result[0]["win"].c_str());
  }

  player->setGameData(total, win, run);
  room->doBroadcastNotify(room->getPlayers(), "UpdateGameData",
                          Cbor::encodeArray({ player->getId(), total, win, run }));
}

// 多线程非常麻烦 把GameOver交给主线程完成去
void Room::gameOver() {
  auto &main_ctx = Server::instance().context();
  auto f = asio::dispatch(main_ctx, asio::use_future([weak = weak_from_this()] {
    auto c = weak.lock();
    if (c) c->_gameOver();
  }));
  f.wait();
}


void Room::updatePlayerGameTime() {
  auto &server = Server::instance();
  auto &um = server.user_manager();

  server.beginTransaction();
  for (auto pConnId : players) {
    auto p = um.findPlayerByConnId(pConnId).lock();
    if (!p) continue;
    auto pid = p->getId();
    if (pid <= 0) continue;

    int time = p->getGameTime();

    auto info_update = fmt::format(
      "UPDATE usergameinfo SET totalGameTime = "
      "IIF(totalGameTime IS NULL, {}, totalGameTime + {}) WHERE id = {};",
      time, time, pid
    );
    server.database().exec(info_update);

    // 然后时间得告诉别人
    auto bytes = Cbor::encodeArray( { pid, time } );
    for (auto connId : players) {
      if (connId == pConnId) continue;
      auto p2 = um.findPlayerByConnId(connId).lock();
      if (p2) p2->doNotify("AddTotalGameTime", bytes);
    }

    // 考虑到阵亡已离开啥的，时间得给真实玩家增加
    auto realPlayer = um.findPlayer(pid).lock();
    if (realPlayer) {
      realPlayer->addTotalGameTime(time);
      realPlayer->doNotify("AddTotalGameTime", bytes);
    }
  }
  server.endTransaction();
}

void Room::_gameOver() {
  updatePlayerGameTime();

  auto &server = Server::instance();
  auto &um = server.user_manager();
  const auto &mode = gameMode;

  for (auto pConnId : players) {
    auto p = um.findPlayerByConnId(pConnId).lock();
    if (!p) continue;
    auto pid = p->getId();
    if (pid <= 0) continue;

    if (p->isRunned()) {
      addRunRate(p->getId(), mode);
    }

    // 游戏结束变回来
    if (p->getState() == ServerPlayer::Trust) {
      p->setState(Player::Online);
    }

    // 踢了并非人类，但是注意下面的两个kick不会释放player
    if (!p->isOnline()) {
      if (p->getState() == ServerPlayer::Offline) {
        if (!isOutdated() && p->isRunned()) {
          server.temporarilyBan(p->getId());
        } else {
          p->emitKicked();
        }
      }
    }
  }
}

void Room::detectSameIpAndDevice() {
  auto &um = Server::instance().user_manager();

  std::unordered_map<std::string_view, std::vector<std::string_view>> uuidList, ipList;
  for (auto pConnId : players) {
    auto p = um.findPlayerByConnId(pConnId).lock();
    if (!p) continue;
    p->setReady(false);
    p->setDied(false);
    p->startGameTimer();

    if (!p->isOnline()) continue;
    auto uuid = p->getUuid();
    auto ip = p->getRouter().getSocket()->peerAddress();
    auto pname = p->getScreenName();
    if (!uuid.empty()) {
      uuidList[uuid].push_back(pname);
    }
    if (!ip.empty()) {
      ipList[ip].push_back(pname);
    }
  }

  static auto join = [](const std::vector<std::string_view>& vec, const std::string_view &spliter) {
    std::string result;
    for (size_t i = 0; i < vec.size(); ++i) {
      if (i != 0) result += spliter;
      result += vec[i];
    }
    return result;
  };

  for (const auto& [ip, names] : ipList) {
    if (names.size() <= 1) continue;
    auto warn = fmt::format("*WARN* Same IP address: [{}]", join(names, ", "));
    doBroadcastNotify(getPlayers(), "ServerMessage", warn);
    spdlog::info(warn);
  }

  for (const auto& [uuid, names] : uuidList) {
    if (names.size() <= 1) continue;
    auto warn = fmt::format("*WARN* Same device id: [{}]", join(names, ", "));
    doBroadcastNotify(getPlayers(), "ServerMessage", warn);
    spdlog::info(warn);
  }
}

void Room::manuallyStart() {
  if (!isFull() || isStarted()) return;

  auto thr = thread().lock();
  if (!thr) return;

  spdlog::info("[GameStart] Room {} started", getId());

  auto &um = Server::instance().user_manager();
  for (auto pConnId : players) {
    auto p = um.findPlayerByConnId(pConnId).lock();
    if (!p) continue;
    p->setReady(false);
    p->setDied(false);
    p->startGameTimer();
  }

  detectSameIpAndDevice();

  thr->pushRequest(fmt::format("-1,{},newroom", id));

  // 立刻加，但又要保证reconnect请求在newroom后面
  increaseRefCount();

  session_id++;
}

void Room::pushRequest(const std::string &req) {
  auto thread = this->thread().lock();
  if (thread) thread->pushRequest(fmt::format("{},{}", id, req));
}

void Room::addRejectId(int id) {
  rejected_players.push_back(id);
}

void Room::removeRejectId(int id) {
  if (auto it = find(rejected_players, id);
    it != rejected_players.end()) {
    rejected_players.erase(it);
  }
}

bool Room::isRejected(ServerPlayer &player) const {
  return find(rejected_players, player.getId()) != rejected_players.end();
}

void Room::setPlayerReady(ServerPlayer &p, bool ready) {
  p.setReady(ready);
  doBroadcastNotify(players, "ReadyChanged", Cbor::encodeArray({ p.getId(), ready }));
}

// ------------------------------------------------
void Room::quitRoom(ServerPlayer &player, const Packet &) {
  removePlayer(player);
  auto &rm = Server::instance().room_manager();
  if (player.getState() == ServerPlayer::Online)
    rm.lobby().lock()->addPlayer(player);

  if (isOutdated()) {
    auto &um = Server::instance().user_manager();
    auto p = um.findPlayer(player.getId()).lock();
    if (p) p->emitKicked();
  }
}

void Room::addRobotRequest(ServerPlayer &player, const Packet &) {
  auto &conf = Server::instance().config();
  if (find(conf.disabledFeatures, "AddRobot") == conf.disabledFeatures.end())
    addRobot(player);
}

void Room::kickPlayer(ServerPlayer &player, const Packet &pkt) {
  auto data = pkt.cborData;
  int i = 0;
  auto result = cbor_stream_decode((cbor_data)data.data(), data.size(), &Cbor::intCallbacks, &i);
  if (result.read == 0 || i == 0) return;

  auto &um = Server::instance().user_manager();
  auto &rm = Server::instance().room_manager();
  auto p = um.findPlayer(i).lock();
  if (!p) return;
  if (isStarted()) return;
  auto room = p->getRoom().lock();
  if (!room || room->getId() != id) return;

  removePlayer(*p);
  rm.lobby().lock()->addPlayer(*p);

  addRejectId(i);

  using namespace std::chrono_literals;
  auto timer = std::make_shared<asio::steady_timer>(Server::instance().context(), 3min);
  timer->async_wait([weak = weak_from_this(), i, timer](const std::error_code &ec) {
    if (!ec) {
      auto ptr = weak.lock();
      if (ptr) ptr->removeRejectId(i);
    } else {
      spdlog::error(ec.message());
    }
  });
}

void Room::trust(ServerPlayer &player, const Packet &pkt) {
  // 仅在对局中允许托管
  if (!isStarted()) return;

  // 将玩家置为托管
  if (player.getState() != ServerPlayer::Trust) {
    player.setState(Player::Trust);
    if (player.thinking()) {
      auto thread = this->thread().lock();
      if (thread) thread->wakeUp(id, "player_trust");
    }
  } else {
    player.setState(Player::Online);
  }
}

// 改变房间配置
void Room::changeRoom(ServerPlayer &player, const Packet &packet) {
  // 检查权限：只有房主才能修改房间配置
  if (player.getConnId() != m_owner_conn_id) {
    player.doNotify("ErrorMsg", "只有房主才能修改房间配置");
    return;
  }
  auto currentplayers = getPlayers();
  auto cbuf = (cbor_data)packet.cborData.data();
  auto len = packet.cborData.size();
  std::string_view newname;
  int newcapacity = -1;
  int newtimeout = -1;
  std::string_view newsettings;
  size_t sz = 0;
  struct cbor_decoder_result decode_result;

  decode_result = cbor_stream_decode(cbuf, len, &Cbor::arrayCallbacks, &sz); // arr
  if (decode_result.read == 0) return;
  cbuf += decode_result.read; len -= decode_result.read;
  if (sz != 4) return;

  decode_result = cbor_stream_decode(cbuf, len, &Cbor::stringCallbacks, &newname);
  if (decode_result.read == 0) return;
  cbuf += decode_result.read; len -= decode_result.read;
  if (newname == "") return;

  decode_result = cbor_stream_decode(cbuf, len, &Cbor::intCallbacks, &newcapacity);
  if (decode_result.read == 0) return;
  cbuf += decode_result.read; len -= decode_result.read;
  if (newcapacity == -1) return;

  decode_result = cbor_stream_decode(cbuf, len, &Cbor::intCallbacks, &newtimeout);
  if (decode_result.read == 0) return;
  cbuf += decode_result.read; len -= decode_result.read;
  if (newtimeout == -1) return;

  newsettings = { (char *)cbuf, len };
  // 房间人数大于新容量，不允许
  if (newcapacity < int(players.size())) {
    player.doNotify("ErrorMsg", "新容量不得低于现有玩家数！");
    return;
  }

  setName(newname);
  setCapacity(newcapacity);
  setTimeout(newtimeout);
  setSettings(newsettings);

  doBroadcastNotify(players, "ChangeRoom", packet.cborData);

  // 按官服 修改配置后所有人重新准备
  auto &um = Server::instance().user_manager();
  for (auto pid : players) {
    auto p = um.findPlayerByConnId(pid).lock();
    if (!p) continue;
    p->setReady(false);
  }
}

void Room::ready(ServerPlayer &player, const Packet &) {
  setPlayerReady(player, !player.isReady());
}

void Room::startGame(ServerPlayer &player, const Packet &) {
  if (isOutdated()) {
    auto &um = Server::instance().user_manager();
    for (auto pid : getPlayers()) {
      auto p = um.findPlayerByConnId(pid).lock();
      if (!p) continue;
      p->doNotify("ErrorMsg", "room is outdated");
      p->emitKicked();
    }
  } else {
    manuallyStart();
  }
}

typedef void (Room::*room_cb)(ServerPlayer &, const Packet &);

void Room::handlePacket(ServerPlayer &sender, const Packet &packet) {
  static const std::unordered_map<std::string_view, room_cb> room_actions = {
    {"QuitRoom", &Room::quitRoom},
    {"AddRobot", &Room::addRobotRequest},
    {"KickPlayer", &Room::kickPlayer},
    {"Ready", &Room::ready},
    {"StartGame", &Room::startGame},
    {"Trust", &Room::trust},
    {"ChangeRoom", &Room::changeRoom},
    {"Chat", &Room::chat},
    {"ReadGlobalSaveState", &Room::readGlobalSaveState},
  };
  if (packet.command == "PushRequest") {
    std::string_view sv;
    auto ret = cbor_stream_decode(
      (cbor_data)packet.cborData.data(), packet.cborData.size(),
      &Cbor::stringCallbacks, &sv
    );
    if (ret.read == 0) return;
    pushRequest(fmt::format("{},{}", sender.getId(), sv));
    return;
  }

  auto iter = room_actions.find(packet.command);
  if (iter != room_actions.end()) {
    auto func = iter->second;
    (this->*func)(sender, packet);
  }
}

// Lua用：request之前设置计时器防止等到死。
void Room::setRequestTimer(int ms) {
  auto thread = this->thread().lock();
  if (!thread) return;
  auto &ctx = thread->context();

  request_timer = std::make_unique<asio::steady_timer>(
    ctx, std::chrono::milliseconds(ms));

  // 不能让即将运行在thread中的lambda捕获到shared_ptr，否则可能会线程内析构自身导致死锁
  auto weak_thr = std::weak_ptr(thread);
  request_timer->async_wait([this, weak_thr](const std::error_code& ec){
    if (!ec) {
      auto thread = weak_thr.lock();
      if (thread) thread->wakeUp(id, "request_timer");
    } else {
      // 我们本来就会调cancel()并销毁requestTimer，所以aborted的情况很多很多
      if (ec.value() != asio::error::operation_aborted) {
        spdlog::error("error in request timer of room {}: {}", id, ec.message());
      }
    }
  });
}

// Lua用：当request完成后手动销毁计时器。
void Room::destroyRequestTimer() {
  if (!request_timer) return;
  request_timer->cancel();
  request_timer = nullptr;
}

int Room::getRefCount() {
  std::lock_guard<std::mutex> locker(lua_ref_mutex);
  return lua_ref_count;
}

void Room::increaseRefCount() {
  std::lock_guard<std::mutex> locker(lua_ref_mutex);
  lua_ref_count++;
}

void Room::decreaseRefCount() {
  {
    std::lock_guard<std::mutex> locker(lua_ref_mutex);
    lua_ref_count--;
    if (lua_ref_count > 0) return;
  }
  checkAbandoned(NoRefCount);
}

int Room::getSessionId() const {
  return session_id;
}

std::string_view Room::getSessionData() const {
  return session_data;
}

void Room::setSessionData(std::string_view json) {
  session_data = json;
}

ServerPlayer &Room::addNpc() {
  auto &um = Server::instance().user_manager();
  auto &robot = um.createRobot();

  players.push_back(robot.getConnId());
  robot.setRoom(*this);

  return robot;
}

void Room::removeNpc(ServerPlayer &player) {
  auto &um = Server::instance().user_manager();

  auto it = find(players, player.getConnId());
  if (it == players.end()) return;

  players.erase(it);
  um.deletePlayer(player);
}
