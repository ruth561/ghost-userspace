#pragma once

#include "lib/agent.h"
#include "lib/ghost.h"
#include "lib/scheduler.h"
#include <cstdio>
#include <deque>
#include <mutex>


namespace ghost {

// タスクの状態を表すenum class
// 簡単に3つの状態のみを扱う。
// タスクの状態
enum class TutorialTaskState {
  kBlocked = 0,
  kQueued,
  kRunning,
};

// TutorialTaskの派生クラス
// タスクの状態に関する情報をメンバ変数に含める
struct TutorialTask : public Task<> {
  // この形式のコンストラクタは実装必須。
  // 基本的には以下のような実装でよい。
  TutorialTask(ghost::Gtid& gtid, ghost_sw_info& sw_info) :
    Task<>(gtid, sw_info) {}

  TutorialTaskState GetState() const { return state_; }

  // タスクの状態変化はこのメンバ関数を必ず介すことにする。
  // タスクの状態変化に関するフック関数を簡単に実装できるようにするため。
  void SetState(TutorialTaskState state) { state_ = state; }

  // 便利な関数
  bool blocked() { return state_ == TutorialTaskState::kBlocked; }
  bool queued() { return state_ == TutorialTaskState::kQueued; }
  bool running() { return state_ == TutorialTaskState::kRunning; }

  int GetCpu() { return cpu_; }
  void SetCpu(int cpu) { cpu_ = cpu; }

 private:
  int cpu_ = -1;
  TutorialTaskState state_ = TutorialTaskState::kBlocked;
};

// タスクの実行可能キューを実装する。
// 基本的にはdeqeueをミューテックスでラップしたもの。
class TutorialRq {
 public:
  TutorialRq() = default;
  TutorialRq(const TutorialRq &) = delete; // コピーの削除
  TutorialRq &operator=(TutorialRq &) = delete; // コピーの削除

  // キューの最後尾にtaskを追加する。
  void PushBack(TutorialTask *task) {
    std::lock_guard<std::mutex> guard(mtx_);
    rq_.push_back(task);
  }

  // キューの先頭にtaskを追加する。
  void PushFront(TutorialTask *task) {
    std::lock_guard<std::mutex> guard(mtx_);
    rq_.push_front(task);
  }

  // キューの先頭を取り出し、その値を返す。
  // もしキューが空であればnullptrを返す。
  TutorialTask *PopFront() {
    std::lock_guard<std::mutex> guard(mtx_);
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
    std::lock_guard<std::mutex> guard(mtx_);
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
    std::lock_guard<std::mutex> guard(mtx_);
    return rq_.size();
  }

  // キューが空か？
  bool Empty() { return Size() == 0; }

  // キューにtaskが含まれているか？
  bool Has(TutorialTask *task) {
    std::lock_guard<std::mutex> guard(mtx_);
    for (auto it = rq_.begin(); it < rq_.end(); it++) {
      if (*it == task) {
        return true;
      }
    }
    return false;
  }

 private:
  std::mutex mtx_;
  std::deque<TutorialTask *> rq_;
};

// CPUごとに管理するデータをまとめたもの。
//  - current: CPUを使用しているghOStタスクへのポインタを格納する。
//             CPUを使用中のタスクがなければnullptrとなる。
//  - agent:   agentスレッド。
//  - channel: CPUに関連付けられたChannelのユニークポインタ。
//  - rq:      実行可能キュー。
struct TutorialCpuState {
  TutorialTask *current = nullptr;
  Agent *agent = nullptr;
  std::unique_ptr<Channel> channel = nullptr;
  TutorialRq rq;
};

class TutorialScheduler : public BasicDispatchScheduler<TutorialTask> {
public:
  // コンストラクタ
  // Channelの開設を行う
  TutorialScheduler(Enclave *enclave, CpuList cpulist, std::shared_ptr<TaskAllocator<TutorialTask>> allocator)
      : BasicDispatchScheduler(enclave, std::move(cpulist), std::move(allocator)) {
    // 管理対象CPUに対してそれぞれ必要な処理を行っていく。
    for (auto cpu: cpulist) {
      TutorialCpuState &cs = cpu_state(cpu);
      // ① cpu専用のChannelを作成
      cs.channel = enclave->MakeChannel(GHOST_MAX_QUEUE_ELEMS, 0, MachineTopology()->ToCpuList({cpu}));
      
      // ② 最初に作成したChannelをデフォルトのものとして設定する
      if (!default_channel_)
        default_channel_ = cs.channel.get();
    }
  }

  // 初期化処理の一番最後に呼び出される関数。
  // この時点でAgentの初期化はほぼ完了していると想定していい。
  // この関数が終了すると、Enclave::Readyは初期化の完了をAgentに伝達する。
  void EnclaveReady() final {
    for (auto cpu: cpus()) {
      TutorialCpuState &cs = cpu_state(cpu);
      cs.agent = enclave()->GetAgent(cpu);
      printf("cs.agent: %p\n", cs.agent);
    }
  }
 
  void Schedule(const Cpu &cpu, const StatusWord &agent_sw);
	
  // 実装必須
  Channel& GetDefaultChannel() final { return *default_channel_; }
  // 実装任意
  Channel& GetAgentChannel(const Cpu &cpu) override { return *cpu_state(cpu).channel; }

protected:
  // コンパイルを通すために実装（中身は無）
  void TaskNew(TutorialTask* task, const Message& msg) final;
  void TaskRunnable(TutorialTask* task, const Message& msg) final;
  void TaskDeparted(TutorialTask* task, const Message& msg) final;
  void TaskDead(TutorialTask* task, const Message& msg) final;
  void TaskYield(TutorialTask* task, const Message& msg) final;
  void TaskBlocked(TutorialTask* task, const Message& msg) final;
  void TaskPreempted(TutorialTask* task, const Message& msg) final;

private:
  // 新しいタスクが追加されたとき、そのタスクを割り振るCPUを選択する関数。
  // 実装はかなり単純化していて、cpus()のなかをぐるぐる周る感じ。
  const Cpu SelectCpu() {
    static auto begin = cpus().begin();
    static auto end = cpus().end();
    static auto next = cpus().end();

    if (end == next)
      next = begin;
    return next++;
  }

  // 現在実行中のCPUを返す。
  // TutorialSchedulerはいろいろなスレッドから呼び出されるので、
  // 今どのagentが実行しているのか、といった情報があるとありがたい。
  int MyCpu() {
    static thread_local int cpu = -1;
    if (cpu < 0)
      cpu = sched_getcpu();
    return cpu;
  }

  TutorialCpuState &cpu_state(int cpu) { return cpu_states_[cpu]; }
  TutorialCpuState &cpu_state(const Cpu &cpu) { return cpu_states_[cpu.id()]; }

  TutorialCpuState cpu_states_[MAX_CPUS];
  Channel *default_channel_ = nullptr;
};

inline std::unique_ptr<TutorialScheduler> TutorialMultiThreadedScheduler(Enclave *enclave, CpuList cpulist) {
  auto allocator = std::make_shared<ThreadSafeMallocTaskAllocator<TutorialTask>>();
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
    
    // agentスレッドはひたすらTutorialScheduler::Scheduleを呼び出し続ける実装。
    while (!Finished()) {
      scheduler_->Schedule(cpu(), status_word());
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
