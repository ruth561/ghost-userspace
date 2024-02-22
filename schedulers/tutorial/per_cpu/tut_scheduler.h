#pragma once

#include "lib/agent.h"
#include "lib/scheduler.h"


namespace ghost {

class TutorialScheduler : public BasicDispatchScheduler<Task<>> {
public:
  // コンストラクタ
  // Channelの開設を行う
  TutorialScheduler(Enclave *enclave, CpuList cpulist, std::shared_ptr<TaskAllocator<Task<>>> allocator)
      : BasicDispatchScheduler(enclave, std::move(cpulist), std::move(allocator)) {
    // 管理対象CPUに対してそれぞれ必要な処理を行っていく。
    for (auto cpu: cpulist) {
      // ① cpu専用のChannelを作成
      channels_[cpu.id()] = enclave->MakeChannel(GHOST_MAX_QUEUE_ELEMS, 0, MachineTopology()->ToCpuList({cpu}));
      
      // ② 最初に作成したChannelをデフォルトのものとして設定する
      if (!default_channel_)
        default_channel_ = channels_[cpu.id()].get();
    }
  }
	
  // 実装必須
  Channel& GetDefaultChannel() final { return *default_channel_; }
  // 実装任意
  Channel& GetAgentChannel(const Cpu &cpu) override { return *channels_[cpu.id()]; }

protected:
  // コンパイルを通すために実装（中身は無）
  void TaskNew(Task<>* task, const Message& msg) final {}
  void TaskRunnable(Task<>* task, const Message& msg) final {}
  void TaskDeparted(Task<>* task, const Message& msg) final {}
  void TaskDead(Task<>* task, const Message& msg) final {}
  void TaskYield(Task<>* task, const Message& msg) final {}
  void TaskBlocked(Task<>* task, const Message& msg) final {}
  void TaskPreempted(Task<>* task, const Message& msg) final {}

private:
  // Channel用メンバ変数
  // このクラスがChannelのインスタンスのライフタイムを管理する
  std::unique_ptr<Channel> channels_[MAX_CPUS];
  Channel *default_channel_ = nullptr;
};

inline std::unique_ptr<TutorialScheduler> TutorialMultiThreadedScheduler(Enclave *enclave, CpuList cpulist) {
  auto allocator = std::make_shared<ThreadSafeMallocTaskAllocator<Task<>>>();
  auto scheduler = std::make_unique<TutorialScheduler>(enclave, std::move(cpulist), std::move(allocator));
  return scheduler;
}

class TutorialAgent : public LocalAgent {
public:
  TutorialAgent(Enclave *enclave, const Cpu &cpu, TutorialScheduler *scheduler)
    : LocalAgent(enclave, cpu), scheduler_(scheduler) {}
  
  // agentスレッドのメイン部分。
  void AgentThread() override {
    // 最初のこの部分は定形部分。他の全ての初期化の完了を待つ。
    SignalReady();
    WaitForEnclaveReady();
      
    printf("[ Agent Thread %d ] Finish Initialization !\n", gettid());
    
    // ★ agentスレッドが終了するまでひたすらCPUをYieldし続ける。
    while (!Finished()) {
      auto req = enclave_->GetRunRequest(cpu());
      req->LocalYield(status_word().barrier(), 0);
    }
  }

  Scheduler *AgentScheduler() const override { return scheduler_; }

private:
  // スケジューラのインスタンス
  TutorialScheduler *scheduler_;
};

class TutorialFullAgent : public FullAgent<> {
public:
  // コンストラクタ
  // ここで初期化処理を記述する。処理は基本的に以下の3つを順番に実行しておけばよい。
  TutorialFullAgent(AgentConfig &config) : FullAgent(config) {
    scheduler_ = TutorialMultiThreadedScheduler(&enclave_, *enclave_.cpus());
    StartAgentTasks();
    enclave_.Ready();
  }

  // 生成したagentスレッドの後始末を行うように必要な実装
  ~TutorialFullAgent() {
    TerminateAgentTasks();
  }
  
  // RPCで呼び出されるAgentプロセス側の実装
  void RpcHandler(int64_t req, const AgentRpcArgs& args,
                  AgentRpcResponse& response) override { 
    // not implemented
  }
  
  // Agentの派生クラスのインスタンスを生成するメンバ関数。
  std::unique_ptr<Agent> MakeAgent(const Cpu& cpu) override { 
    return std::make_unique<TutorialAgent>(&enclave_, cpu, scheduler_.get()); 
  }

private:
  std::unique_ptr<TutorialScheduler> scheduler_;
};

}; // namespace ghost
