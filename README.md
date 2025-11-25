freekill-asio
==============

![](https://img.shields.io/github/repo-size/Qsgs-Fans/freekill-asio?color=green)
![](https://img.shields.io/github/languages/top/Qsgs-Fans/freekill-asio?color=red)
![](https://img.shields.io/github/license/Qsgs-Fans/freekill-asio)
![](https://img.shields.io/github/v/tag/Qsgs-Fans/freekill-asio)
![](https://img.shields.io/github/issues/Qsgs-Fans/freekill-asio)
[![Discord](https://img.shields.io/badge/chat-discord-blue)](https://discord.gg/tp35GrQR6v)
![](https://img.shields.io/github/stars/Qsgs-Fans/freekill-asio?style=social)

freekill-asio是一个移除了Qt依赖的freekill服务端，力求不浪费服务器的性能。

项目文档详见 https://fkbook-all-in-one.readthedocs.io/zh-cn/latest/for-devs/asio/index.html

**已知问题 & TODO**

- [ ] 优化`router->notify`和`router->request`，现版本相当于将消息复制了一遍，实验一下能不能直接分几段sendMessage以规避消息字符串的拷贝
- [ ] `RoomBase::handlePacket`里面开发频率限制功能（操作太快啦~）
- [ ] 在线程安全的前提下，考虑引入（基于asio的）http库实现某些restful API

直接下载运行
--------------

去下载最新Release，选择适合自己的CPU架构（例如 `freekill-asio-static-0.0.1-amd64.tar.gz` ）

然后找一台Linux服务器，将包复制进去后，执行：

```sh
$ tar xf freekill-asio-static-0.0.1-amd64.tar.gz
$ cd freekill-asio-static
$ ./freekill-asio
```

恭喜你顺利运行起了FreeKill服务端。

手动构建运行
--------------

本服务端只支持Linux，且手动构建运行至少需要Debian13。推荐用尽可能新的版本。

### 安装依赖

**Debian 13:**

```sh
$ sudo apt install git g++ cmake pkg-config
$ sudo apt install libasio-dev libssl-dev libcbor-dev nlohmann-json3-dev libsqlite3-dev libgit2-dev libreadline-dev libspdlog-dev
```

其余版本较新的发行版（如Arch、Kali等）安装依赖方式与此大同小异。

freekill-asio并不直接将Lua嵌入到自己执行，而是将Lua作为子进程执行，这需要系统安装了lua5.4。

```sh
$ sudo apt install lua5.4 lua-socket lua-filesystem
```

**Windows (WSL2 + Alpine Linux)**

再介绍windows上基于wsl2和alpine linux的环境搭建方案。Alpine Linux是非常精简的Linux发行版，相比debian能够节省更多资源，当然你也可以用wsl2和debian搭。

```sh
$ apk add git cmake build-base
$ apk add sqlite-dev readline-dev nlohmann-json spdlog-dev boost-dev libgit2-dev libcbor-dev
$ apk add lua5.4 lua5.4-socket lua5.4-filesystem
```

之后的步骤一致，但是可以看到alpine占用的磁盘空间更低。

### 构建

上文安装Lua依赖时已经clone过仓库了，下面需要在仓库的根目录下执行：

```sh
$ mkdir build && cd build
$ cmake ..
$ make
```

### 运行

和Freekill一样，freekill-asio不能直接在build目录下运行，需要在repo目录下运行：

```sh
$ ln -s build/freekill-asio
$ ./freekill-asio
```

这样就启动了freekill-asio服务器，界面与FreeKill类似，但是必须安装freekill-core，否则无法游玩（实际上在原版Freekill服务器撒谎那个这个包也是必须安装的）

```
[freekill-asio v0.0.1] Welcome to CLI. Enter 'help' for usage hints.
fk-asio> install https://gitee.com/Qsgs-Fans/freekill-core
```

然后如同普通的FreeKill服务器一样安装其他需要的包即可。

平台支持
-----------

仅测试过Linux (GCC 10+)

- Linux (Debian 13+, Arch)

若为Release中的静态编译版，则测试过了：

- Linux (Debian 7)

开源许可
-----------

本项目是基于FreeKill的源码重新开发的，依照GPL协议继续使用GPLv3许可。
