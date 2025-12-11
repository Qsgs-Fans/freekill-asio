// SPDX-License-Identifier: GPL-3.0-or-later

#include "server/gamelogic/roomthread.h"
#include "server/gamelogic/rpc-dispatchers.h"
#include "server/server.h"
#include "server/user/user_manager.h"
#include "server/user/player.h"
#include "server/room/room_manager.h"
#include "server/room/room.h"

using json = nlohmann::json;

// 这何尝不是一种手搓swig。。

using namespace JsonRpc;
using namespace std::literals;
using _rpcRet = std::pair<bool, JsonRpcParam>;

static JsonRpcParam nullVal;

// part1: stdout相关

static _rpcRet _rpc_qDebug(const JsonRpcPacket &packet) {
  if (!( packet.param_count == 1 && std::holds_alternative<std::string_view>(packet.param1) )) {
    return { false, nullVal };
  }

  spdlog::debug("{}", std::get<std::string_view>(packet.param1));
  return { true, nullVal };
}

static _rpcRet _rpc_qInfo(const JsonRpcPacket &packet) {
  if (!( packet.param_count == 1 && std::holds_alternative<std::string_view>(packet.param1) )) {
    return { false, nullVal };
  }

  spdlog::info("{}", std::get<std::string_view>(packet.param1));
  return { true, nullVal };
}

static _rpcRet _rpc_qWarning(const JsonRpcPacket &packet) {
  if (!( packet.param_count == 1 && std::holds_alternative<std::string_view>(packet.param1) )) {
    return { false, nullVal };
  }

  spdlog::warn("{}", std::get<std::string_view>(packet.param1));
  return { true, nullVal };
}

static _rpcRet _rpc_qCritical(const JsonRpcPacket &packet) {
  if (!( packet.param_count == 1 && std::holds_alternative<std::string_view>(packet.param1) )) {
    return { false, nullVal };
  }

  spdlog::error("{}", std::get<std::string_view>(packet.param1));
  return { true, nullVal };
}

static _rpcRet _rpc_print(const JsonRpcPacket &packet) {
  for (size_t i = 0; i < packet.param_count; i++) {
    switch (i) {
      case 0:
        std::cout << std::get<std::string_view>(packet.param1) << '\t';
        break;
      case 1:
        std::cout << std::get<std::string_view>(packet.param2) << '\t';
        break;
      case 2:
        std::cout << std::get<std::string_view>(packet.param3) << '\t';
        break;
      case 3:
        std::cout << std::get<std::string_view>(packet.param4) << '\t';
        break;
      case 4:
        std::cout << std::get<std::string_view>(packet.param5) << '\t';
        break;
    }
  }
  std::cout << std::endl;
  return { true, nullVal };
}

// part2: Player相关

static _rpcRet _rpc_Player_doRequest(const JsonRpcPacket &packet) {
  if (!( packet.param_count == 5 &&
    std::holds_alternative<int>(packet.param1) &&
    std::holds_alternative<std::string_view>(packet.param2) &&
    std::holds_alternative<std::string_view>(packet.param3) &&
    std::holds_alternative<int>(packet.param4) &&
    std::holds_alternative<int64_t>(packet.param5)
  )) {
    return { false, nullVal };
  }

  auto connId = std::get<int>(packet.param1);
  auto command = std::get<std::string_view>(packet.param2);
  auto jsonData = std::get<std::string_view>(packet.param3);
  auto timeout = std::get<int>(packet.param4);
  int64_t timestamp = std::get<int64_t>(packet.param5);

  auto player = Server::instance().user_manager().findPlayerByConnId(connId).lock();
  if (!player) {
    return { false, "Player not found"sv };
  }

  player->doRequest(command, jsonData, timeout, timestamp);

  return { true, nullVal };
}

