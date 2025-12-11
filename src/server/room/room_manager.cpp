// SPDX-License-Identifier: GPL-3.0-or-later

#include "server/room/room_manager.h"
#include "server/room/room.h"
#include "server/room/roombase.h"
#include "server/room/lobby.h"
#include "server/user/serverplayer.h"
#include "server/gamelogic/roomthread.h"
#include "server/server.h"
#include <spdlog/spdlog.h>

RoomManager::RoomManager() {
  m_lobby = std::make_shared<Lobby>();
}

std::shared_ptr<Room> RoomManager::createRoom(ServerPlayer &creator, const std::string &name, int capacity,
                                              int timeout, const std::string &settings)
{
  auto &server = Server::instance();
  if (!server.checkBanWord(name)) {
    creator.doNotify("ErrorMsg", "unk error");
    return nullptr;
  }

  auto &thread = server.getAvailableThread();

  auto room = std::make_shared<Room>();
  auto id = room->getId();

  rooms[id] = room;
  room->setName(name);
  room->setCapacity(capacity);
  room->setThread(thread);
  room->setTimeout(timeout);
  room->setSettings(settings);
  return room;
}

void RoomManager::removeRoom(int id) {
  if (rooms.contains(id)) {
    rooms.erase(id);
  }
}

std::weak_ptr<Room> RoomManager::findRoom(int id) const {
  try {
    return rooms.at(id);
  } catch (const std::out_of_range &) {
    return {};
  }
}

std::weak_ptr<Lobby> RoomManager::lobby() const {
  return m_lobby;
}

auto RoomManager::getRooms() const -> const decltype(rooms) & {
  return rooms;
}
