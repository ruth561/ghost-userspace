#include "lib/enclave.h"
#include "lib/agent.h"

#include "schedulers/tutorial/centralized/tut_scheduler.h"

using namespace ghost;


int main() {
  AgentConfig config;
  config.cpus_ = MachineTopology()->ParseCpuStr("0-3");
  config.topology_ = MachineTopology();

  auto uap = new AgentProcess<TutorialFullAgent, AgentConfig>(config);

  while (1) {
    std::string s;
    std::cout << "> ";
    std::cin >> s;
    if (s == "q" || s == "quit")
      break;
    try {
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
