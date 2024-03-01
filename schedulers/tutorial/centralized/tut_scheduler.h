#pragma once 

#include "kernel/ghost_uapi.h"
#include "lib/agent.h"
#include "lib/enclave.h"
#include "lib/scheduler.h"
#include <atomic>
#include <cstdio>
#include <deque>
#include <memory>


namespace ghost {

enum class TutorialTaskState {
  kBlocked = 0,
  kQueued,
  kRunning,
};

struct TutorialTask : public Task<> {
  TutorialTask(ghost::Gtid& gtid, ghost_sw_info& sw_info) :
    Task<>(gtid, sw_info) {} // 実装必須
  
  int GetCpu() const { return cpu_; }
  void SetCpu(int cpu) { cpu_ = cpu; }
  
  TutorialTaskState GetState() const { return state_; }
  void SetState(TutorialTaskState state) { state_ = state; }

  bool blocked() { return state_ == TutorialTaskState::kBlocked; }
  bool runnable() { return state_ == TutorialTaskState::kQueued; }
  bool oncpu() { return state_ == TutorialTaskState::kRunning; }

private:
  int cpu_ = -1; // CPUが未割り当てのときは-1
  TutorialTaskState state_ = TutorialTaskState::kBlocked;
};

// タスクの実行可能キュー。
// マルチスレッドに対応している必要はない。
class TutorialRq {
 public:
  TutorialRq() = default;
  TutorialRq(const TutorialRq &) = delete; // コピーの削除
  TutorialRq &operator=(TutorialRq &) = delete; // コピーの削除

  // キューの最後尾にtaskを追加する。
  void PushBack(TutorialTask *task) {
    rq_.push_back(task);
  }

  // キューの先頭にtaskを追加する。
  void PushFront(TutorialTask *task) {
    rq_.push_front(task);
  }

  // キューの先頭を取り出し、その値を返す。
  // もしキューが空であればnullptrを返す。
  TutorialTask *PopFront() {
    if (rq_.empty())
      return nullptr;
    auto ret = rq_.front();
    rq_.pop_front();
    return ret;
  }

  // キューからtaskを削除する。
  // 無事にtaskの削除に成功すればtrueを返す。
  // もし、キューにtaskが存在していなければfalseを返す。
  bool Erase(TutorialTask *task) {
    for (auto it = rq_.begin(); it < rq_.end(); it++) {
      if (*it == task) {
        rq_.erase(it);
        return true;
      }
    }
    return false;
  }

  // キューに含まれているタスクの数を返す。
  size_t Size() {
    return rq_.size();
  }

  // キューが空か？
  bool Empty() { return Size() == 0; }

 private:
  std::deque<TutorialTask *> rq_;
};

struct TutorialCpuState {
  TutorialTask *current = nullptr;
  Agent *agent = nullptr;
};

// スケジューラの実装
// Schedulerインスタンスへは複数のagentからアクセスされる可能性があるが、
// Centralized型の場合は、工夫すれば排他処理をなくすことができる。
// ただし、排他処理をなくす代わりに、より軽量なアトミック変数global_cpu_
// を用意する必要はある。
class TutorialScheduler : public BasicDispatchScheduler<TutorialTask> {
 public:
  TutorialScheduler(Enclave *enclave, 
                    CpuList cpulist,
                    std::shared_ptr<TaskAllocator<TutorialTask>> allocator)
    : BasicDispatchScheduler(enclave, std::move(cpulist), std::move(allocator)),
      global_cpu_(cpus()[0].id()), // CPUリストの先頭のCPUをグローバルCPUとして設定する
      global_channel_(GHOST_MAX_QUEUE_ELEMS, 0) {
    CHECK(cpus().IsSet(global_cpu_));
  }

  ~TutorialScheduler() {}

  Channel &GetDefaultChannel() { return global_channel_; }

  void GlobalSchedule(const BarrierToken &agent_barrier);

  void EnclaveReady() {
    for (auto cpu: cpus()) {
      cpu_state(cpu)->agent = enclave()->GetAgent(cpu);
    }
  }

  // global_cpu_を扱うためのメンバ関数
  // agentのbarrierとの操作の順序が重要
  int GetGlobalCpuId() { return global_cpu_.load(std::memory_order_acquire); }
  void SetGlobalCpuId(int next_cpu) {
    global_cpu_.store(next_cpu, std::memory_order_release);
  }

  // 次にglobal_cpu_にするcpuの番号を返す
  // 返されたcpuは、現在使用可能ということになっている。
  Cpu PickNextGlobalCpuId() {
    // CPUをリストの先頭から見ていき、最初に見つけられた使用可能なCPUを
    // 次のグローバルCPUとして設定する。この実装は愚直な実装であり、
    // 本来はハイパースレッディングの相手➙L3キャッシュを共有しているCPU➙
    // NUMAノードを共有しているCPU、といった順に検索をかけるべきである。
  redo:
    for (auto cpu: cpus()) {
      if (Available(cpu)) {
        // グローバルCPUをセットしてからPingを送る。
        SetGlobalCpuId(cpu.id());
        enclave()->GetAgent(cpu)->Ping();
        return cpu;
      }
    }
    goto redo;
  }