static _rpcRet _rpc_Player_waitForReply(const JsonRpcPacket &packet) {
  if (!( packet.param_count == 2 &&
    std::holds_alternative<int>(packet.param1) &&
    std::holds_alternative<int>(packet.param2)
  )) {
    return { false, nullVal };
  }

  auto connId = std::get<int>(packet.param1);
  auto timeout = std::get<int>(packet.param2);

  auto player = Server::instance().user_manager().findPlayerByConnId(connId).lock();
  if (!player) {
    return { false, "Player not found"sv };
  }

  auto reply = player->waitForReply(timeout);
  return { true, reply };
}

static _rpcRet _rpc_Player_doNotify(const JsonRpcPacket &packet) {
  if (!( packet.param_count == 3 &&
    std::holds_alternative<int>(packet.param1) &&
    std::holds_alternative<std::string_view>(packet.param2) &&
    std::holds_alternative<std::string_view>(packet.param3)
  )) {
    return { false, nullVal };
  }

  auto connId = std::get<int>(packet.param1);
  auto command = std::get<std::string_view>(packet.param2);
  auto jsonData = std::get<std::string_view>(packet.param3);

  auto player = Server::instance().user_manager().findPlayerByConnId(connId).lock();
  if (!player) {
    return { false, "Player not found"sv };
  }

  player->doNotify(command, jsonData);

  return { true, nullVal };
}

static _rpcRet _rpc_Player_thinking(const JsonRpcPacket &packet) {
  if (!( packet.param_count == 1 &&
    std::holds_alternative<int>(packet.param1)
  )) {
    return { false, nullVal };
  }

  auto connId = std::get<int>(packet.param1);
  auto player = Server::instance().user_manager().findPlayerByConnId(connId).lock();
  if (!player) {
    return { false, "Player not found"sv };
  }

  bool isThinking = player->thinking();
  return { true, isThinking };
}

static _rpcRet _rpc_Player_setThinking(const JsonRpcPacket &packet) {
  if (!( packet.param_count == 2 &&
    std::holds_alternative<int>(packet.param1) &&
    std::holds_alternative<bool>(packet.param2)
  )) {
    return { false, nullVal };
  }

  auto connId = std::get<int>(packet.param1);
  bool thinking = std::get<bool>(packet.param2);

  auto player = Server::instance().user_manager().findPlayerByConnId(connId).lock();
  if (!player) {
    return { false, "Player not found"sv };
  }

  player->setThinking(thinking);
  return { true, nullVal };
}

static _rpcRet _rpc_Player_setDied(const JsonRpcPacket &packet) {
  if (!( packet.param_count == 2 &&
    std::holds_alternative<int>(packet.param1) &&
    std::holds_alternative<bool>(packet.param2)
  )) {
    return { false, nullVal };
  }

  auto connId = std::get<int>(packet.param1);
  bool died = std::get<bool>(packet.param2);

  auto player = Server::instance().user_manager().findPlayerByConnId(connId).lock();
  if (!player) {
    return { false, "Player not found"sv };
  }

  player->setDied(died);
  return { true, nullVal };
}

static _rpcRet _rpc_Player_emitKick(const JsonRpcPacket &packet) {
  if (!( packet.param_count == 1 &&
    std::holds_alternative<int>(packet.param1)
  )) {
    return { false, nullVal };
  }

  auto connId = std::get<int>(packet.param1);
  auto player = Server::instance().user_manager().findPlayerByConnId(connId).lock();
  if (!player) {
    return { false, "Player not found"sv };
  }

  player->emitKicked();
  return { true, nullVal };
}

static _rpcRet _rpc_Player_saveState(const JsonRpcPacket &packet) {
  if (!(packet.param_count == 2 &&
    std::holds_alternative<int>(packet.param1) &&
    std::holds_alternative<std::string_view>(packet.param2)
  )) {
    return { false, nullVal };
  }

  auto connId = std::get<int>(packet.param1);
  auto jsonData = std::get<std::string_view>(packet.param2);

  auto player = Server::instance().user_manager().findPlayerByConnId(connId).lock();
  if (!player) {
    return { false, nullVal };
  }

  auto room_base = player->getRoom().lock();
  auto room = dynamic_pointer_cast<Room>(room_base);
  player->saveState(jsonData, [room] {
    if (!room) return;
    auto thread = room->thread().lock();
    if (thread) thread->wakeUp(room->getId(), "query_done");
  });
  return { true, true };
}

