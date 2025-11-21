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

  struct cbor_load_result result;
  auto mp = cbor_load((cbor_data)data.data(), data.size(), &result);
  if (!cbor_isa_map(mp)) {
    cbor_decref(&mp);
    return;
  }

  auto sz = cbor_map_size(mp);
  auto pairs = cbor_map_handle(mp);

  int type = 1;
  std::string msg;
  auto senderId = sender.getId();

  for (size_t i = 0; i < sz; i++) {
    auto pair = pairs[i];
    auto k = pair.key, v = pair.value;

    if (cbor_isa_string(k) && strncmp((const char *)cbor_string_handle(k), "msg", 3) == 0) {
      if (!cbor_isa_string(v)) continue;
      msg = std::string { (const char *)cbor_string_handle(v), cbor_string_length(v) };
    }

    if (cbor_isa_string(k) && strncmp((const char *)cbor_string_handle(k), "type", 4) == 0) {
      if (!cbor_isa_uint(v)) continue;
      type = cbor_get_uint8(v);
    }
  }

  // 好了mp没用了 我们必须发新map给client
  cbor_decref(&mp);

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

  // 还是来拼好cbor吧 前有手搓cbor
  std::ostringstream oss;
  char uint_buf[10]; size_t uint_len;
  uint_len = cbor_encode_uint(senderId, (cbor_mutable_data)uint_buf, 10);

  if (type == 1) {
    auto lobby = dynamic_cast<Lobby *>(this);
    if (!lobby) return;

    oss << "\xA4\x64type\x01\x66sender" << std::string_view { uint_buf, uint_len }
      << "\x68userName";

    uint_len = cbor_encode_uint(sender.getScreenName().size(), (cbor_mutable_data)uint_buf, 10);
    uint_buf[0] += 0x60; // uint(n) -> str(#)
    oss << std::string_view { uint_buf, uint_len } << sender.getScreenName()
      << "\x63msg";

    uint_len = cbor_encode_uint(msg.size(), (cbor_mutable_data)uint_buf, 10);
    uint_buf[0] += 0x60; // uint(n) -> str(#)
    oss << std::string_view { uint_buf, uint_len } << msg;

    for (auto &[pid, _] : lobby->getPlayers()) {
      auto p = um.findPlayerByConnId(pid).lock();
      if (p) p->doNotify("Chat", oss.str());
    }
  } else {
    auto room = dynamic_cast<Room *>(this);
    if (!room) return;

    oss << "\xA3\x64type\x02\x66sender" << std::string_view { uint_buf, uint_len }
      << "\x63msg";

    uint_len = cbor_encode_uint(msg.size(), (cbor_mutable_data)uint_buf, 10);
    uint_buf[0] += 0x60; // uint(n) -> str(#)
    oss << std::string_view { uint_buf, uint_len } << msg;

    room->doBroadcastNotify(room->getPlayers(), "Chat", oss.str());
    room->doBroadcastNotify(room->getObservers(), "Chat", oss.str());
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

    // 包装成cbor string后原样发回
    u_char buf[10]; size_t buflen;
    buflen = cbor_encode_uint(val.size(), buf, 10);
    buf[0] += 0x60;
    std::string toSend;
    toSend.reserve(val.size() + 10);
    toSend += std::string_view { (char*)buf, buflen };
    toSend += val;
    sender.doNotify("ReadGlobalSaveState", toSend);
  });
}
