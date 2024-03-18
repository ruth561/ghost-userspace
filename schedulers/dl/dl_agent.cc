#include <string>
#include <vector>

#include "lib/agent.h"
#include "lib/enclave.h"
#include "lib/topology.h"

#include "schedulers/dl/dl_scheduler.h"

using namespace ghost;


int main() {
  // agentプロセスを生成
  // Centralized型で実装するため、2つのCPUのうち片方をagent、もう片方を
  // ghOStスレッドに使う。
  AgentConfig config;
  config.topology_ = MachineTopology();
  config.cpus_ = MachineTopology()->ParseCpuStr("0,1");
  auto uap = new AgentProcess<DeadlineFullAgent, AgentConfig>(config);

  // 対話形式のUIで、RPCを通して手元のプロセスとAgent用プロセスの間で通信を行う。
  while (1) {
    std::string s;
    std::cout << "> ";
    std::cin >> s;
    if (s == "q" || s == "quit")
      break;
    try {
      // RPCは数値によって呼び出す処理を指定する。
      int req = std::stoi(s);
      long ret = uap->Rpc(req);
      std::cout << "RpcResponse: " << ret << std::endl;
    } catch (...) {
      std::cerr << "[ Error ] Invalid operation: " << s << std::endl;
    }
  }
  
  delete uap;
  std::cout << "\nDone!\n";
}
