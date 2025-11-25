#!/bin/sh
set -e  # 遇到任何错误立即退出

# -------------------------------
# 配置参数
# -------------------------------
LUA_VERSION="5.4.8"
ROOT_DIR="/root"
NPROC=$(nproc || echo 4)

echo '[+] 设置构建环境'

# -------------------------------
# 安装系统依赖
# -------------------------------
echo '[+] 安装系统依赖'
apk add git vim cmake build-base ca-certificates \
  sqlite-dev readline-dev nlohmann-json spdlog-dev boost-dev openssl-dev \
  readline-static ncurses-static sqlite-static \
  openssl-libs-static zlib-static

# -------------------------------
# 编译安装 libgit2（静态库）
# -------------------------------
if [ ! -d "$ROOT_DIR/libgit2" ]; then
  echo '[+] 克隆 libgit2'
  cd "$ROOT_DIR"
  git clone https://github.com/libgit2/libgit2.git --depth 1 "$ROOT_DIR/libgit2"
fi

if [ ! -f "$ROOT_DIR/libgit2/build/libgit2.a" ]; then
  echo '[+] 编译安装 libgit2（静态库）'
  cd "$ROOT_DIR/libgit2"
  mkdir -p build && cd build
  cmake .. -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTS=OFF -DBUILD_CLI=OFF
  make -j$NPROC
  make install
  cd "$ROOT_DIR"
else
  echo '[*] libgit2 已编译，跳过...'
fi

# -------------------------------
# 编译安装 libcbor（静态库）
# -------------------------------
if [ ! -d "$ROOT_DIR/libcbor" ]; then
  echo '[+] 克隆 libcbor'
  cd "$ROOT_DIR"
  git clone https://github.com/PJK/libcbor.git
fi

if [ ! -f "$ROOT_DIR/libcbor/build/src/libcbor.a" ]; then
  echo '[+] 编译安装 libcbor'
  cd "$ROOT_DIR/libcbor"
  mkdir -p build && cd build
  cmake ..
  make -j$NPROC
  make install
  cd "$ROOT_DIR"
else
  echo '[*] libcbor 已编译，跳过...'
fi

# -------------------------------
# 编译 Lua 解释器（静态）
# -------------------------------
LUA_DIR="$ROOT_DIR/lua-$LUA_VERSION"
if [ ! -f "$LUA_DIR/src/liblua.a" ]; then
  if [ ! -d "$LUA_DIR" ]; then
    echo '[+] 下载 Lua 源码'
    cd "$ROOT_DIR"
    wget "https://www.lua.org/ftp/lua-$LUA_VERSION.tar.gz"
    tar xf "lua-$LUA_VERSION.tar.gz"
    rm "lua-$LUA_VERSION.tar.gz"
  fi

  echo '[+] 编译静态 Lua 5.4'
  cd "$LUA_DIR"
  make clean
  make linux MYLDFLAGS="-static -Wl,--gc-sections" MYCFLAGS="-fPIC" -j$NPROC
  make install
  cd "$ROOT_DIR"
else
  echo '[*] Lua 已编译，跳过...'
fi

# -------------------------------
# 编译 luasocket（静态库）
# -------------------------------
LUASOCKET_DIR="$LUA_DIR/luasocket"
if [ ! -d "$LUASOCKET_DIR" ]; then
  echo '[+] 克隆 luasocket'
  cd "$LUA_DIR"
  git clone https://github.com/lunarmodules/luasocket
fi

SOCKET_A="$LUASOCKET_DIR/src/socket.a"
MIME_A="$LUASOCKET_DIR/src/mime.a"
if [ ! -f "$SOCKET_A" ] || [ ! -f "$MIME_A" ]; then
  echo '[+] 编译 luasocket 静态库'
  cd "$LUASOCKET_DIR"
  # 修改 Makefile：生成 .a 而非 .so
  sed -i 's/^all:.*/all: socket.a mime.a/' src/makefile

  # 添加静态库打包规则（兼容 busybox sed）
  cat >> src/makefile << 'EOF'

socket.a: $(SOCKET_OBJS)
	$(AR) rcs socket.a $(SOCKET_OBJS)

mime.a: $(MIME_OBJS)
	$(AR) rcs mime.a $(MIME_OBJS)
EOF

  make LUAV=5.4 clean
  make LUAV=5.4 -j$NRPOC
  cd "$LUA_DIR"
else
  echo '[*] luasocket 已编译，跳过...'
fi

# -------------------------------
# 编译 luafilesystem（静态库）
# -------------------------------
LFS_DIR="$LUA_DIR/luafilesystem"
LFS_A="$LFS_DIR/src/lfs.a"
if [ ! -d "$LFS_DIR" ]; then
  echo '[+] 克隆 luafilesystem'
  cd "$LUA_DIR"
  git clone https://github.com/lunarmodules/luafilesystem
