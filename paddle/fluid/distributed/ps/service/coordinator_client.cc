// Copyright (c) 2022 PaddlePaddle Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "paddle/fluid/distributed/ps/service/coordinator_client.h"

#include <memory>
#include <sstream>
#include <string>

#include "paddle/fluid/distributed/ps/service/brpc_ps_client.h"
#include "paddle/fluid/framework/archive.h"
#include "paddle/fluid/string/split.h"

static const int MIN_PORT = 8500;
static const int MAX_PORT = 65535;
DEFINE_uint64(total_fl_client_size, 100, "supported total fl client size");
DEFINE_uint32(coordinator_wait_all_clients_max_time, 60, "uint32: s");

namespace paddle {
namespace distributed {

void CoordinatorService::FlService(
    ::google::protobuf::RpcController* controller,
    const CoordinatorReqMessage* request, CoordinatorResMessage* response,
    ::google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  VLOG(0) << ">>> entering CoordinatorService::FlService";
  response->set_err_code(0);
  response->set_err_msg("");
  brpc::Controller* cntl = static_cast<brpc::Controller*>(controller);
  int32_t msg_type = request->cmd_id();
  uint32_t from_client_id = request->client_id();
  VLOG(0) << "recv client id: " << from_client_id << ", msg_type: " << msg_type;
  std::unique_lock<std::mutex> lck(_mtx);
  auto itr = _service_handle_map.find(msg_type);
  if (itr == _service_handle_map.end()) {
    LOG(ERROR) << "unknown client2coordinator_msg type:" << msg_type;
    return;
  }
  int ret = itr->second(*request, response, cntl);
  lck.unlock();
  if (ret != 0) {
    response->set_err_code(-1);
    response->set_err_msg("handle_client2client_msg failed");
  }
  return;
}

int32_t CoordinatorClient::Initialize(
    const std::vector<std::string>& trainer_endpoints) {
  brpc::ChannelOptions options;
  options.protocol = "baidu_std";
  options.timeout_ms = FLAGS_pserver_timeout_ms;
  options.connection_type = "pooled";
  options.connect_timeout_ms = FLAGS_pserver_connect_timeout_ms;
  options.max_retry = 3;

  std::string server_ip_port;

  // 获取 Pserver 列表，并连接
  if (_env == nullptr) {
    LOG(ERROR) << "_env is null in CoordinatorClient::Initialize()";
    return -1;
  }
  std::vector<PSHost> pserver_list = _env->GetPsServers();

  _pserver_channels.resize(pserver_list.size());
  for (size_t i = 0; i < pserver_list.size(); ++i) {
    server_ip_port.assign(pserver_list[i].ip.c_str());
    server_ip_port.append(":");
    server_ip_port.append(std::to_string(pserver_list[i].port));
    for (size_t j = 0; j < _pserver_channels[i].size(); ++j) {
      _pserver_channels[i][j].reset(new brpc::Channel());
      if (_pserver_channels[i][j]->Init(server_ip_port.c_str(), "", &options) !=
          0) {
        LOG(ERROR) << "CoordinatorClient connect to PServer:" << server_ip_port
                   << " Failed! Try again.";
        std::string int_ip_port =
            GetIntTypeEndpoint(pserver_list[i].ip, pserver_list[i].port);
        if (_pserver_channels[i][j]->Init(int_ip_port.c_str(), "", &options) !=
            0) {
          LOG(ERROR) << "CoordinatorClient connect to PServer:" << int_ip_port
                     << " Failed!";
          return -1;
        }
      }
    }
  }

  // 获取 fl_client 列表，并连接
  std::vector<PSHost> fl_client_list;
  fl_client_list.resize(trainer_endpoints.size());
  if (fl_client_list.empty()) {
    LOG(ERROR) << ">>> fl clients addr info lost";
    return -1;
  }
  for (size_t i = 0; i < trainer_endpoints.size(); i++) {
    std::vector<std::string> addr =
        paddle::string::Split(trainer_endpoints[i], ':');
    fl_client_list[i].ip = addr[0];
    fl_client_list[i].port = std::stol(addr[1]);
    fl_client_list[i].rank = i;  // TO CHECK
  }
  std::string fl_client_ip_port;
  for (size_t i = 0; i < fl_client_list.size(); ++i) {
    fl_client_ip_port.assign(fl_client_list[i].ip);
    fl_client_ip_port.append(":");
    fl_client_ip_port.append(std::to_string(fl_client_list[i].port));
    uint32_t rank = fl_client_list[i].rank;
    VLOG(0) << ">>> coordinator connect to fl_client: " << rank;
    _fl_client_channels[rank].reset(new brpc::Channel());
    if (_fl_client_channels[rank]->Init(fl_client_ip_port.c_str(), "",
                                        &options) != 0) {
      LOG(ERROR) << "CoordinatorClient connect to FlClient:"
                 << fl_client_ip_port << " Failed! Try again.";
      std::string int_ip_port =
          GetIntTypeEndpoint(fl_client_list[i].ip, fl_client_list[i].port);
      if (_fl_client_channels[rank]->Init(int_ip_port.c_str(), "", &options) !=
          0) {
        LOG(ERROR) << "CoordinatorClient connect to PSClient:" << int_ip_port
                   << " Failed!";
        return -1;
      }
    }
  }

  InitTotalFlClientNum(fl_client_list.size());
  _service.InitDefaultFlStrategy();
  return 0;
}

int32_t CoordinatorClient::StartClientService() {
  _service.Initialize();

  _server.AddService(&_service, brpc::SERVER_DOESNT_OWN_SERVICE);
  brpc::ServerOptions options;
  options.num_threads = 1;
  if (_endpoint.empty()) {
    LOG(ERROR) << "Coordinator endpoints not set";
    return -1;
  }
  auto addr = paddle::string::Split(_endpoint, ':');
  std::string ip = addr[0];
  std::string port = addr[1];
  std::string rank = addr[2];
  std::string ip_port = ip + ":" + port;
  if (_server.Start(ip_port.c_str(), &options) != 0) {
    LOG(ERROR) << "CoordinatorServer start failed";
    return -1;
  }
  uint32_t port_ = std::stol(port);
  int32_t rank_ = std::stoi(rank);
  _env->RegisteCoordinatorClient(ip, port_, rank_);
  VLOG(0) << ">>> coordinator service addr: " << ip << ", " << port << ", "
          << _coordinator_id;
  return 0;
}

void CoordinatorClient::SendFlStrategy(const uint32_t& client_id) {
  VLOG(0) << ">>> entering CoordinatorClient::SendFlStrategy! peer client id: "
          << client_id;
  size_t request_call_num = 1;
  FlClientBrpcClosure* closure =
      new FlClientBrpcClosure(request_call_num, [](void* done) {
        auto* closure = reinterpret_cast<FlClientBrpcClosure*>(done);
        int ret = 0;
        if (closure->check_response(0, FL_PUSH_FL_STRATEGY) != 0) {
          LOG(ERROR) << "SendFlStrategy response from coordinator is failed";
          ret = -1;
        }
        closure->set_promise_value(ret);
      });
  auto promise = std::make_shared<std::promise<int32_t>>();
  std::future<int32_t> fut = promise->get_future();
  closure->add_promise(promise);
  closure->request(0)->set_cmd_id(FL_PUSH_FL_STRATEGY);
  closure->request(0)->set_client_id(client_id);
  //
  std::string fl_strategy =
      _service.GetCoordinatorServiceHandlePtr()->_fl_strategy_mp[client_id];
  //
  closure->request(0)->set_str_params(fl_strategy);
  brpc::Channel* rpc_channel = _fl_client_channels[client_id].get();
  if (rpc_channel == nullptr) {
    LOG(ERROR) << "_fl_client_channels is null";
  }
  PsService_Stub rpc_stub(rpc_channel);  // DownpourPsClientService
  rpc_stub.FlService(closure->cntl(0), closure->request(0),
                     closure->response(0), closure);
  fut.wait();
  VLOG(0) << "<<< CoordinatorClient::SendFlStrategy finished";
  return;
}

}  // namespace distributed
}  // namespace paddle
