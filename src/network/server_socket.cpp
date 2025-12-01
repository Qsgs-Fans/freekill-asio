// SPDX-License-Identifier: GPL-3.0-or-later

#include "network/server_socket.h"
#include "network/client_socket.h"

#include "server/server.h"
#include "server/user/user_manager.h"

#include <nlohmann/json.hpp>

namespace asio = boost::asio;
using asio::awaitable;
using asio::detached;
using asio::use_awaitable;
using asio::redirect_error;

using json = nlohmann::json;

ServerSocket::ServerSocket(asio::io_context &io_ctx, tcp::endpoint end, udp::endpoint udpEnd):
  worker_io {}, main_io { io_ctx }, m_acceptor { worker_io, end }, m_udp_socket { io_ctx, udpEnd }
{
  spdlog::info("server is ready to listen on [{}]:{}", end.address().to_string(), end.port());
}

ServerSocket::~ServerSocket() {
  // worker_io.stop();
  m_acceptor.close();
  m_thread.join();
}

void ServerSocket::start() {
  m_thread = std::thread([&] {
    pthread_setname_np(pthread_self(), "AcceptorThread");
    // auto guard = boost::asio::make_work_guard(worker_io);
    asio::co_spawn(worker_io, listener(), asio::detached);
    asio::co_spawn(main_io, udpListener(), asio::detached);
    worker_io.run();
  });
}

awaitable<void> ServerSocket::listener() {
  for (;;) {
    boost::system::error_code ec;

    // 先创建绑定到main_io的socket，再在这里accept
    auto socket = tcp::socket(main_io);
    co_await m_acceptor.async_accept(socket, redirect_error(use_awaitable, ec));

    if (!ec) {
      asio::post(main_io, [cb = std::forward<decltype(new_connection_callback)>(new_connection_callback), socket = std::move(socket)] () mutable {
        try {
          auto conn = std::make_shared<ClientSocket>(std::move(socket));

          if (cb) {
            cb(conn);
            conn->start();
          }
        } catch (std::system_error &e) {
          spdlog::error("ClientSocket creation error: {}", e.what());
        }
      });
    } else if (ec == asio::error::operation_aborted) {
      co_return;
    } else {
      spdlog::error("Accept error: {}", ec.message());
    }
  }
}

awaitable<void> ServerSocket::udpListener() {
  for (;;) {
    auto buffer = asio::buffer(udp_recv_buffer);
    boost::system::error_code ec;
    auto len = co_await m_udp_socket.async_receive_from(
      buffer, udp_remote_end, redirect_error(use_awaitable, ec));

    auto sv = std::string_view { udp_recv_buffer.data(), len };
    // spdlog::debug("RX (udp [{}]:{}): {}", udp_remote_end.address().to_string(), udp_remote_end.port(), sv);
    if (sv == "fkDetectServer") {
      m_udp_socket.async_send_to(
        asio::const_buffer("me", 2), udp_remote_end, detached);
    } else if (sv.starts_with("fkGetDetail,")) {
      auto &conf = Server::instance().config();
      auto &um = Server::instance().user_manager();

      auto json = nlohmann::json {
        "0.5.14+",
        conf.iconUrl,
        conf.description,
        conf.capacity,
        um.getPlayers().size(),
        std::string(sv).substr(12)
      }.dump();

      m_udp_socket.async_send_to(
        asio::const_buffer(json.data(), json.size()), udp_remote_end, detached);

      // spdlog::debug("TX (udp [{}]:{}): {}", udp_remote_end.address().to_string(), udp_remote_end.port(), json);
    }
  }
}

void ServerSocket::set_new_connection_callback(std::function<void(std::shared_ptr<ClientSocket>)> f) {
  new_connection_callback = f;
}