static _rpcRet _rpc_Player_getSaveState(const JsonRpcPacket &packet) {
  if (!(packet.param_count == 1 &&
    std::holds_alternative<int>(packet.param1)
  )) {
    return { false, nullVal };
  }

  auto connId = std::get<int>(packet.param1);

  auto player = Server::instance().user_manager().findPlayerByConnId(connId).lock();
  if (!player) {
    return { false, nullVal };
  }

  auto room_base = player->getRoom().lock();
  auto room = dynamic_pointer_cast<Room>(room_base);
  player->getSaveState([room](std::string result) {;
    if (!room) return;
    auto thread = room->thread().lock();
    if (thread) thread->wakeUp(room->getId(), result);
  });
  return { true, true };
}

static _rpcRet _rpc_Player_saveGlobalState(const JsonRpcPacket &packet) {
  if (!(packet.param_count == 3 &&
    std::holds_alternative<int>(packet.param1) &&
    std::holds_alternative<std::string_view>(packet.param2) &&
    std::holds_alternative<std::string_view>(packet.param3)
  )) {
    return { false, nullVal };
  }

  auto connId = std::get<int>(packet.param1);
  auto key = std::get<std::string_view>(packet.param2);
  auto jsonData = std::get<std::string_view>(packet.param3);

  auto player = Server::instance().user_manager().findPlayerByConnId(connId).lock();
  if (!player) {
    return { false, nullVal };
  }

  auto room_base = player->getRoom().lock();
  auto room = dynamic_pointer_cast<Room>(room_base);
  player->saveGlobalState(key, jsonData, [room] {
    if (!room) return;
    auto thread = room->thread().lock();
    if (thread) thread->wakeUp(room->getId(), "query_done");
  });
  return { true, true };
}

static _rpcRet _rpc_Player_getGlobalSaveState(const JsonRpcPacket &packet) {
  if (!(packet.param_count == 2 &&
    std::holds_alternative<int>(packet.param1) &&
    std::holds_alternative<std::string_view>(packet.param2)
  )) {
    return { false, nullVal };
  }

  auto connId = std::get<int>(packet.param1);
  auto key = std::get<std::string_view>(packet.param2);

  auto player = Server::instance().user_manager().findPlayerByConnId(connId).lock();
  if (!player) {
    return { false, nullVal };
  }

  auto room_base = player->getRoom().lock();
  auto room = dynamic_pointer_cast<Room>(room_base);
  player->getGlobalSaveState(key, [room](std::string result) {
    if (!room) return;
    auto thread = room->thread().lock();
    if (thread) thread->wakeUp(room->getId(), result);
  });
  return { true, true };
}


// part3: Room相关

static _rpcRet _rpc_Room_delay(const JsonRpcPacket &packet) {
  if (!( packet.param_count == 2 &&
    std::holds_alternative<int>(packet.param1) &&
    std::holds_alternative<int>(packet.param2)
  )) {
    return { false, nullVal };
  }
  int id = std::get<int>(packet.param1);
  int ms = std::get<int>(packet.param2);
  if (ms <= 0) {
    return { false, nullVal };
  }
  auto room = Server::instance().room_manager().findRoom(id).lock();
  if (!room) {
    return { false, "Room not found"sv };
  }

  room->delay(ms);

  return { true, nullVal };
}

static _rpcRet _rpc_Room_updatePlayerWinRate(const JsonRpcPacket &packet) {
  if (!( packet.param_count == 5 &&
    std::holds_alternative<int>(packet.param1) &&
    std::holds_alternative<int>(packet.param2) &&
    std::holds_alternative<std::string_view>(packet.param3) &&
    std::holds_alternative<std::string_view>(packet.param4) &&
    std::holds_alternative<int>(packet.param5)
  )) {
    return { false, nullVal };
  }

  int roomId = std::get<int>(packet.param1);
  int playerId = std::get<int>(packet.param2);
  auto mode = std::get<std::string_view>(packet.param3);
  auto role = std::get<std::string_view>(packet.param4);
  int result = std::get<int>(packet.param5);

  auto room = Server::instance().room_manager().findRoom(roomId).lock();
  if (!room) {
    return { false, "Room not found"sv };
  }

  room->updatePlayerWinRate(playerId, mode, role, result);

  return { true, nullVal };
}

