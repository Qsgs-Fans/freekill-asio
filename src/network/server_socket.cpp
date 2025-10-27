// SPDX-License-Identifier: GPL-3.0-or-later

#include "network/server_socket.h"
#include "network/client_socket.h"

#include "server/server.h"
#include "server/user/user_manager.h"

#include <cjson/cJSON.h>

namespace asio = boost::asio;
using asio::awaitable;
using asio::detached;
using asio::use_awaitable;
using asio::redirect_error;

ServerSocket::ServerSocket(asio::io_context &io_ctx, tcp::endpoint end, udp::endpoint udpEnd):
  m_acceptor { io_ctx, end }, m_udp_socket { io_ctx, udpEnd }
{
  spdlog::info("server is ready to listen on {}", end.port());
}

void ServerSocket::start() {
  asio::co_spawn(m_acceptor.get_executor(), listener(), asio::detached);
  asio::co_spawn(m_acceptor.get_executor(), udpListener(), asio::detached);
}

awaitable<void> ServerSocket::listener() {
  for (;;) {
    boost::system::error_code ec;
    auto socket = co_await m_acceptor.async_accept(redirect_error(use_awaitable, ec));

    if (!ec) {
      try {
        auto conn = std::make_shared<ClientSocket>(std::move(socket));

        if (new_connection_callback) {
          new_connection_callback(conn);
          conn->start();
        }
      } catch (std::system_error &e) {
        spdlog::error("ClientSocket creation error: {}", e.what());
      }
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

      cJSON *jsonArray = cJSON_CreateArray();
      cJSON_AddItemToArray(jsonArray, cJSON_CreateString("0.5.14+"));
      cJSON_AddItemToArray(jsonArray, cJSON_CreateString(conf.iconUrl.c_str()));
      cJSON_AddItemToArray(jsonArray, cJSON_CreateString(conf.description.c_str()));
      cJSON_AddItemToArray(jsonArray, cJSON_CreateNumber(conf.capacity));
      cJSON_AddItemToArray(jsonArray, cJSON_CreateNumber(um.getPlayers().size()));
      cJSON_AddItemToArray(jsonArray, cJSON_CreateString(std::string(sv).substr(12).c_str()));

      char *json = cJSON_PrintUnformatted(jsonArray);
      m_udp_socket.async_send_to(
        asio::const_buffer(json, strlen(json)), udp_remote_end, detached);

      // spdlog::debug("TX (udp [{}]:{}): {}", udp_remote_end.address().to_string(), udp_remote_end.port(), json);
      cJSON_Delete(jsonArray);
      free(json);
    }
  }
}

void ServerSocket::set_new_connection_callback(std::function<void(std::shared_ptr<ClientSocket>)> f) {
  new_connection_callback = f;
}

