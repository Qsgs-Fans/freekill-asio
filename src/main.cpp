// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/packman.h"
#include "server/server.h"
#include "server/admin/shell.h"

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/callback_sink.h>

#include <getopt.h>

namespace asio = boost::asio;
using asio::ip::tcp;
using asio::ip::udp;

// 也是一种很蠢的方式了
static bool shellAlive = false;

#include "spdlog/pattern_formatter.h"
class thread_name_flag : public spdlog::custom_flag_formatter {
public:
  void format(const spdlog::details::log_msg &, const std::tm &, spdlog::memory_buf_t &dest) override
  {
    char thread_name[16];
    pthread_t thread = pthread_self();
    int rc = pthread_getname_np(thread, thread_name, sizeof(thread_name));
    if (rc == 0) {
      dest.append(std::string_view { thread_name, strlen(thread_name) });
    }
  }

  std::unique_ptr<custom_flag_formatter> clone() const override {
    return spdlog::details::make_unique<thread_name_flag>();
  }
};

static void initLogger() {
  auto logger_file = "freekill.log";

  std::vector<spdlog::sink_ptr> sinks;

  // 先清空已输入的内容
  auto clearline_sink = std::make_shared<spdlog::sinks::callback_sink_mt>(
    [](const spdlog::details::log_msg &msg) {
      if (!shellAlive) return;
      auto &shell = Server::instance().shell();
      shell.clearLine();
    });
  sinks.push_back(clearline_sink);

  // 然后输出到stdout中
  auto stdout_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();

  auto formatter = std::make_unique<spdlog::pattern_formatter>();
  formatter->add_flag<thread_name_flag>('t').set_pattern("\r%C-%m-%d %H:%M:%S.%e %t[%^%L%$] %v");
  stdout_sink->set_formatter(std::move(formatter));

  stdout_sink->set_color_mode(spdlog::color_mode::always);
  sinks.push_back(stdout_sink);

  // 然后输出到rotate文件
  // 单个log文件最大30M 最多备份5个 算上当前log文件的话最多同时存在6个log
  // 解决了牢版服务器关服后log消失的事情 伟大
  auto rotate_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(logger_file, 1048576 * 30, 5);

  formatter = std::make_unique<spdlog::pattern_formatter>();
  formatter->add_flag<thread_name_flag>('t').set_pattern("%C-%m-%d %H:%M:%S.%e %t[%L] %v");
  rotate_sink->set_formatter(std::move(formatter));
  sinks.push_back(rotate_sink);

  // 最后重新展示已输入内容
  auto redisplay_sink = std::make_shared<spdlog::sinks::callback_sink_mt>(
    [](const spdlog::details::log_msg &msg) {
      if (!shellAlive) return;
      auto &shell = Server::instance().shell();
      if (!shell.lineDone()) shell.redisplay();
    });
  sinks.push_back(redisplay_sink);

  auto spd_logger = std::make_shared<spdlog::logger>("fk-logger", begin(sinks), end(sinks));
  spdlog::register_logger(spd_logger);
  spdlog::set_default_logger(spd_logger);
  spdlog::set_level(spdlog::level::trace);
  spdlog::flush_every(std::chrono::seconds(3));
}

static void show_usage(const char *prog) {
  printf(
    "Usage: %s [OPTION]...\n"
    "Start FreeKill server.\n"
    "\n"
    "Options:\n"
    "  -v, --version           Display version information.\n"
    "  -h, --help              Show this help message.\n"
    "  -p, --port <port>       Specify a port number to listen on.\n"
    "\n"
    "See more at our documentation: \n"
    "<https://fkbook-all-in-one.readthedocs.io/zh-cn/latest/for-server-hosts/index.html>.\n",

    prog
  );
}

static void print_version() {
  printf(
    "freekill-asio (Non-Qt FreeKill server) v%s\n"
    "Copyright (C) 2025, Qsgs-Fans.\n"
    "License GPLv3: GNU GPL version 3 <https://gnu.org/licenses/gpl.html>.\n"
    "This is free software: you are free to change and redistribute it.\n"
    "There is NO WARRANTY, to the extent permitted by law.\n"
    "\n"
    "Written by Notify-ctrl and others; see\n"
    "<https://github.com/Qsgs-Fans/freekill-asio>.\n",

    FK_VERSION
  );
}

struct cmdConfig {
  uint16_t port = 9527;
};

static bool parse_opt(int argc, char **argv, cmdConfig &cfg) {
  struct option longOptions[] = {
    {"version", no_argument, nullptr, 'v'},
    {"help", no_argument, nullptr, 'h'},
    {"port", required_argument, nullptr, 'p'},
    {nullptr, 0, nullptr, 0}
  };

  int opt;
  while ((opt = getopt_long(argc, argv, "vhp:", longOptions, nullptr)) != -1) {
    switch (opt) {
      case 'v':
        print_version();
        return false;
      case 'h':
        show_usage(argv[0]);
        return false;
      case 'p': {
        int port = std::atoi(optarg);
        if (port < 1024 || port > 65535) {
          // 超出范围，随机端口
          std::srand(static_cast<unsigned int>(std::time(nullptr)));
          port = std::rand() % (65535 - 1024 + 1) + 1024;
        }
        cfg.port = (uint16_t)port;
        break;
      }
      default:
        show_usage(argv[0]);
        return false;
    }
  }

  return true;
}

int main(int argc, char *argv[]) {
  cmdConfig cfg;

  if (!parse_opt(argc, argv, cfg)) {
    return 0;
  }

  pthread_setname_np(pthread_self(), "MainThread");

  initLogger();
  spdlog::info("server is starting");

  asio::io_context io_ctx;
  Server::instance().listen(
    io_ctx,
    tcp::endpoint(tcp::v6(), cfg.port),
    udp::endpoint(udp::v6(), cfg.port)
  );

  shellAlive = true;
  io_ctx.run();
  shellAlive = false;

  // 手动释放确保一切都在控制下
  Server::destroy();
  PackMan::destroy();

  spdlog::shutdown();
}
