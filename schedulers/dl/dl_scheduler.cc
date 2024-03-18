#include <cstdio>
#include <ctime>

#include "lib/base.h"
#include "lib/ghost.h"
#include "lib/topology.h"

#include "schedulers/dl/dl_scheduler.h"
#include "shared/prio_table.h"

namespace ghost {

__attribute__((unused))
static void DumpCpuList(const char *name, const CpuList &cpulist) {
  printf("+------------+");
  for (int i = 0; i < cpulist.Size(); i++) {
    printf("----+");
  }
  printf("\n");

  printf("| %10s |", name);
  for (auto cpu: cpulist) {
    printf(" %2d |", cpu.id());
  }
  printf("\n");

  printf("+------------+");
  for (int i = 0; i < cpulist.Size(); i++) {
    printf("----+");
  }
  printf("\n");
}

void DeadlineScheduler::SchedParamsCallback(Orchestrator& orch, 
                                            const SchedParams* sp,
                                            Gtid oldgtid) {

  // 更新が行われたSchedParamに対応するタスクとgtidを取得する。
  auto gtid = sp->GetGtid();
  auto task = allocator()->GetTask(gtid);

  printf("[ SchedParamsCallback ] old_tid = %d, current_tid = %d\n", oldgtid.tid(), gtid.tid());
  
  // メッセージ処理とSchedParamの処理はそれぞれ独立に行われる。
  // そのため、TaskNewメッセージを処理する前にSchedParamの更新処理が行われる
  // 可能性もあり、その場合はまだそのタスクのDeadlineTaskオブジェクトが
  // 作成されていないので、直ちに終了する。
  if (!task) {
    return;
  }

  task->sp = sp;

  if (orch.Repeating(sp)) {
    // 周期タスクの場合
    auto wcid = sp->GetWorkClass();
    struct work_class *wc = orch.prio_table().work_class(wcid);
    printf(" - Repeating\n");
    task->deadline = MonotonicNow() + absl::Nanoseconds(wc->period); // 絶対デッドライン
  } else {
    // One-Shotタスクの場合
    printf(" - One-Shot\n");
    task->deadline = sp->GetDeadline();
  }
  std::cout << " - deadline: " << task->deadline - init_time << std::endl;
  std::cout << " - runtime: " << task->status_word.runtime() << std::endl; 

  // タスクの新しい状態を反映させる。
  // タスクがkBlocked状態のときは、いくらSchedPramの内容が実行可能であるといっても実行可能状態
  // にはなりえない。
  if (task->blocked()) {
    return;
  }
  // タスクの状態はkPaused, kQueued, kRunningのどれか。
  bool has_work = task->sp->HasWork();
  if (has_work) { // タスクはkQueued状態かkRunning状態になる。
    // 状態を遷移させるのは、kPaused状態のときのみでよい。
    if (task->paused()) {
      task->SetState(DeadlineTaskState::kQueued);
      rq_.Push(task);
    }
  } else { // タスクはkPaused状態になる。
    if (task->GetState() == DeadlineTaskState::kQueued) {
      rq_.Erase(task);
    } else if (task->GetState() == DeadlineTaskState::kRunning) {
      cpu_state(task->GetCpu())->current = nullptr;
    } else {
      CHECK(task->paused());
    }
    task->SetState(DeadlineTaskState::kPaused);
  }
}

void DeadlineScheduler::GlobalSchedule(const BarrierToken &agent_barrier) {
  // メッセージの処理
  Message msg;
  while (!(msg = global_channel_.Peek()).empty()) {
    DispatchMessage(msg);
    global_channel_.Consume(msg);
  }

  // 各SchedParamsの値を更新する
  // orchs_の要素はプロセスごとに1つ存在するため、これは現在管理下にある
  // プロセスそれぞれに対して行われる処理である。
  for (auto& scraper : orchs_) {
    scraper.second->RefreshSchedParams(kSchedCallbackFunc);
  }

  // トランザクションを行うべきCPUのリスト。
  // タスクの優先度はデッドラインの近さによって求まる。
  // そのため、rq_の先頭とcs->currentを比較し、rq_の先頭のタスクの方が
  // デッドラインが近い場合は、プリエンプションを行う。
  CpuList txns = MachineTopology()->EmptyCpuList();
  DeadlineTask *next = rq_.Top();
  for (const Cpu &cpu: cpus()) {
    // kQueued状態のタスクが存在しなければ終了。
    if (next == nullptr)
      break;
    // グローバルCPUはグローバルagentが使用中のため、ghOStタスクをスケジュール
    // することはできない。
    if (cpu.id() == GetGlobalCpuId())
      continue;

    // 他のスケジューリングクラスのタスクが存在しているCPUには、ghOStタスクを
    // スケジュールすることはできない。
    // TODO: こういった場合、そのCPUに紐付けられているタスクを他のCPUへ移動させたほうがいい？
    if (!Available(cpu))
      continue;
    
    // もし、まだタスクが実行中だったら、タスクを切り替えることはしない。
    // タスクが実行中でない、とは、cs->current==nullptrのときをいう。
    auto cs = cpu_state(cpu);
    RunRequest* req = enclave()->GetRunRequest(cpu);
    // もし、cs->currentの方がデッドラインが近い場合は、nextはプリエンプト
    // できない。
    if (cs->current && *cs->current <= *next)
      continue;
    // cs->currentが存在しないということは、nextを実行状態にできるということ
    // rq_からnextを取り出す。
    CHECK_EQ(rq_.Pop(), next);
    cs->next = next;
    req->Open({
      .target = next->gtid,
      .target_barrier = next->seqnum,
      .commit_flags = COMMIT_AT_TXN_COMMIT});
    txns.Set(cpu);
    // std::cout << "txns (cpu = " << cpu.id() << ", tid = " << cs->next->gtid.tid() << ")\n";
    next = rq_.Top();
  }

  if (txns.Empty()) {
    return;
  }

  // トランザクションを行うCPUのリストを出力する。
  // DumpCpuList("txns", txns);

  // トランザクションを行う
  enclave()->CommitRunRequests(txns);

  // コミットの結果を見て処理を行う。
  for (auto cpu: txns) {
    DeadlineCpuState* cs = cpu_state(cpu);
    RunRequest* req = enclave()->GetRunRequest(cpu);
    if (req->succeeded()) {
      // コミットに成功した場合
      // std::cout << "Successfully commit txn (cpu = " << cpu.id() << ", tid = " << cs->next->gtid.tid() << ")\n";
      if (cs->current) {
        cs->current->SetState(DeadlineTaskState::kQueued);
        rq_.Push(cs->current);
      }
      cs->next->SetState(DeadlineTaskState::kRunning);
      cs->next->SetCpu(cpu.id());
      cs->current = cs->next;
      cs->next = nullptr;
    } else {
      // コミットに失敗した場合
      // std::cout << "Failed to commit txn (cpu = " << cpu.id() << ", tid = " << cs->next->gtid.tid() << ")\n";
      cs->next->SetState(DeadlineTaskState::kQueued);
      rq_.Push(cs->next);
      cs->next = nullptr;
    }
  }
}

void DeadlineScheduler::TaskNew(DeadlineTask* task, const Message& msg) {
  // std::cout << "[ TaskNew ]" << std::endl;

  // メッセージのペイロード部分にTASK_NEWメッセージ固有の情報が格納されている。
  const ghost_msg_payload_task_new* payload = 
    reinterpret_cast<const ghost_msg_payload_task_new *>(msg.payload());
  // taskのseqnumを更新する（必須）
  task->seqnum = msg.seqnum();
  
  CHECK(task->blocked());

  // タスクの状態がqueuedでTASK_NEWが送られてくることもあるので最初のタスクの状態に注意！
  if (payload->runnable) {
    task->SetState(DeadlineTaskState::kQueued);
    rq_.Push(task);
  }

  // 新しいスレッドグループだった場合はOrchestratorを追加する。
  pid_t tgid = task->gtid.tgid();
  CHECK_GE(tgid, 0);
  if (orchs_.find(tgid) == orchs_.end()) {
    std::cout << "[ Debug ] orchs_.size() = " << orchs_.size() << std::endl;
    auto orch = std::make_unique<Orchestrator>();
    if (!orch->Init(tgid)) {
      // Orchestratorの初期化に失敗した場合。
      // 初期化に失敗するケースとしては、PrioTableが存在していないとき、などが挙げられる。
      printf("[ TaskNew ] Failed to init Orchestrator.. (tgid = %d)\n", tgid);
      // 最初にSchedParamsが変更されるまではダミーのSchedParamを指すことにする。
      static SchedParams dummy_sp;
      task->sp = &dummy_sp;
      return;
    }
    
    // Orchestratorの初期化に成功したので、orchs_にオブジェクトを追加する。
    printf("[ TaskNew ] Successfully init Orchestrator! (tgid = %d)\n", tgid);
    auto pair = std::make_pair(tgid, std::move(orch));
    orchs_.insert(std::move(pair));
  }
}

void DeadlineScheduler::TaskRunnable(DeadlineTask* task, const Message& msg) {
  // std::cout << "[ TaskRunnable ]" << std::endl;

  CHECK(task->blocked());
  task->SetState(DeadlineTaskState::kQueued);
  rq_.Push(task);
}

void DeadlineScheduler::TaskDeparted(DeadlineTask* task, const Message& msg) {
  // std::cout << "[ TaskDeparted ]" << std::endl;

  if (task->running()) {
    int cpu = task->GetCpu();
    CHECK_GE(cpu, 0);
    auto cs = cpu_state(cpu);
    CHECK_EQ(cs->current, task);
    cs->current = nullptr;
  } else if (task->queued()) {
    CHECK(rq_.Erase(task));
  } else {
    CHECK(task->blocked() || task->paused());
  }
  allocator()->FreeTask(task);
}

void DeadlineScheduler::TaskDead(DeadlineTask* task, const Message& msg) {
  // std::cout << "[ TaskDead ]" << std::endl;

  CHECK(task->blocked());
  allocator()->FreeTask(task);
}

void DeadlineScheduler::TaskYield(DeadlineTask* task, const Message& msg) {
  // std::cout << "[ TaskYield ]" << std::endl;

  // ＴＯＤＯ：DLにおけるYieldの処理を実装する
  CHECK(task->running());
  int cpu = task->GetCpu();
  CHECK_GE(cpu, 0);
  auto cs = cpu_state(cpu);
  CHECK_EQ(cs->current, task);
  task->SetState(DeadlineTaskState::kQueued);
  task->SetCpu(-1);
  rq_.Push(task);
  cs->current = nullptr;
}

void DeadlineScheduler::TaskBlocked(DeadlineTask* task, const Message& msg) {
  std::cout << "[+] TaskBlocked" << std::endl;

  // ＴＯＤＯ：未検証
  if (task->running()) {
    int cpu = task->GetCpu();
    CHECK_GE(cpu, 0);
    auto cs = cpu_state(cpu);
    CHECK_EQ(cs->current, task);
    cs->current = nullptr;
  } else if (task->queued()) {
    CHECK(rq_.Erase(task));
  } else {
    CHECK(false);
  }
  task->SetState(DeadlineTaskState::kBlocked);
  task->SetCpu(-1);
}

void DeadlineScheduler::TaskPreempted(DeadlineTask* task, const Message& msg) {
  // std::cout << "[+] TaskPreempted" << std::endl;

  if (task->running()) {
    int cpu = task->GetCpu();
    CHECK_GE(cpu, 0);
    auto cs = cpu_state(cpu);
    CHECK_EQ(cs->current, task);
    task->SetState(DeadlineTaskState::kQueued);
    task->SetCpu(-1);
    rq_.Push(task);
    cs->current = nullptr;
  } else {
    CHECK(task->queued() || task->paused());
  }
}

} // namespace ghost
