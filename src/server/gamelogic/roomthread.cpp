// SPDX-License-Identifier: GPL-3.0-or-later

#include "server/gamelogic/roomthread.h"
#include "server/server.h"
// #include "core/util.h"
#include "core/c-wrapper.h"
// #include "server/rpc-lua/rpc-lua.h"
#include "server/gamelogic/rpc-dispatchers.h"
#include "server/user/player.h"
#include "server/user/user_manager.h"
#include "server/room/room_manager.h"
#include "server/room/room.h"
#include "server/rpc-lua/rpc-lua.h"

#include <spdlog/spdlog.h>
#include <thread>
#include <pthread.h>

using json = nlohmann::json;

namespace asio = boost::asio;
using namespace std::literals;

RoomThread::RoomThread(asio::io_context &main_ctx) : io_ctx {},
  m_thread {} // 调用start后才有效
{
  static int nextThreadId = 1000;
  m_id = nextThreadId++;

  auto &server = Server::instance();
  m_capacity = server.config().roomCountPerThread;
  md5 = server.getMd5();

  // 在run中创建，这样就能在接下来的exec中处理事件了
  // 这集可以直接在构造函数创了 Qt故事里面是为了绑定到新线程对应的eventLoop
  L = std::make_unique<RpcLua>(io_ctx);

  push_request_callback = [&](const std::string msg) {
    // spdlog::debug("--> PushRequest {}" , msg);
    L->call("HandleRequest", msg);
  };
  delay_callback = [&](int roomId, int ms) {
    // spdlog::debug("--> Delay {} {}", roomId, ms);
    auto timer = std::make_shared<asio::steady_timer>(io_ctx, std::chrono::milliseconds(ms));
    timer->async_wait([weak = weak_from_this(), roomId, timer](const std::error_code& ec){
      if (!ec) {
        auto t = weak.lock();
        if (!t) return;
        t->L->call("ResumeRoom", roomId, "delay_done"sv);
      } else {
        spdlog::error("error in delay(): {}", ec.message());
      }
    });
  };
  wake_up_callback = [&](int roomId, const std::string reason) {
    // spdlog::debug("--> ResumeRoom {} {}", roomId, reason);
    L->call("ResumeRoom", roomId, std::string_view { reason });
  };

  set_player_state_callback = [&](int connId, int pid, int roomId) {
    auto &um = Server::instance().user_manager();
    auto p = um.findPlayerByConnId(connId).lock();
    if (!p) {
      L->call("SetPlayerState", roomId, pid, Player::Offline);
      return;
    }

    // spdlog::debug("--> SetPlayerState {}, {}, {}, {}", roomId, connId, p->getId(), p->getStateString());
    L->call("SetPlayerState", roomId, p->getId(), p->getState());
  };
  add_observer_callback = [&](int connId, int roomId) {
    auto &um = Server::instance().user_manager();
    auto p = um.findPlayerByConnId(connId).lock();
    if (!p) return;

    // spdlog::debug("--> AddObserver {}, {}, {}", roomId, connId, p->getId());
    auto bin = json::to_cbor(RpcDispatchers::getPlayerObject(*p));
    L->call("AddObserver", roomId, std::string(bin.begin(), bin.end()));
  };
  remove_observer_callback = [&](int pid, int roomId) {
    // spdlog::debug("--> RemoveObserver {}, {}", roomId, pid);
    L->call("RemoveObserver", roomId, pid);
  };

  start();
}

RoomThread::~RoomThread() {
  io_ctx.stop();
  m_thread.join();
  // spdlog::debug("[MEMORY] RoomThread {} destructed", m_id);
}

int RoomThread::id() const {
  return m_id;
}

asio::io_context &RoomThread::context() {
  return io_ctx;
}

void RoomThread::start() {
  m_thread = std::thread([&] {
    pthread_setname_np(pthread_self(), "RoomThread");
    auto guard = boost::asio::make_work_guard(io_ctx);
    io_ctx.run();
  });
}

void RoomThread::shutdown() {
  md5 = ""; // outdated = true;

  auto &rm = Server::instance().room_manager();
  for (auto roomId : m_rooms) {
    auto room = rm.findRoom(roomId).lock();
    if (!room) continue;

    // 无论如何都减一下 因为Lua肯定不存在了
    room->decreaseRefCount();

    room->setOutdated();
    room->doBroadcastNotify(room->getPlayers(), "ErrorDlg", "Server Internal Error");
    rm.removeRoom(roomId);
  }
}

void RoomThread::emit_signal(std::function<void()> f) {
  if (!L->alive()) {
    spdlog::error("Lua is not working ({}). Shutting down thread {}.", L->getConnectionInfo(), m_id);
    shutdown();
    return;
  }

  asio::dispatch(io_ctx, f);
}

void RoomThread::pushRequest(const std::string &req) {
  emit_signal([=, this] { push_request_callback(req); });
}

void RoomThread::delay(int roomId, int ms) {
  emit_signal([=, this] { delay_callback(roomId, ms); });
}

void RoomThread::wakeUp(int roomId, std::string &reason) {
  emit_signal([=, this] { wake_up_callback(roomId, reason); });
}

void RoomThread::wakeUp(int roomId, const char *reason) {
  emit_signal([=, this] { wake_up_callback(roomId, reason); });
}

void RoomThread::setPlayerState(int connId, int pid, int roomId) {
  emit_signal([=, this] { set_player_state_callback(connId, pid, roomId); });
}

void RoomThread::addObserver(int connId, int roomId) {
  emit_signal([=, this] { add_observer_callback(connId, roomId); });
}

void RoomThread::removeObserver(int pid, int roomId) {
  emit_signal([=, this] { remove_observer_callback(pid, roomId); });
}

const RpcLua &RoomThread::getLua() const {
  return *L;
}

bool RoomThread::isFull() const {
  return m_capacity <= m_ref_count;
}

int RoomThread::getCapacity() const { return m_capacity; }

std::string RoomThread::getMd5() const { return md5; }

bool RoomThread::isOutdated() {
  bool ret = md5 != Server::instance().getMd5();
  if (ret) {
    // 让以后每次都outdate
    // 不然反复disable/enable的情况下会出乱子
    md5 = "";
  }
  return ret;
}

int RoomThread::getRefCount() const {
  return m_ref_count;
}

void RoomThread::increaseRefCount() {
  m_ref_count++;
}

void RoomThread::decreaseRefCount() {
  m_ref_count--;
  if (m_ref_count > 0) return;

  if (isOutdated()) {
    asio::post(Server::instance().context(), [this] {
      Server::instance().removeThread(m_id);
    });
  }
}

void RoomThread::addRoom(int roomId) {
  m_rooms.push_back(roomId);
}

void RoomThread::removeRoom(int roomId) {
  if (auto it = std::ranges::find(m_rooms, roomId); it != m_rooms.end()) {
    m_rooms.erase(it);
  }
}
