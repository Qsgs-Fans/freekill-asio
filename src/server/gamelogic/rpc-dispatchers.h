// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "server/rpc-lua/jsonrpc.h"
#include <nlohmann/json.hpp>

class ServerPlayer;

namespace RpcDispatchers {

extern nlohmann::json getPlayerObject(ServerPlayer &p);

extern const JsonRpc::RpcMethodMap ServerRpcMethods;

}