static _rpcRet _rpc_Room_updateGeneralWinRate(const JsonRpcPacket &packet) {
  if (!( packet.param_count == 5 &&
    std::holds_alternative<int>(packet.param1) &&
    std::holds_alternative<std::string_view>(packet.param2) &&
    std::holds_alternative<std::string_view>(packet.param3) &&
    std::holds_alternative<std::string_view>(packet.param4) &&
    std::holds_alternative<int>(packet.param5)
  )) {
    return { false, nullVal };
  }

  int roomId = std::get<int>(packet.param1);
  auto general = std::get<std::string_view>(packet.param2);
  auto mode = std::get<std::string_view>(packet.param3);
  auto role = std::get<std::string_view>(packet.param4);
  int result = std::get<int>(packet.param5);

  auto room = Server::instance().room_manager().findRoom(roomId).lock();
  if (!room) {
    return { false, "Room not found"sv };
  }

  room->updateGeneralWinRate(general, mode, role, result);

  return { true, nullVal };
}

static _rpcRet _rpc_Room_gameOver(const JsonRpcPacket &packet) {
  if (!( packet.param_count == 1 &&
    std::holds_alternative<int>(packet.param1)
  )) {
    return { false, nullVal };
  }

  int roomId = std::get<int>(packet.param1);
  auto room = Server::instance().room_manager().findRoom(roomId).lock();
  if (!room) {
    return { false, "Room not found"sv };
  }

  room->gameOver();

  return { true, nullVal };
}

static _rpcRet _rpc_Room_setRequestTimer(const JsonRpcPacket &packet) {
  if (!( packet.param_count == 2 &&
    std::holds_alternative<int>(packet.param1) &&
    std::holds_alternative<int>(packet.param2)
  )) {
    return { false, nullVal };
  }

  int id = std::get<int>(packet.param1);
  int ms = std::get<int>(packet.param2);
  if (ms <= 0) {
    return { false, nullVal };
  }

  auto room = Server::instance().room_manager().findRoom(id).lock();
  if (!room) {
    return { false, "Room not found"sv };
  }

  room->setRequestTimer(ms);

  return { true, nullVal };
}

static _rpcRet _rpc_Room_destroyRequestTimer(const JsonRpcPacket &packet) {
  if (!( packet.param_count == 1 &&
    std::holds_alternative<int>(packet.param1)
  )) {
    return { false, nullVal };
  }

  int roomId = std::get<int>(packet.param1);
  auto room = Server::instance().room_manager().findRoom(roomId).lock();
  if (!room) {
    return { false, "Room not found"sv };
  }

  room->destroyRequestTimer();

  return { true, nullVal };
}

static _rpcRet _rpc_Room_decreaseRefCount(const JsonRpcPacket &packet) {
  if (!( packet.param_count == 1 &&
    std::holds_alternative<int>(packet.param1)
  )) {
    return { false, nullVal };
  }

  int roomId = std::get<int>(packet.param1);
  auto room = Server::instance().room_manager().findRoom(roomId).lock();
  if (!room) {
    return { false, "Room not found"sv };
  }

  room->decreaseRefCount();

  return { true, nullVal };
}

static _rpcRet _rpc_Room_getSessionId(const JsonRpcPacket &packet) {
  if (!( packet.param_count == 1 &&
    std::holds_alternative<int>(packet.param1)
  )) {
    return { false, nullVal };
  }

  int roomId = std::get<int>(packet.param1);
  auto room = Server::instance().room_manager().findRoom(roomId).lock();
  if (!room) {
    return { false, "Room not found"sv };
  }

  int id = room->getSessionId();

  return { true, id };
}

