// SPDX-License-Identifier: GPL-3.0-or-later

#include "server/room/lobby.h"
#include "server/server.h"
#include "server/user/serverplayer.h"
#include "server/user/user_manager.h"
#include "server/room/room_manager.h"
#include "server/room/room.h"
#include "network/client_socket.h"
#include "server/task/task_manager.h"
#include "server/task/task.h"

#include "core/c-wrapper.h"
#include "core/util.h"

#include <openssl/sha.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

Lobby::Lobby() {
  id = 0;
}

auto Lobby::getPlayers() const -> const decltype(players) & {
  return players;
}

void Lobby::addPlayer(ServerPlayer &player) {
  auto &um = Server::instance().user_manager();
  if (player.getState() == Player::Robot) {
    um.deletePlayer(player);
  } else {
    players[player.getConnId()] = true;
    player.setRoom(*this);
    player.doNotify("EnterLobby", "");
    // spdlog::debug("[LOBBY_ADDPLAYER] Player {} (connId={}, state={})", player.getId(), player.getConnId(), player.getStateString());
  }

  updateOnlineInfo();
}

void Lobby::removePlayer(ServerPlayer &player) {
  auto connId = player.getConnId();
  // spdlog::debug("[LOBBY_REMOVEPLAYER] Player {} (connId={}, state={})", player.getId(), player.getConnId(), player.getStateString());
  players.erase(connId);
  updateOnlineInfo();
}

void Lobby::updateOnlineInfo() {
  auto &um = Server::instance().user_manager();
  auto arr = Cbor::encodeArray({
    players.size(),
    um.getPlayers().size(),
  });
  for (auto &[pid, _] : players) {
    auto p = um.findPlayerByConnId(pid).lock();
    if (p) p->doNotify("UpdatePlayerNum", arr);
  }
}

// Lobby不可能abandon 这个主要是检查lobby的死人并清除
// 然后命名这一块顺应Room同款函数
void Lobby::checkAbandoned() {
  auto &um = Server::instance().user_manager();
  std::vector<int> to_delete;

  for (auto &[pConnId, _] : players) {
    auto p = um.findPlayerByConnId(pConnId).lock();
    if (!p || !p->isOnline()) {
      to_delete.push_back(pConnId);
    }
  }

  for (auto pConnId : to_delete) {
    auto p = um.findPlayerByConnId(pConnId).lock();
    if (p) um.deletePlayer(*p);

    players.erase(pConnId);
  }
}

void Lobby::updateAvatar(ServerPlayer &sender, const Packet &packet) {
  auto cbuf = (cbor_data)packet.cborData.data();
  auto len = packet.cborData.size();

  std::string_view avatar;

  struct cbor_decoder_result decode_result;

  decode_result = cbor_stream_decode(cbuf, len, &Cbor::stringCallbacks, &avatar);
  if (decode_result.read == 0) return;
  cbuf += decode_result.read; len -= decode_result.read;
  if (avatar == "") return;

  if (!Sqlite3::checkString(avatar)) return;
  Server::instance().database().exec(
    fmt::format(
      "UPDATE userinfo SET avatar='{}' WHERE id={};",
      avatar,
      sender.getId()
    )
  );

  sender.setAvatar(std::string(avatar));
  sender.doNotify("UpdateAvatar", avatar);
}