  // 指定されたCPUでghOStスレッドをスケジュール可能であればtrueを返す関数。
  // ステータスワードのGHOST_SW_CPU_AVAILABLEフラグがセットされているか、
  // を確認する。
  bool Available(const Cpu &cpu) {
    Agent *agent = enclave()->GetAgent(cpu);
    return agent->cpu_avail();
  }

 protected:
  void TaskNew(TutorialTask* task, const Message& msg) final;
  void TaskRunnable(TutorialTask* task, const Message& msg) final;
  void TaskDeparted(TutorialTask* task, const Message& msg) final;
  void TaskDead(TutorialTask* task, const Message& msg) final;
  void TaskYield(TutorialTask* task, const Message& msg) final;
  void TaskBlocked(TutorialTask* task, const Message& msg) final;
  void TaskPreempted(TutorialTask* task, const Message& msg) final;

 private:

  TutorialCpuState *cpu_state(const Cpu &cpu) { return &cpu_states_[cpu.id()]; }
  TutorialCpuState *cpu_state(int cpu) { return &cpu_states_[cpu]; }
  
  // CPUごとのTutorialCpuStateインスタンス。
  TutorialCpuState cpu_states_[MAX_CPUS];

  // 現在のグローバルCPU。
  // この値に対応するagentがGlobal Agentとなり、スケジューラの処理を行っていく。
  // Global CPUで他のCFSタスクなどが実行可能状態になった場合、スケジューラは
  // Global CPUを他のCPUに移動することでCPUを明け渡すことになっている。
  // Global CPUの切り替わり処理は、PickNextGlobalCpuIdで行われる。
  std::atomic<int> global_cpu_;

  // ただ1つのメッセージキュー。
  // Centralized型の場合はメッセージキューは1つしか使わない。
  LocalChannel global_channel_;

  // 実行可能キュー。ここに入っているタスクは、状態が実行可能状態となる。
  TutorialRq rq_;
};

class TutorialAgent : public LocalAgent {
 public:
  TutorialAgent(Enclave *enclave, const Cpu &cpu, TutorialScheduler *scheduler)
    : LocalAgent(enclave, cpu), global_scheduler_(scheduler) {}
  
  // Agentスレッドのメイン処理部分
  void AgentThread() final {
    // いつもの定型処理
    SignalReady();
    WaitForEnclaveReady();

    // RunRequestは予め取り出しておいてもいい
    auto req = enclave()->GetRunRequest(cpu());
    
    // スケジューラのメイン処理部分
    while (!Finished()) {
      // ループの最初にagentのバリア値を読み出しておく必要がある。
      // （少なくともGetGlobalCpuIdの実行よりは先に！）
      BarrierToken agent_barrier = barrier();

      // 現在のglobal cpuの値を見て、自分がSatellite Agentなのか、
      // Global Agentなのか、を確認し、それぞれの処理を行う。
      if (cpu().id() != global_scheduler_->GetGlobalCpuId()) { // Satellite Agent
        req->LocalYield(agent_barrier, /* flags */ 0);
      } else { // Global Agent
        // 優先度がboostされていれば、GlobalCPUを切り替える。
        // this cpuを使いたがっているタスクにCPUを明け渡し、スケジューラは
        // 別のCPUに移動する、というイメージ。
        if (boosted_priority()) {
          // グローバルCPUを切り替える
          global_scheduler_->PickNextGlobalCpuId();
        } else {
          global_scheduler_->GlobalSchedule(agent_barrier);
        }
      }
    }
  }

  Scheduler *AgentScheduler() const final { return global_scheduler_; }

 private:
  TutorialScheduler *global_scheduler_;
};

class TutorialFullAgent : public FullAgent<> {
 public:
  TutorialFullAgent(AgentConfig &config) : FullAgent(config) {
    auto allocator = std::make_shared<SingleThreadMallocTaskAllocator<TutorialTask>>();
    global_scheduler_ = std::make_unique<TutorialScheduler>(&enclave_, *enclave_.cpus(), std::move(allocator));
    StartAgentTasks();
    enclave_.Ready();
  }

  // GlobalAgentから先にkillする。
  // この実装はFIFO Centralizedの実装を流用している。
  ~TutorialFullAgent() {
    auto global_cpuid = global_scheduler_->GetGlobalCpuId();

    // agents_の先頭がGlobalAgentではなかったとき、GlobalAgentをagents_の先頭にもってくる。
    if (agents_.front()->cpu().id() != global_cpuid) {
      for (auto it = this->agents_.begin(); it != this->agents_.end(); it++) {
        if (((*it)->cpu().id() == global_cpuid)) {
          auto d = std::distance(this->agents_.begin(), it);
          std::iter_swap(this->agents_.begin(), this->agents_.begin() + d);
          break;
        }
      }
    }

    // 先頭はGlobalAgentであることを確認して、終了処理を開始する。
    CHECK_EQ(this->agents_.front()->cpu().id(), global_cpuid);
    this->TerminateAgentTasks();
  }

  void RpcHandler(int64_t req, const AgentRpcArgs& args, AgentRpcResponse& response) override {}

  std::unique_ptr<Agent> MakeAgent(const Cpu &cpu) final {
    return std::make_unique<TutorialAgent>(&enclave_, cpu, global_scheduler_.get());
  }

 private:
  std::unique_ptr<TutorialScheduler> global_scheduler_;
};

} // namespace ghost
