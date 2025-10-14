// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

class ClientSocket;

class ServerSocket {
public:
  using io_context = boost::asio::io_context;
  using tcp = boost::asio::ip::tcp;
  using udp = boost::asio::ip::udp;

  ServerSocket() = delete;
  ServerSocket(ServerSocket &) = delete;
  ServerSocket(ServerSocket &&) = delete;
  ServerSocket(io_context &io_ctx, tcp::endpoint end, udp::endpoint udpEnd);

  void listen();
  void listen_udp();

  // signal connectors
  void set_new_connection_callback(std::function<void(std::shared_ptr<ClientSocket>)>);

private:
  tcp::acceptor m_acceptor;
  udp::socket m_udp_socket;

  udp::endpoint udp_remote_end;
  std::array<char, 128> udp_recv_buffer;

  // signals
  std::function<void(std::shared_ptr<ClientSocket>)> new_connection_callback;
};
