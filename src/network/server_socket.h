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

  ~ServerSocket();

  void start();

  // signal connectors
  void set_new_connection_callback(std::function<void(std::shared_ptr<ClientSocket>)>);

private:
  io_context worker_io;
  io_context &main_io;
  std::thread m_thread;

  tcp::acceptor m_acceptor;
  udp::socket m_udp_socket;

  udp::endpoint udp_remote_end;
  std::array<char, 128> udp_recv_buffer;

  // signals
  std::function<void(std::shared_ptr<ClientSocket>)> new_connection_callback;

  boost::asio::awaitable<void> listener();
  boost::asio::awaitable<void> udpListener();
};
