#pragma once

#include "lib/agent.h"


namespace ghost {

class TutorialFullAgent : public FullAgent<> {
public:
  TutorialFullAgent(AgentConfig &config) : FullAgent(config) {
    // not implemented
  }
  
  // RPCで呼び出されるAgentプロセス側の実装
  void RpcHandler(int64_t req, const AgentRpcArgs& args,
                  AgentRpcResponse& response) override { 
    // not implemented
  }
  
  // 純粋仮想関数なので実装が必要。
  std::unique_ptr<Agent> MakeAgent(const Cpu& cpu) override {
    // not implemented
    return nullptr;
  }
};

}; // namespace ghost