fi

if [ ! -f "$LFS_A" ]; then
  echo '[+] 编译 luafilesystem 静态库'
  cd "$LFS_DIR/src"
  gcc -c lfs.c -o lfs.o -I"$LUA_DIR/src"
  ar rcs lfs.a lfs.o
  cd "$LUA_DIR"
else
  echo '[*] luafilesystem 已编译，跳过...'
fi

# -------------------------------
# 静态链接 Lua 解释器（带模块）
# -------------------------------
echo '[+] 静态链接嵌入模块的 Lua 解释器'
cd "$LUA_DIR"
cp src/lua.c src/lua_patched.c

#printf '%s\n' \
#  'int luaopen_lfs(lua_State *L);' \
#  'int luaopen_socket_core(lua_State *L);' \
#  'int luaopen_mime_core(lua_State *L);' > /tmp/lua_inject_headers.txt
#sed -i '0i /tmp/lua_inject_headers.txt' src/lua_patched.c
sed -i '/^static/ i\
int luaopen_lfs(lua_State *L);\
int luaopen_socket_core(lua_State *L);\
int luaopen_mime_core(lua_State *L);\
' src/lua_patched.c

printf '%s\n' \
  '  luaL_requiref(L, "lfs", luaopen_lfs, 1);' \
  '  lua_pop(L, 1);' \
  '  luaL_requiref(L, "socket.core", luaopen_socket_core, 1);' \
  '  lua_pop(L, 1);' \
  '  luaL_requiref(L, "mime.core", luaopen_mime_core, 1);' \
  '  lua_pop(L, 1);' > /tmp/lua_inject.txt
sed -i '/luaL_openlibs(L);/r /tmp/lua_inject.txt' src/lua_patched.c

# 链接静态库
gcc src/lua_patched.c -o lua \
  -Isrc \
  -Lsrc -llua \
  "$LUASOCKET_DIR/src/socket.a" \
  "$LUASOCKET_DIR/src/mime.a" \
  "$LFS_DIR/src/lfs.a" \
  -static -Wl,--gc-sections \
  -lm -ldl

# -------------------------------
# 编译 freekill-asio（静态）
# -------------------------------
if [ ! -d "$ROOT_DIR/freekill-asio" ]; then
  echo '[+] 克隆 freekill-asio'
  cd "$ROOT_DIR"
  git clone https://github.com/Qsgs-Fans/freekill-asio
fi

if [ ! -f "$ROOT_DIR/freekill-asio/build/freekill-asio" ]; then
  echo '[+] 静态编译 freekill-asio'
  cd "$ROOT_DIR/freekill-asio"
  mkdir -p build && cd build
  cmake .. --toolchain=../distro/static-build/alpine_static.cmake 2&>/dev/null || true
  cmake .. --toolchain=../distro/static-build/alpine_static.cmake || true
  make -j$NPROC
  cd "$ROOT_DIR"
else
  echo '[*] freekill-asio 已编译，跳过...'
fi

# -------------------------------
# 打包发布
# -------------------------------
echo '[+] 打包为 freekill-asio-static.tar.gz'

DIST_DIR="$ROOT_DIR/freekill-asio-static"
rm -rf "$DIST_DIR"
mkdir -p "$DIST_DIR"

cd "$DIST_DIR"

cp -r "$ROOT_DIR/freekill-asio/packages" .
rm -f packages/.gitignore

cp -r "$ROOT_DIR/freekill-asio/server" .
cp -r "$ROOT_DIR/freekill-asio/freekill.server.config.json.example" .

# 复制 CA 证书
mkdir -p certs
cp -rL /etc/ssl/certs/* certs/ 2>/dev/null || true

# 创建 bin 目录并复制二进制
mkdir -p bin
cp "$ROOT_DIR/freekill-asio/build/freekill-asio" bin/
cp "$LUA_DIR/lua" bin/lua5.4

strip bin/freekill-asio
strip bin/lua5.4

# 复制 Lua 脚本
mkdir -p luasocket
cp "$LUASOCKET_DIR/src/"*.lua luasocket/

# 创建启动脚本
cat << 'EOF' > freekill-asio
#!/bin/sh

# 令freekill-asio在execlp时能找到lua5.4
export PATH=$(pwd)/bin:${PATH}

# 令lua5.4能正确require "socket"
# 我也不知道为什么这里不需要写一个?.lua跟在后面 但总之这样就能跑 不能跑的话就补个?.lua
export LUA_PATH=";;$(pwd)/luasocket/"

freekill-asio $@
EOF

chmod +x freekill-asio

# 打包
cd "$ROOT_DIR"
tar -czf freekill-asio-static.tar.gz freekill-asio-static/

echo '[*] 构建完成！输出文件：freekill-asio-static.tar.gz'
