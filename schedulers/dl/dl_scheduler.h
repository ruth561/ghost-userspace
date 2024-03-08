#pragma once

#include "lib/agent.h"
#include "lib/ghost.h"
#include "lib/scheduler.h"
#include "lib/logger.h"
#include <deque>
#include <queue>
#include <vector>


namespace ghost {

// タスクの状態を表すenum class
// 簡単に3つの状態のみを扱う。
// タスクの状態
enum class DeadlineTaskState {
  kBlocked = 0,
  kQueued,
  kRunning,
};

// DeadlineTaskの派生クラス
// タスクの状態に関する情報をメンバ変数に含める
struct DeadlineTask : public Task<> {
  // この形式のコンストラクタは実装必須。
  // 基本的には以下のような実装でよい。
  DeadlineTask(ghost::Gtid& gtid, ghost_sw_info& sw_info) :
    Task<>(gtid, sw_info) {}

  int GetCpu() const { return cpu_; }
  void SetCpu(int cpu) { cpu_ = cpu; }

  DeadlineTaskState GetState() const { return state_; }
  // タスクの状態変化はこのメンバ関数を必ず介すことにする。
  // タスクの状態変化に関するフック関数を簡単に実装できるようにするため。
  void SetState(DeadlineTaskState state) { state_ = state; }

  // 便利な関数
  bool blocked() { return state_ == DeadlineTaskState::kBlocked; }
  bool queued() { return state_ == DeadlineTaskState::kQueued; }
  bool running() { return state_ == DeadlineTaskState::kRunning; }

  // 2つのタスクを比較し、どちらの優先度の方が高いかを返す比較関数。
  // 絶対デッドラインが近い方のタスクを優先度高く評価する。
  bool operator<(const DeadlineTask &rhs) const {
    return deadline < rhs.deadline;
  }

  bool operator>(const DeadlineTask &rhs) const {
    return deadline > rhs.deadline;
  }

  bool operator<=(const DeadlineTask &rhs) const {
    return deadline <= rhs.deadline;
  }

  bool operator>=(const DeadlineTask &rhs) const {
    return deadline >= rhs.deadline;
  }

 private:
  int cpu_ = -1; // CPUが未割り当てのときは-1
  DeadlineTaskState state_ = DeadlineTaskState::kBlocked;

  // runtime: 累計実行時間（ns）
  // elapsed_runtime: 直前の実行の経過時間（ns）
  // estimated_runtime: 1周期ごとに行う処理の実行時間（ns）
  //                    最初に設定される値
  // deadline: 周期実行での相対デッドライン
  // sched_deadline: この時刻までにタスクを実行状態にしないといけない指標
  //                 sched_deadline = deadline - estimated_runtime
  absl::Duration runtime = absl::ZeroDuration();
  absl::Duration elapsed_runtime = absl::ZeroDuration();
  absl::Duration estimated_runtime = absl::Milliseconds(1);
  absl::Duration relative_deadline = absl::Milliseconds(1);
  absl::Duration period = absl::Milliseconds(1);
  absl::Time deadline = absl::InfiniteFuture();
  absl::Time sched_deadline = absl::InfiniteFuture();
};

// Deadlineスケジューラ用の実行可能キュー。
// デッドラインが近いほど優先度が高いとして優先度付きキューでタスクを
// 管理する。
class DeadlineRq {
 public:
  DeadlineRq() = default;
  DeadlineRq(const DeadlineRq &) = delete; // コピーの削除
  DeadlineRq &operator=(DeadlineRq &) = delete; // コピーの削除

  // キューの先頭要素を返す。
  // 先頭要素はキューの中で最もデッドラインが迫っているtaskになる。
  // キューが空のときはnullptrを返す。
  DeadlineTask *Top() {
    if (Empty())
      return nullptr;
    else
      return rq_.front();
  }

  // キューにtaskを追加する。
  void Push(DeadlineTask *task) {
    rq_.push_back(task);
    UpdateRqPosition(Size() - 1); 
  }

  // キューの先頭を取り出し、その値を返す。
  // もしキューが空であればnullptrを返す。
  DeadlineTask *Pop() {
    if (rq_.empty())
      return nullptr;
    auto ret = rq_.front();
    std::swap(rq_.front(), rq_.back());  
    rq_.pop_back();
    UpdateRqPosition(0);
    return ret;
  }