void Lobby::updatePassword(ServerPlayer &sender, const Packet &packet) {
  auto cbuf = (cbor_data)packet.cborData.data();
  auto len = packet.cborData.size();

  std::string_view oldpw;
  std::string_view newpw;
  size_t sz = 0;

  struct cbor_decoder_result decode_result;

  decode_result = cbor_stream_decode(cbuf, len, &Cbor::arrayCallbacks, &sz); // arr
  if (decode_result.read == 0) return;
  cbuf += decode_result.read; len -= decode_result.read;
  if (sz != 2) return;

  decode_result = cbor_stream_decode(cbuf, len, &Cbor::stringCallbacks, &oldpw);
  if (decode_result.read == 0) return;
  cbuf += decode_result.read; len -= decode_result.read;
  if (oldpw == "") return;

  decode_result = cbor_stream_decode(cbuf, len, &Cbor::stringCallbacks, &newpw);
  if (decode_result.read == 0) return;
  cbuf += decode_result.read; len -= decode_result.read;
  if (newpw == "") return;


  auto passed = false;
  auto &db = Server::instance().database();
  auto arr2 = db.select(fmt::format(
    "SELECT password, salt FROM userinfo WHERE id={};", sender.getId()));
  auto result = arr2[0];

  auto pw = std::string(oldpw) + result["salt"];
  unsigned char hash[SHA256_DIGEST_LENGTH];
  SHA256((const u_char *)pw.data(), pw.size(), hash);

  passed = (result["password"] == toHex(std::string_view { (char*)hash, SHA256_DIGEST_LENGTH }));
  if (passed) {
    auto pw2 = std::string(newpw) + result["salt"];
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256((const u_char *)pw2.data(), pw2.size(), hash);

    auto sql = fmt::format(
      "UPDATE userinfo SET password='{}' WHERE id={};",
      toHex(std::string_view { (char*)hash, SHA256_DIGEST_LENGTH }),
      sender.getId()
    );

    db.exec(sql);
  }

  sender.doNotify("UpdatePassword", passed ? "1" : "0");
}

void Lobby::createRoom(ServerPlayer &sender, const Packet &packet) {
  auto cbuf = (cbor_data)packet.cborData.data();
  auto len = packet.cborData.size();

  std::string_view name;
  int capacity = -1;
  int timeout = -1;
  std::string_view settings;
  size_t sz = 0;

  struct cbor_decoder_result decode_result;

  decode_result = cbor_stream_decode(cbuf, len, &Cbor::arrayCallbacks, &sz); // arr
  if (decode_result.read == 0) return;
  cbuf += decode_result.read; len -= decode_result.read;
  if (sz != 4) return;

  decode_result = cbor_stream_decode(cbuf, len, &Cbor::stringCallbacks, &name);
  if (decode_result.read == 0) return;
  cbuf += decode_result.read; len -= decode_result.read;
  if (name == "") return;

  decode_result = cbor_stream_decode(cbuf, len, &Cbor::intCallbacks, &capacity);
  if (decode_result.read == 0) return;
  cbuf += decode_result.read; len -= decode_result.read;
  if (capacity == -1) return;

  decode_result = cbor_stream_decode(cbuf, len, &Cbor::intCallbacks, &timeout);
  if (decode_result.read == 0) return;
  cbuf += decode_result.read; len -= decode_result.read;
  if (timeout == -1) return;

  settings = { (char *)cbuf, len };

  auto &rm = Server::instance().room_manager();
  auto room_ptr = rm.createRoom(sender, std::string(name), capacity, timeout, std::string(settings));
  if (room_ptr) {
    room_ptr->addPlayer(sender);
    auto room = sender.getRoom().lock();
    if (room && room->getId() == room_ptr->getId()) {
      removePlayer(sender);
    }
  }
}

