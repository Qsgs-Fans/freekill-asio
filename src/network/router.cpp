// SPDX-License-Identifier: GPL-3.0-or-later

#include "network/router.h"
#include "network/client_socket.h"
#include "server/user/serverplayer.h"
#include "server/server.h"
#include "core/c-wrapper.h"

namespace asio = boost::asio;

Router::Router(ServerPlayer *player, std::shared_ptr<ClientSocket> socket, RouterType type) {
  this->type = type;
  this->player = player;
  this->socket = nullptr;
  setSocket(socket);
}

Router::~Router() {
  setSocket(nullptr);
  abortRequest();
}

std::shared_ptr<ClientSocket> Router::getSocket() const { return socket; }

void Router::setSocket(std::shared_ptr<ClientSocket> socket) {
  if (this->socket != nullptr) {
    this->socket->set_message_got_callback([](Packet&){});
    this->socket->set_disconnected_callback([]{});
  }

  this->socket = nullptr;
  if (socket != nullptr) {
    socket->set_message_got_callback([this](Packet &p) { handlePacket(p); });
    socket->set_disconnected_callback([this] { player->onDisconnected(); });
    this->socket = socket;
  }
}

void Router::set_reply_ready_callback(std::function<void()> callback) {
  reply_ready_callback = std::move(callback);
}

void Router::set_notification_got_callback(std::function<void(const Packet &)> callback) {
  notification_got_callback = std::move(callback);
}

void Router::request(int type, const std::string_view &command,
                     const std::string_view &cborData, int timeout, int64_t timestamp) {
  static int requestId = 0;
  requestId++;
  if (requestId > 10000000) requestId = 1;

  expectedReplyIds.push_back(requestId);
  replyTimeout = timeout;

  using namespace std::chrono;
  requestStartTime =
    duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();

  replyMutex.lock();
  m_reply = "__notready";
  replyMutex.unlock();

  sendMessage(Cbor::encodeArray({
    requestId,
    type,
    command,
    cborData,
    timeout,
    (timestamp <= 0 ? requestStartTime : timestamp)
  }));
}

void Router::notify(int type, const std::string_view &command, const std::string_view &data) {
  if (!socket) return;
  auto buf = Cbor::encodeArray({
    -2,
    Router::TYPE_NOTIFICATION | Router::SRC_SERVER | Router::DEST_CLIENT,
    command,
    data,
  });
  sendMessage(buf);
}

// timeout永远是0
std::string Router::waitForReply(int timeout) {
  std::lock_guard<std::mutex> lock(replyMutex);
  return m_reply;
}

void Router::abortRequest() {
  std::lock_guard<std::mutex> lock(replyMutex);
  m_reply = "";
  // TODO wake up room?
}

void Router::handlePacket(const Packet &packet) {
  int requestId = packet.requestId;
  int type = packet.type;
  auto cborData = packet.cborData;

  if (type & TYPE_NOTIFICATION) {
    notification_got_callback(packet);
  } else if (type & TYPE_REPLY) {
    using namespace std::chrono;
    std::lock_guard<std::mutex> lock(replyMutex);

    auto it = std::find(expectedReplyIds.begin(), expectedReplyIds.end(), requestId);
    if (it == expectedReplyIds.end())
      return;

    expectedReplyIds.erase(it);

    auto now = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    if (replyTimeout >= 0 &&
      replyTimeout * 1000 < now - requestStartTime)

      return;

    m_reply = cborData;
    // TODO: callback?

    reply_ready_callback();
  }
}

void Router::sendMessage(const std::string &msg) {
  if (!socket) return;
  // 将send任务交给主进程（如同Qt）并等待
  auto &main_ctx = Server::instance().context();
  auto f = asio::dispatch(main_ctx, asio::use_future([&, weak = socket->weak_from_this()] {
    auto c = weak.lock();
    if (c) c->send(std::make_shared<std::string>(msg));
  }));
  f.wait();
}