  // キューからtaskを削除する。
  // 無事にtaskの削除に成功すればtrueを返す。
  // もし、キューにtaskが存在していなければfalseを返す。
  // 最初に見つけたtaskのみを削除する（複数のtaskがRqに入っている
  // ことは想定しない）
  bool Erase(DeadlineTask *task) {
    for (int i = 0; i < Size(); i++) {
      if (rq_[i] == task) {
        std::swap(rq_[i], rq_.back());
        rq_.pop_back();
        UpdateRqPosition(i);
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

  // 指定した場所の要素の位置を整える。
  // 優先度付きキューの整合性を保ったまま行う必要がある。
  void UpdateRqPosition(int pos) {
    if (pos >= Size()) {
      return;
    }

    // 上方向
    while (pos > 0) {
      int parent = (pos - 1) / 2;
      if (*rq_[parent] < *rq_[pos]) { // parentの方がdeadlineが近かったら終了
        break;
      }
      std::swap(rq_[parent], rq_[pos]);
      pos = parent;
    }

    // 下方向
    while (pos < Size()) {
      int child1 = 2 * pos + 1;
      int child2 = 2 * pos + 2;
      if (child2 == Size()) { // child1のみ含まれている場合
        if (*rq_[child1] < *rq_[pos]) { // child1の方がdeadlineが近ければ交換
          std::swap(rq_[child1], rq_[pos]);
        }
        break;
      } else if (child2 < Size()) { // どっちも含まれている場合
        int child = child2;
        if (*rq_[child1] < *rq_[child2]) {
          child = child1;
        }
        // childはよりデッドラインが近い方を指す
        if (*rq_[child] < *rq_[pos]) {
          std::swap(rq_[child], rq_[pos]);
          pos = child;
        } else {
          break;
        }
      } else {
        // childがいないので何もせず終了
        break;
      }
    }
  }
  
  std::vector<DeadlineTask *> rq_;
};

struct DeadlineCpuState {
  DeadlineTask *current = nullptr;
  DeadlineTask *next = nullptr;
  Agent *agent = nullptr;
};

// =====================================================================
// TODO: チュートリアルの実装をそのまま流用している！！！↓↓↓↓↓↓↓
// =====================================================================

// スケジューラの実装
// Schedulerインスタンスへは複数のagentからアクセスされる可能性があるが、
// Centralized型の場合は、工夫すれば排他処理をなくすことができる。
// ただし、排他処理をなくす代わりに、より軽量なアトミック変数global_cpu_
// を用意する必要はある。
class DeadlineScheduler : public BasicDispatchScheduler<DeadlineTask> {
 public:
  DeadlineScheduler(Enclave *enclave, 
                    CpuList cpulist,
                    std::shared_ptr<TaskAllocator<DeadlineTask>> allocator)
    : BasicDispatchScheduler(enclave, std::move(cpulist), std::move(allocator)),
      global_cpu_(cpus()[0].id()), // CPUリストの先頭のCPUをグローバルCPUとして設定する
      global_channel_(GHOST_MAX_QUEUE_ELEMS, 0) {
    CHECK(cpus().IsSet(global_cpu_));
  }

  ~DeadlineScheduler() {}

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
  void TaskNew(DeadlineTask* task, const Message& msg) final;
  void TaskRunnable(DeadlineTask* task, const Message& msg) final;
  void TaskDeparted(DeadlineTask* task, const Message& msg) final;
  void TaskDead(DeadlineTask* task, const Message& msg) final;
  void TaskYield(DeadlineTask* task, const Message& msg) final;
  void TaskBlocked(DeadlineTask* task, const Message& msg) final;
  void TaskPreempted(DeadlineTask* task, const Message& msg) final;

 private:

  DeadlineCpuState *cpu_state(const Cpu &cpu) { return &cpu_states_[cpu.id()]; }
  DeadlineCpuState *cpu_state(int cpu) { return &cpu_states_[cpu]; }
  
  // CPUごとのDeadlineCpuStateインスタンス。
  DeadlineCpuState cpu_states_[MAX_CPUS];

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
  DeadlineRq rq_;
};

class DeadlineAgent : public LocalAgent {
 public:
  DeadlineAgent(Enclave *enclave, const Cpu &cpu, DeadlineScheduler *scheduler)
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
  DeadlineScheduler *global_scheduler_;
};

class DeadlineFullAgent : public FullAgent<> {
 public:
  DeadlineFullAgent(AgentConfig &config) : FullAgent(config) {
    auto allocator = std::make_shared<SingleThreadMallocTaskAllocator<DeadlineTask>>();
    global_scheduler_ = std::make_unique<DeadlineScheduler>(&enclave_, *enclave_.cpus(), std::move(allocator));
    StartAgentTasks();
    enclave_.Ready();
  }

  // GlobalAgentから先にkillする。
  // この実装はFIFO Centralizedの実装を流用している。
  ~DeadlineFullAgent() {
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
    return std::make_unique<DeadlineAgent>(&enclave_, cpu, global_scheduler_.get());
  }

 private:
  std::unique_ptr<DeadlineScheduler> global_scheduler_;
};

} // namespace ghost