void Lobby::joinRoom(ServerPlayer &sender, const Packet &pkt, bool observe) {
  auto &rm = Server::instance().room_manager();

  auto data = pkt.cborData;
  auto cbuf = (cbor_data)data.data();
  auto len = data.size();
  size_t sz;
  int roomId = 0;
  std::string_view pw;

  cbor_decoder_result result;
  result = cbor_stream_decode(cbuf, len, &Cbor::arrayCallbacks, &sz);
  if (result.read == 0) return;
  cbuf += result.read; len -= result.read;
  if (sz != 2) return;

  result = cbor_stream_decode(cbuf, len, &Cbor::intCallbacks, &roomId);
  if (result.read == 0) return;
  cbuf += result.read; len -= result.read;
  if (roomId == 0) return;

  result = cbor_stream_decode(cbuf, len, &Cbor::stringCallbacks, &pw);
  if (result.read == 0) return;
  cbuf += result.read; len -= result.read;

  auto room = rm.findRoom(roomId).lock();
  if (room) {
    auto password = room->getPassword();
    if (password.empty() || pw == password) {
      if (room->isOutdated()) {
        sender.doNotify("ErrorMsg", "room is outdated");
      } else {
        if (!observe) {
          room->addPlayer(sender);
        } else {
          room->addObserver(sender);
        }

        auto sroom = sender.getRoom().lock();
        if (sroom && sroom->getId() == room->getId()) {
          removePlayer(sender);
        }
      }
    } else {
      sender.doNotify("ErrorMsg", "room password error");
    }
  } else {
    sender.doNotify("ErrorMsg", "no such room");
  }
}

void Lobby::enterRoom(ServerPlayer &sender, const Packet &pkt) {
  joinRoom(sender, pkt, false);
}

void Lobby::observeRoom(ServerPlayer &sender, const Packet &pkt) {
  joinRoom(sender, pkt, true);
}

void Lobby::refreshRoomList(ServerPlayer &sender, const Packet &) {
  auto &rm = Server::instance().room_manager();

  auto &rooms = rm.getRooms();

  // 拼好cbor 首先拼一个头
  std::ostringstream oss;

  size_t sz = rooms.size();
  char buf[10];
  size_t len = cbor_encode_uint(sz, (cbor_mutable_data)buf, 10);
  buf[0] += 0x80; // uint -> array header

  oss << std::string_view { buf, len };

  for (auto &[_, room] : rooms) {
    if (room->isFull()) continue;
    oss << Cbor::encodeArray({
      room->getId(),
      room->getName().data(),
      room->getGameMode().data(),
      room->getPlayers().size(),
      room->getCapacity(),
      !room->getPassword().empty(),
      room->isOutdated(),
    });
  }
  for (auto &[_, room] : rooms) {
    if (!room->isFull()) continue;
    oss << Cbor::encodeArray({
      room->getId(),
      room->getName().data(),
      room->getGameMode().data(),
      room->getPlayers().size(),
      room->getCapacity(),
      !room->getPassword().empty(),
      room->isOutdated(),
    });
  }
  sender.doNotify("UpdateRoomList", oss.str());
}

void Lobby::handleTask(ServerPlayer &sender, const Packet &packet) {
  json arr;
  try {
    arr = json::from_cbor(packet.cborData);
  } catch (...) {
    return;
  }
  if (!arr.is_array()) return;

  if (arr.size() != 2) return;
  std::string type = arr[0];
  auto bindata = json::to_cbor(arr[1]);
  std::string data { bindata.begin(), bindata.end() };

  auto &tm = Server::instance().task_manager();
  auto &task = tm.createTask(type, data);
  tm.attachTaskToUser(task.getId(), sender.getConnId());
  task.start();
}

typedef void (Lobby::*room_cb)(ServerPlayer &, const Packet &);

void Lobby::handlePacket(ServerPlayer &sender, const Packet &packet) {
  static const std::unordered_map<std::string_view, room_cb> lobby_actions = {
    {"UpdateAvatar", &Lobby::updateAvatar},
    {"UpdatePassword", &Lobby::updatePassword},
    {"CreateRoom", &Lobby::createRoom},
    {"EnterRoom", &Lobby::enterRoom},
    {"ObserveRoom", &Lobby::observeRoom},
    {"RefreshRoomList", &Lobby::refreshRoomList},
    {"LobbyTask", &Lobby::handleTask},
    {"Chat", &Lobby::chat},
    {"ReadGlobalSaveState", &Lobby::readGlobalSaveState},
  };

  auto iter = lobby_actions.find(packet.command);
  if (iter != lobby_actions.end()) {
    auto func = iter->second;
    (this->*func)(sender, packet);
  }
}