static _rpcRet _rpc_Room_getSessionData(const JsonRpcPacket &packet) {
  if (!( packet.param_count == 1 &&
    std::holds_alternative<int>(packet.param1)
  )) {
    return { false, nullVal };
  }

  int roomId = std::get<int>(packet.param1);
  auto room = Server::instance().room_manager().findRoom(roomId).lock();
  if (!room) {
    return { false, "Room not found"sv };
  }

  auto s = room->getSessionData();

  return { true, s };
}

static _rpcRet _rpc_Room_setSessionData(const JsonRpcPacket &packet) {
  if (!( packet.param_count == 2 &&
    std::holds_alternative<int>(packet.param1) &&
    std::holds_alternative<std::string_view>(packet.param2)
  )) {
    return { false, nullVal };
  }

  int roomId = std::get<int>(packet.param1);
  auto jsonData = std::get<std::string_view>(packet.param2);
  auto room = Server::instance().room_manager().findRoom(roomId).lock();
  if (!room) {
    return { false, "Room not found"sv };
  }

  room->setSessionData(jsonData);

  return { true, nullVal };
}

static _rpcRet _rpc_Room_addNpc(const JsonRpcPacket &packet) {
  if (!( packet.param_count == 1 &&
    std::holds_alternative<int>(packet.param1)
  )) {
    return { false, nullVal };
  }

  int roomId = std::get<int>(packet.param1);
  auto room = Server::instance().room_manager().findRoom(roomId).lock();
  if (!room) {
    return { false, "Room not found"sv };
  }

  auto &bot = room->addNpc();

  auto bin = json::to_cbor(RpcDispatchers::getPlayerObject(bot));

  return { true, std::string(bin.begin(), bin.end()) };
}

static _rpcRet _rpc_Room_removeNpc(const JsonRpcPacket &packet) {
  if (!( packet.param_count == 2 &&
    std::holds_alternative<int>(packet.param1) &&
    std::holds_alternative<int>(packet.param2)
  )) {
    return { false, nullVal };
  }

  int id = std::get<int>(packet.param1);
  int pid = std::get<int>(packet.param2);

  auto room = Server::instance().room_manager().findRoom(id).lock();
  if (!room) {
    return { false, "Room not found"sv };
  }

  auto player = Server::instance().user_manager().findPlayerByConnId(pid).lock();
  if (!player) {
    return { false, "Player not found"sv };
  }

  room->removeNpc(*player);

  return { true, nullVal };
}

static _rpcRet _rpc_Room_saveGlobalState(const JsonRpcPacket &packet) {
  if (!(packet.param_count == 3 &&
    std::holds_alternative<int>(packet.param1) &&
    std::holds_alternative<std::string_view>(packet.param2) &&
    std::holds_alternative<std::string_view>(packet.param3)
  )) {
    return { false, nullVal };
  }

  auto roomId = std::get<int>(packet.param1);
  auto key = std::get<std::string_view>(packet.param2);
  auto jsonData = std::get<std::string_view>(packet.param3);

  auto room = Server::instance().room_manager().findRoom(roomId).lock();
  if (!room) {
    return { false, nullVal };
  }

  room->saveGlobalState(key, jsonData, [room] {
    if (!room) return;
    auto thread = room->thread().lock();
    if (thread) thread->wakeUp(room->getId(), "query_done");
  });
  return { true, true };
}

static _rpcRet _rpc_Room_getGlobalSaveState(const JsonRpcPacket &packet) {
  if (!(packet.param_count == 2 &&
    std::holds_alternative<int>(packet.param1) &&
    std::holds_alternative<std::string_view>(packet.param2)
  )) {
    return { false, nullVal };
  }

  auto roomId = std::get<int>(packet.param1);
  auto key = std::get<std::string_view>(packet.param2);

  auto room = Server::instance().room_manager().findRoom(roomId).lock();
  if (!room) {
    return { false, nullVal };
  }

  room->getGlobalSaveState(key, [room](std::string result) {
    if (!room) return;
    auto thread = room->thread().lock();
    if (thread) thread->wakeUp(room->getId(), result);
  });
  return { true, true };
}


