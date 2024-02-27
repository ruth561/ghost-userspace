#include "schedulers/tutorial/per_cpu/tut_scheduler.h"
#include "lib/ghost.h"
#include <cstdio>


namespace ghost {

void TutorialScheduler::Schedule(const Cpu &cpu, const StatusWord &agent_sw) {
  auto &cs = cpu_state(cpu);

  // ① メッセージの処理をする前に barrier の値を読み出しておく。
  BarrierToken barrier = agent_sw.barrier();

  // ② キューに来ているメッセージの処理を順番に行っていく。
  // 　 agent に関連付けられた Channel からメッセージを取り出していく。
  // 　 DispatchMessage の内部でメッセージごとの処理が呼び出される。
  Message msg;
  while (!(msg = cs.channel->Peek()).empty()) {
    DispatchMessage(msg);
    Consume(cs.channel.get(), msg);
  }
  
  // ③ スケジューリング
  RunRequest *req = enclave()->GetRunRequest(cpu);
  if (agent_sw.boosted_priority()) {
    // GHOSTタスクより高優先度なタスク（CFSタスクなど）があればCPUを明け渡す
    // RTLA_ON_IDLEを指定することを忘れずに（CPUがIDLE状態になったときにagentを
    // 実行状態にするためのおまじない）！
    req->LocalYield(barrier, RTLA_ON_IDLE);
    return;
  }

  // nextには次に実行状態にするタスクへのポインタを格納する
  TutorialTask *next = nullptr;

  // cs.currentがnullptrでなければ、そのタスクの実行を続ける。
  // cs.currentがnullptrだったときは、cs.rqの先頭のタスクを実行状態にする。
  if (cs.current) {
    next = cs.current;
  } else {
    next = cs.rq.PopFront();
  }

  if (next) { 
    // nextの状態はkQueuedではあるが、cs.rqには含まれていない状態
    CHECK(!cs.rq.Has(next));
    CHECK(next->queued());

    // next候補があればトランザクションを用意
    req->Open({
      .target = next->gtid,
      .target_barrier = next->seqnum,
      .agent_barrier = barrier,
      .commit_flags = COMMIT_AT_TXN_COMMIT,
    });

    // トランザクションを発行
    if (req->Commit()) {
      // succeeded !! 
      next->SetState(TutorialTaskState::kRunning);
      // cs.current == nullptr か cs.current == next のどちらかしか起こりえない。
      cs.current = next;
    } else {
      std::cerr << "Failed to commit txn." << std::endl;
      std::cerr << "txn state: " << RunRequest::StateToString(req->state()) << std::endl;
      // コミットに失敗した場合はもとの状態に戻す。
      if (next->queued()) { // nextがrqに入っていた場合
        cs.rq.PushFront(next);
      } else { // nextが実行中だった場合
        CHECK(next->running());
        CHECK_EQ(next, cs.current);
      }
    }
  } else {
    // next候補がなければCPUを明け渡す（CPUはIDLE状態に）
    req->LocalYield(barrier, 0);
  }
}

void TutorialScheduler::TaskNew(TutorialTask* task, const Message& msg) {
  // メッセージのペイロード部分にTASK_NEWメッセージ固有の情報が格納されている。
  const ghost_msg_payload_task_new* payload = 
    reinterpret_cast<const ghost_msg_payload_task_new *>(msg.payload());
  // taskのseqnumを更新する（必須）
  task->seqnum = msg.seqnum();
  
  // taskを割り当てるCPUを選択する。
  CHECK_EQ(task->GetCpu(), -1);
  auto cpu = SelectCpu();
  // taskをcpuに関連付ける。
  // ここの処理が正常に終了すると、このtaskに関連するメッセージはそのCPUのメッセージキューに
  // 発行されるようになる。
  int status;
  bool result = cpu_state(cpu).channel->AssociateTask(task->gtid, task->seqnum, &status);
  if (!result) {
    // TaskNewでのASSOC_QUEUEが失敗した場合は、いったんデフォルトキューに関連付けを戻しておく。
    printf("[!] Failed to AssociateTask in TaskNew. status = 0x%08x", status);
    cpu = cpus()[0];
  }

  task->SetCpu(cpu.id());
  printf("[*] Task %d is associated with cpu %d\n", task->gtid.tid(), cpu.id());

  auto &cs = cpu_state(cpu);
  // payload->runnableがtrueのときはそのタスクがすでに実行可能状態であるということ、
  // 逆にfalseのときはスリープ状態であることを意味する。
  if (payload->runnable) {
    task->SetState(TutorialTaskState::kQueued);
    cs.rq.PushBack(task);
    cs.agent->Ping();
  }
}

// スリープ状態 -> 実行可能状態
void TutorialScheduler::TaskRunnable(TutorialTask* task, const Message& msg) {
  CHECK_EQ(task->GetCpu(), MyCpu());
  auto &cs = cpu_state(MyCpu());

  CHECK(task->blocked());

  // 実行可能キューの最後尾にtaskを追加
  task->SetState(TutorialTaskState::kQueued);
  cs.rq.PushBack(task);
}

// 実行状態 -> スリープ状態
void TutorialScheduler::TaskBlocked(TutorialTask* task, const Message& msg) {
  CHECK_EQ(task->GetCpu(), MyCpu());
  auto &cs = cpu_state(MyCpu());

  CHECK_EQ(task, cs.current);
  CHECK(task->running());

  // currentをnullptrに変更する
  task->SetState(TutorialTaskState::kBlocked);
  cs.current = nullptr;
}

// 任意の状態 -> 他のスケジューリングクラス
void TutorialScheduler::TaskDeparted(TutorialTask* task, const Message& msg) {
  CHECK_EQ(task->GetCpu(), MyCpu());
  auto &cs = cpu_state(MyCpu());

  if (task->running()) {
    cs.current = nullptr;
  } else if (task->queued()) {
    CHECK(cs.rq.Erase(task));
  }

  // タスクが占めるメモリ領域を解放する
  allocator()->FreeTask(task);
}

// スリープ状態 -> タスクの終了
void TutorialScheduler::TaskDead(TutorialTask* task, const Message& msg) {
  CHECK_EQ(task->GetCpu(), MyCpu());

  CHECK(task->blocked());

  // taskが占めるメモリ領域を解放する
  allocator()->FreeTask(task);
}

// 実行状態 -> 実行可能状態
void TutorialScheduler::TaskYield(TutorialTask* task, const Message& msg) {
  CHECK_EQ(task->GetCpu(), MyCpu());
  auto &cs = cpu_state(MyCpu());

  CHECK_EQ(task, cs.current);
  CHECK(task->running());

  // cs.currentをcs.rqの最後尾に移動する。
  cs.current = nullptr;
  task->SetState(TutorialTaskState::kQueued);
  cs.rq.PushBack(task);
}

// 実行状態 ➙ 実行可能状態
void TutorialScheduler::TaskPreempted(TutorialTask* task, const Message& msg) {
  CHECK_EQ(task->GetCpu(), MyCpu());
  auto &cs = cpu_state(MyCpu());

  CHECK_EQ(task, cs.current);
  CHECK(task->running());

  cs.current = nullptr;
  task->SetState(TutorialTaskState::kQueued);
  cs.rq.PushFront(task);
}

} // namespace ghost
