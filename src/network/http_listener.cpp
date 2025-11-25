#include "http_listener.h"

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;
using asio::awaitable;
using asio::detached;
using asio::use_awaitable;
using asio::redirect_error;

HttpListener::HttpListener(tcp::endpoint end) :
  io_ctx {}, endpoint { std::move(end) }
{
  spdlog::info("http API is ready to listen on {}", end.port());
}

HttpListener::~HttpListener() {
  io_ctx.stop();
  m_thread.join();
}

void HttpListener::start() {
  m_thread = std::thread(&HttpListener::run, this);
}

void HttpListener::run() {
  asio::co_spawn(io_ctx, listener(), asio::detached);
  io_ctx.run();
}

static awaitable<void> session(beast::tcp_stream stream);

awaitable<void> HttpListener::listener() {
  auto acceptor = tcp::acceptor { io_ctx, endpoint };

  for (;;) {
    boost::system::error_code ec;
    auto socket = co_await acceptor.async_accept(redirect_error(use_awaitable, ec));
    if (ec) {
      spdlog::warn("Http accept error: {}", ec.message());
      continue;
    }

    auto stream = beast::tcp_stream { std::move(socket) };
    asio::co_spawn(io_ctx, session(std::move(stream)), detached);
  }
}

static awaitable<void> session(beast::tcp_stream stream) {
  beast::flat_buffer buffer;

  for (;;) {
    stream.expires_after(std::chrono::seconds(30));

    http::request<http::string_body> req;
    boost::system::error_code ec;
    co_await http::async_read(stream, buffer, req, redirect_error(use_awaitable, ec));
    if (ec) break;

    // TODO 按照example中的添加一下http处理逻辑
    http::response<http::string_body> res{ http::status::ok, req.version() };
    res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
    res.set(http::field::content_type, "text/html");
    res.body() = "<b>Hello, world!</b>";
    res.prepare_payload();

    co_await beast::async_write(stream, http::message_generator { std::move(res) }, use_awaitable);

    // if (!req.keep_alive()) {
    if (true) {
      break;
    }
  }

  stream.socket().shutdown(tcp::socket::shutdown_send);
}
