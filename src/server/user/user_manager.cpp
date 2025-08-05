// SPDX-License-Identifier: GPL-3.0-or-later

#include "server/user/user_manager.h"
#include "server/user/player.h"
#include "server/user/auth.h"
#include "server/server.h"
#include "server/room/room_manager.h"
#include "server/room/lobby.h"
#include "network/client_socket.h"
#include "network/router.h"
#include "core/c-wrapper.h"

UserManager::UserManager() {
  m_auth = std::make_unique<AuthManager>();
}

std::weak_ptr<Player> UserManager::findPlayer(int id) const {
  if (id < 0) return findRobot(id);
  try {
    return online_players_map.at(id);
  } catch (const std::out_of_range &) {
    return {};
  }
}

std::weak_ptr<Player> UserManager::findRobot(int id) const {
  try {
    return robots_map.at(id);
  } catch (const std::out_of_range &) {
    return {};
  }
}

std::weak_ptr<Player> UserManager::findPlayerByConnId(int connId) const {
  try {
    return players_map.at(connId);
  } catch (const std::out_of_range &) {
    return {};
  }
}

void UserManager::addPlayer(std::shared_ptr<Player> player) {
  int id = player->getId();
  if (id > 0) {
    if (online_players_map[id])
      online_players_map.erase(id);

    online_players_map[id] = player;
  } else {
    if (robots_map[id]) [[unlikely]]
      robots_map.erase(id);

    robots_map[id] = player;
  }

  players_map[player->getConnId()] = player;
}

void UserManager::deletePlayer(Player &p) {
  removePlayer(p, p.getId());
  removePlayerByConnId(p.getConnId());
}

void UserManager::removePlayer(Player &p, int id) {
  if (online_players_map.find(id) != online_players_map.end() &&
    online_players_map[id].get() == &p) {
    online_players_map.erase(id);
  }
  if (robots_map.find(id) != robots_map.end()) {
    robots_map.erase(id);
  }
}

void UserManager::removePlayerByConnId(int connId) {
  if (players_map.find(connId) != players_map.end()) {
    players_map.erase(connId);
  }
}


const std::unordered_map<int, std::shared_ptr<Player>> &UserManager::getPlayers() const {
  return online_players_map;
}

void UserManager::processNewConnection(std::shared_ptr<ClientSocket> client) {
  auto addr = client->peerAddress();
  spdlog::info("client {} connected", addr);

  auto &server = Server::instance();
  auto &db = server.database();

  // check ban ip
  auto result = db.select(fmt::format("SELECT * FROM banip WHERE ip='{}';", addr));

  const char *errmsg = nullptr;

  if (!result.empty()) {
    errmsg = "you have been banned!";
  } else if (server.isTempBanned(addr)) {
    errmsg = "you have been temporarily banned!";
  } else if (online_players_map.size() >= server.config().capacity) {
    errmsg = "server is full!";
  }

  if (errmsg) {
    server.sendEarlyPacket(*client, "ErrorDlg", errmsg);
    spdlog::info("Refused banned IP: {}", addr);
    client->disconnectFromHost();
    return;
  }

  // network delay test
  server.sendEarlyPacket(*client, "NetworkDelayTest", m_auth->getPublicKeyCbor());
  // Note: the client should send a setup string next
  client->set_message_got_callback(
    std::bind(&AuthManager::processNewConnection, m_auth.get(), client, std::placeholders::_1));
  // client->timerSignup.start(30000);
}

void UserManager::createNewPlayer(std::shared_ptr<ClientSocket> client, std::string_view name, std::string_view avatar, int id, std::string_view uuid_str) {
  // create new Player and setup
  auto player = std::make_shared<Player>();
  player->router().setSocket(client);
  player->setState(Player::Online);
  player->setScreenName(std::string(name));
  player->setAvatar(std::string(avatar));
  player->setId(id);
  player->setUuid(std::string(uuid_str));

  auto &server = Server::instance();
  if (online_players_map.size() <= 10) {
    server.broadcast("ServerMessage", fmt::format("{} logged in", player->getScreenName()));
  }

  addPlayer(player);

  setupPlayer(*player);

  auto result = server.database().select(fmt::format(
    "SELECT totalGameTime FROM usergameinfo WHERE id={};", id
  ));
  auto time = atoi(result[0]["totalGameTime"].c_str());
  player->addTotalGameTime(time);
  player->doNotify("AddTotalGameTime", Cbor::encodeArray({ id, time }));

  auto lobby = Server::instance().room_manager().lobby().lock();
  if (lobby) lobby->addPlayer(*player);
}

Player &UserManager::createRobot() {
  static int nextRobotId = -2;

  auto robot = std::make_shared<Player>();
  robot->setState(Player::Robot);
  robot->setId(nextRobotId--);
  if (nextRobotId < (int)0x800000FF) nextRobotId = -2;
  robot->setAvatar("guanyu");
  robot->setScreenName(fmt::format("COMP-{}", robot->getId()));
  robot->setReady(true);

  addPlayer(robot);
  return *robot;
}

void UserManager::setupPlayer(Player &player, bool all_info) {
  // tell the lobby player's basic property
  player.doNotify("Setup", Cbor::encodeArray({
    player.getId(),
    player.getScreenName().data(),
    player.getAvatar().data(),
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count(),
  }));

  if (all_info) {
    auto &conf = Server::instance().config();

    // 经典环节
    std::string toSend;
    toSend.reserve(1024);
    u_char buf[10]; size_t buflen;

    toSend += "\x83"; // array(3)

    // arr[0] = motd
    buflen = cbor_encode_uint(conf.motd.size(), buf, 10);
    buf[0] += 0x60;
    toSend += std::string_view { (char*)buf, buflen };
    toSend += conf.motd;

    // arr[1] = hiddenPacks
    buflen = cbor_encode_uint(conf.hiddenPacks.size(), buf, 10);
    buf[0] += 0x80;
    toSend += std::string_view { (char*)buf, buflen };
    for (auto &s : conf.hiddenPacks) {
      buflen = cbor_encode_uint(s.size(), buf, 10);
      buf[0] += 0x60;
      toSend += std::string_view { (char*)buf, buflen };
      toSend += s;
    }

    // arr[2] = enableBots
    buflen = cbor_encode_bool(conf.enableBots, buf, 10);
    toSend += std::string_view { (char*)buf, buflen };

    player.doNotify("SetServerSettings", toSend);
  }
}