// 收官：getRoom

json RpcDispatchers::getPlayerObject(Player &p) {
  return {
    { "connId", p.getConnId() },
    { "id", p.getId() },
    { "screenName", p.getScreenName() },
    { "avatar", p.getAvatar() },
    { "totalGameTime", p.getTotalGameTime() },
    { "state", p.getState() },
    { "gameData", p.getGameData() },
  };
}

static _rpcRet _rpc_RoomThread_getRoom(const JsonRpcPacket &packet) {
  if (!( packet.param_count == 1 &&
    std::holds_alternative<int>(packet.param1)
  )) {
    return { false, nullVal };
  }
  int id = std::get<int>(packet.param1);
  if (id <= 0) {
    return { false, nullVal };
  }

  auto room = Server::instance().room_manager().findRoom(id).lock();
  if (!room) {
    return { false, "Room not found"sv };
  }

  const auto &pids = room->getPlayers();

  auto owner = room->getOwner().lock();
  json j {
    { "id", room->getId() },
    { "ownerId", owner ? owner->getId() : 0 },
    { "timeout", room->getTimeout() },
    { "settings", room->getSettings() },
  };

  auto players = json::array();
  auto &um = Server::instance().user_manager();
  for (auto pid : pids) {
    auto p = um.findPlayerByConnId(pid).lock();
    if (p) players.push_back( RpcDispatchers::getPlayerObject(*p) );
  }
  j["players"] = players;

  auto bin = json::to_cbor(j);
  return { true, std::string(bin.begin(), bin.end()) };
}

const JsonRpc::RpcMethodMap RpcDispatchers::ServerRpcMethods {
  { "qDebug", _rpc_qDebug },
  { "qInfo", _rpc_qInfo },
  { "qWarning", _rpc_qWarning },
  { "qCritical", _rpc_qCritical },
  { "print", _rpc_print },

  { "ServerPlayer_doRequest", _rpc_Player_doRequest },
  { "ServerPlayer_waitForReply", _rpc_Player_waitForReply },
  { "ServerPlayer_doNotify", _rpc_Player_doNotify },
  { "ServerPlayer_thinking", _rpc_Player_thinking },
  { "ServerPlayer_setThinking", _rpc_Player_setThinking },
  { "ServerPlayer_setDied", _rpc_Player_setDied },
  { "ServerPlayer_emitKick", _rpc_Player_emitKick },
  { "ServerPlayer_saveState", _rpc_Player_saveState },
  { "ServerPlayer_getSaveState", _rpc_Player_getSaveState },
  { "ServerPlayer_saveGlobalState", _rpc_Player_saveGlobalState },
  { "ServerPlayer_getGlobalSaveState", _rpc_Player_getGlobalSaveState },

  { "Room_delay", _rpc_Room_delay },
  { "Room_updatePlayerWinRate", _rpc_Room_updatePlayerWinRate },
  { "Room_updateGeneralWinRate", _rpc_Room_updateGeneralWinRate },
  { "Room_gameOver", _rpc_Room_gameOver },
  { "Room_setRequestTimer", _rpc_Room_setRequestTimer },
  { "Room_destroyRequestTimer", _rpc_Room_destroyRequestTimer },
  { "Room_decreaseRefCount", _rpc_Room_decreaseRefCount },
  { "Room_getSessionId", _rpc_Room_getSessionId },
  { "Room_getSessionData", _rpc_Room_getSessionData },
  { "Room_setSessionData", _rpc_Room_setSessionData },
  { "Room_addNpc", _rpc_Room_addNpc },
  { "Room_removeNpc", _rpc_Room_removeNpc },
  { "Room_saveGlobalState", _rpc_Room_saveGlobalState },
  { "Room_getGlobalSaveState", _rpc_Room_getGlobalSaveState },

  { "RoomThread_getRoom", _rpc_RoomThread_getRoom },
};
