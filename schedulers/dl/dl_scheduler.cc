#include <cstdio>

#include "lib/base.h"
#include "lib/ghost.h"
#include "lib/topology.h"

#include "schedulers/dl/dl_scheduler.h"

namespace ghost {

void DeadlineScheduler::GlobalSchedule(const BarrierToken &agent_barrier) {
  // メッセージの処理
  Message msg;
  while (!(msg = global_channel_.Peek()).empty()) {
    DispatchMessage(msg);
    global_channel_.Consume(msg);
  }

  // トランザクションを行うべきCPUのリスト。
  // タスクの優先度はデッドラインの近さによって求まる。
  // そのため、rq_の先頭とcs->currentを比較し、rq_の先頭のタスクの方が
  // デッドラインが近い場合は、プリエンプションを行う。
  CpuList should_txn = MachineTopology()->EmptyCpuList();
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
    should_txn.Set(cpu);
    next = rq_.Top();
  }

  // トランザクションを行う
  if (!should_txn.Empty()) {
    enclave()->CommitRunRequests(should_txn);
  }

  // コミットの結果を見て処理を行う。
  for (auto cpu: should_txn) {
    DeadlineCpuState* cs = cpu_state(cpu);
    RunRequest* req = enclave()->GetRunRequest(cpu);
    if (req->succeeded()) {
      // コミットに成功した場合
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
    CHECK(task->blocked());
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
  // std::cout << "[ TaskBlocked ]" << std::endl;

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
  // std::cout << "[ TaskPreempted ]" << std::endl;

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

} // namespace ghost
