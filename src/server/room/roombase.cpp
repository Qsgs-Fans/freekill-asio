// SPDX-License-Identifier: GPL-3.0-or-later

#include "server/room/roombase.h"
#include "server/room/lobby.h"
#include "server/room/room.h"
#include "server/server.h"
#include "server/room/room_manager.h"
#include "server/user/user_manager.h"
#include "server/user/player.h"
#include "network/client_socket.h"
#include "core/c-wrapper.h"
#include "core/util.h"
#include "server/io/dbthread.hpp"

#include <nlohmann/json.hpp>
using json = nlohmann::json;

bool RoomBase::isLobby() const {
  return dynamic_cast<const Lobby *>(this) != nullptr;
}

int RoomBase::getId() const { return id; }

void RoomBase::doBroadcastNotify(const std::vector<int> targets,
                                 const std::string_view &command, const std::string_view &cborData) {
  auto &um = Server::instance().user_manager();
  for (auto connId : targets) {
    auto p = um.findPlayerByConnId(connId).lock();
    if (p) p->doNotify(command, cborData);
  }
}

void RoomBase::chat(Player &sender, const Packet &packet) {
  auto &server = Server::instance();
  auto &um = server.user_manager();
  auto data = packet.cborData;

  json mp;
  try {
    mp = json::from_cbor(data);
  } catch (...) {
    return;
  }
  if (!mp.is_object()) return;

  int type = mp.value("type", 1);
  std::string msg = mp.value("msg", "");
  auto senderId = sender.getId();

  if (!server.checkBanWord(msg)) {
    return;
  }

  int muteType = server.isMuted(senderId);
  if (muteType == 1) { // 完全禁言
    return;
  } else if (muteType == 2 && msg.starts_with("$")) {
    return;
  }

  // 300字限制，与客户端相同 STL必须先判长度
  if (msg.size() > 300)
    msg.erase(msg.begin() + 300, msg.end());

  // 新map: { type, sender, msg, [userName] }

  if (type == 1) {
    auto lobby = dynamic_cast<Lobby *>(this);
    if (!lobby) return;

    auto vec = json::to_cbor({
      { "type", 1 },
      { "sender", senderId },
      { "userName", sender.getScreenName() },
      { "msg", msg },
    });

    for (auto &[pid, _] : lobby->getPlayers()) {
      auto p = um.findPlayerByConnId(pid).lock();
      if (p) p->doNotify("Chat", std::string(vec.begin(), vec.end()));
    }
  } else {
    auto room = dynamic_cast<Room *>(this);
    if (!room) return;

    auto vec = json::to_cbor({
      { "type", 2 },
      { "sender", senderId },
      { "msg", msg },
    });
    std::string payload(vec.begin(), vec.end());

    room->doBroadcastNotify(room->getPlayers(), "Chat", payload);
    room->doBroadcastNotify(room->getObservers(), "Chat", payload);
  }

  spdlog::info("[Chat/{}] {}: {}",
        isLobby() ? "Lobby" : fmt::format("#{}", dynamic_cast<Room *>(this)->getId()),
        sender.getScreenName(),
        msg);
}

void RoomBase::readGlobalSaveState(Player &sender, const Packet &packet) {
  // Packet内容：一个string
  auto cbuf = (cbor_data)packet.cborData.data();
  auto len = packet.cborData.size();
  struct cbor_decoder_result decode_result;

  std::string_view key;
  decode_result = cbor_stream_decode(cbuf, len, &Cbor::stringCallbacks, &key);
  if (decode_result.read == 0) return;

  sender.getGlobalSaveState(key, [weak_player = sender.weak_from_this()](std::string val) {
    auto p = weak_player.lock();
    if (!p) return;
    auto &sender = *p;

    auto vec = json::to_cbor(val);
    sender.doNotify("ReadGlobalSaveState", std::string(vec.begin(), vec.end()));
  });
}

void RoomBase::saveGlobalState(std::string_view key, std::string_view jsonData, std::function<void()> &&cb) {
  do {
    if (!Sqlite3::checkString(key)) {
      spdlog::error("Invalid key string for saveGlobalState: {}", std::string(key));
      break;
    }

    auto hexData = toHex(jsonData);
    auto &gamedb = Server::instance().gameDatabase();
    auto sql = fmt::format("REPLACE INTO globalSaves (uid, key, data) VALUES (0,'{}',X'{}')", key, hexData);

    return gamedb.async_exec(sql, cb);
  } while (false);

  auto &main_ctx = Server::instance().context();
  asio::post(main_ctx, cb);
}

void RoomBase::getGlobalSaveState(std::string_view key, std::function<void(std::string)> &&cb) {
  do {
    if (!Sqlite3::checkString(key)) {
      spdlog::error("Invalid key string for getGlobalSaveState: {}", std::string(key));
      break;
    }

    auto sql = fmt::format("SELECT data FROM globalSaves WHERE uid = 0 AND key = '{}'", key);

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
