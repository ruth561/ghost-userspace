#include "schedulers/tutorial/centralized/tut_scheduler.h"
#include "lib/base.h"
#include "lib/ghost.h"
#include "lib/topology.h"
#include <cstdio>


namespace ghost {

void TutorialScheduler::GlobalSchedule(const BarrierToken &agent_barrier) {
  // メッセージの処理
  Message msg;
  while (!(msg = global_channel_.Peek()).empty()) {
    DispatchMessage(msg);
    global_channel_.Consume(msg);
  }

  // スケジュール可能なCPUを選ぶ。
  CpuList available = MachineTopology()->EmptyCpuList();
  for (const Cpu &cpu: cpus()) {
    // グローバルCPUはグローバルagentが使用中のため、ghOStタスクをスケジュール
    // することはできない。
    if (cpu.id() == GetGlobalCpuId())
      continue;

    // 他のスケジューリングクラスのタスクが存在しているCPUには、ghOStタスクを
    // スケジュールすることはできない。
    if (!Available(cpu))
      continue;
      
    // もし、まだタスクが実行中だったら、タスクを切り替えることはしない。
    // タスクが実行中でない、とは、cs->current==nullptrのときをいう。
    auto cs = cpu_state(cpu);
    if (cs->current) {
      continue;
    }

    available.Set(cpu);
  }

  // availableなCPUそれぞれのトランザクションを作成する。
  CpuList assigned = MachineTopology()->EmptyCpuList();

  // 各CPUのnext taskを選び、トランザクションを発行する。
  while (!available.Empty()) {
    // 実行可能タスクが存在しなければ終了
    if (rq_.Empty())
      break;
    
    // nextタスクを実行可能キューから取り出す。
    TutorialTask *next = rq_.PopFront();
    CHECK(next->runnable());
    CHECK_EQ(next->GetCpu(), -1);

    // nextタスクを割り当てるCPUを選択する。
    const Cpu& next_cpu = available.Front();
    TutorialCpuState* cs = cpu_state(next_cpu);
    CHECK_EQ(cs->current, nullptr);

    // cs->currentにnextをセットし、トランザクションをopenする。
    // ※ トランザクションはまだcommitしない。あとでまとめて行う。
    cs->current = next;
    available.Clear(next_cpu);
    assigned.Set(next_cpu);
    RunRequest* req = enclave()->GetRunRequest(next_cpu);
    req->Open({
      .target = next->gtid,
      .target_barrier = next->seqnum,
      // TODO
      .commit_flags = COMMIT_AT_TXN_COMMIT});
  }

  // まとめてコミットする。
  if (!assigned.Empty()) {
    enclave()->CommitRunRequests(assigned);
  }

  // コミットの結果を見て処理を行う。
  for (const Cpu& next_cpu : assigned) {
    TutorialCpuState* cs = cpu_state(next_cpu);
    RunRequest* req = enclave()->GetRunRequest(next_cpu);
    if (req->succeeded()) {
      // コミットに成功した場合、晴れてcs->currentが実行状態になる。
      cs->current->SetState(TutorialTaskState::kRunning);
      cs->current->SetCpu(next_cpu.id());
    } else {
      // コミットに失敗した場合、cs->currentを実行可能キューに戻す必要がある。
      rq_.PushFront(cs->current);
      cs->current = nullptr;
    }
  }
}

void TutorialScheduler::TaskNew(TutorialTask* task, const Message& msg) {
  std::cout << "[ TaskNew ]" << std::endl;

  // メッセージのペイロード部分にTASK_NEWメッセージ固有の情報が格納されている。
  const ghost_msg_payload_task_new* payload = 
    reinterpret_cast<const ghost_msg_payload_task_new *>(msg.payload());
  // taskのseqnumを更新する（必須）
  task->seqnum = msg.seqnum();
  
  CHECK(task->blocked());

  // タスクの状態がrunnableでTASK_NEWが送られてくることもあるので最初のタスクの状態に注意！
  if (payload->runnable) {
    task->SetState(TutorialTaskState::kQueued);
    rq_.PushBack(task);
  }
}

void TutorialScheduler::TaskRunnable(TutorialTask* task, const Message& msg) {
  std::cout << "[ TaskRunnable ]" << std::endl;

  CHECK(task->blocked());
  task->SetState(TutorialTaskState::kQueued);
  rq_.PushBack(task);
}

void TutorialScheduler::TaskDeparted(TutorialTask* task, const Message& msg) {
  std::cout << "[ TaskDeparted ]" << std::endl;

  if (task->oncpu()) {
    int cpu = task->GetCpu();
    CHECK_GE(cpu, 0);
    auto cs = cpu_state(cpu);
    CHECK_EQ(cs->current, task);
    cs->current = nullptr;
  } else if (task->runnable()) {
    CHECK(rq_.Erase(task));
  } else {
    CHECK(task->blocked());
  }
  allocator()->FreeTask(task);
}

void TutorialScheduler::TaskDead(TutorialTask* task, const Message& msg) {
  std::cout << "[ TaskDead ]" << std::endl;

  CHECK(task->blocked());
  allocator()->FreeTask(task);
}

void TutorialScheduler::TaskYield(TutorialTask* task, const Message& msg) {
  std::cout << "[ TaskYield ]" << std::endl;

  CHECK(task->oncpu());
  int cpu = task->GetCpu();
  CHECK_GE(cpu, 0);
  auto cs = cpu_state(cpu);
  CHECK_EQ(cs->current, task);
  task->SetState(TutorialTaskState::kQueued);
  task->SetCpu(-1);
  rq_.PushBack(task);
  cs->current = nullptr;
}

void TutorialScheduler::TaskBlocked(TutorialTask* task, const Message& msg) {
  std::cout << "[ TaskBlocked ]" << std::endl;

  if (task->oncpu()) {
    int cpu = task->GetCpu();
    CHECK_GE(cpu, 0);
    auto cs = cpu_state(cpu);
    CHECK_EQ(cs->current, task);
    cs->current = nullptr;
  } else if (task->runnable()) {
    CHECK(rq_.Erase(task));
  } else {
    CHECK(false);
  }
  task->SetState(TutorialTaskState::kBlocked);
  task->SetCpu(-1);
}

void TutorialScheduler::TaskPreempted(TutorialTask* task, const Message& msg) {
  std::cout << "[ TaskPreempted ]" << std::endl;

  CHECK(task->oncpu());
  int cpu = task->GetCpu();
  CHECK_GE(cpu, 0);
  auto cs = cpu_state(cpu);
  CHECK_EQ(cs->current, task);
  task->SetState(TutorialTaskState::kQueued);
  task->SetCpu(-1);
  rq_.PushFront(task);
  cs->current = nullptr;
}

} // namespace ghost
