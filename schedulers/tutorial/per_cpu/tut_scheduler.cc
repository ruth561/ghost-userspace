#include "schedulers/tutorial/per_cpu/tut_scheduler.h"


namespace ghost {

void TutorialScheduler::Schedule(const Cpu &cpu, const StatusWord &agent_sw) {
  // ① メッセージの処理をする前に barrier の値を読み出しておく。
  BarrierToken barrier = agent_sw.barrier();

  // ② キューに来ているメッセージの処理を順番に行っていく。
  // 　 agent に関連付けられた Channel からメッセージを取り出していく。
  // 　 DispatchMessage の内部でメッセージごとの処理が呼び出される。
  Message msg;
  while (!(msg = channels_[cpu.id()]->Peek()).empty()) {
    DispatchMessage(msg);
    Consume(channels_[cpu.id()].get(), msg);
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
  // ◎ メッセージ処理のときにTaskNew()メンバ関数などでrq_にタスクが
  //  　プッシュされているはず
  if (!rq_.empty()) {
    next = rq_.front();
  }

  if (next) { 
    CHECK(!next->blocked());

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
    } else {
      std::cerr << "Failed to commit txn." << std::endl;
      std::cerr << "txn state: " << RunRequest::StateToString(req->state()) << std::endl;
    }
  } else {
    // next候補がなければCPUを明け渡す（CPUはIDLE状態に）
    req->LocalYield(barrier, 0);
  }
}

void TutorialScheduler::TaskNew(TutorialTask* task, const Message& msg) {
  std::cout << msg << std::endl;
    // メッセージのペイロード部分にTASK_NEWメッセージ固有の情報が格納されている。
  const ghost_msg_payload_task_new* payload = 
    reinterpret_cast<const ghost_msg_payload_task_new *>(msg.payload());
  // taskのseqnumを更新する（必須）
  task->seqnum = msg.seqnum();
  
  // payload->runnableがtrueのときはそのタスクがすでに実行可能状態であるということ、
  // 逆にfalseのときはスリープ状態であることを意味する。
  if (payload->runnable) {
    task->SetState(TutorialTaskState::kQueued);
    rq_.push_back(task);
  }
}

// スリープ状態 -> 実行可能状態
void TutorialScheduler::TaskRunnable(TutorialTask* task, const Message& msg) {
  CHECK(task->blocked());

  // 実行可能キューの最後尾にtaskを追加
  task->SetState(TutorialTaskState::kQueued);
  rq_.push_back(task);
}

// 実行状態 -> スリープ状態
void TutorialScheduler::TaskBlocked(TutorialTask* task, const Message& msg) {
  CHECK(!rq_.empty());
  CHECK_EQ(task, rq_.front()); // 現在実行状態のタスクはrq_の先頭にいるはず
  CHECK(task->running());

  task->SetState(TutorialTaskState::kBlocked);
  rq_.pop_front();
}

// 任意の状態 -> 他のスケジューリングクラス
void TutorialScheduler::TaskDeparted(TutorialTask* task, const Message& msg) {
  // rq_の中にtaskがある場合はrq_から取り除く
  rq_.erase(std::remove(
    rq_.begin(), 
    rq_.end(), 
    task), rq_.end());

  // タスクが占めるメモリ領域を解放する
  allocator()->FreeTask(task);
}

// スリープ状態 -> タスクの終了
void TutorialScheduler::TaskDead(TutorialTask* task, const Message& msg) {
  CHECK(task->blocked());

  // taskが占めるメモリ領域を解放する
  allocator()->FreeTask(task);
}

// 実行状態 -> 実行可能状態
void TutorialScheduler::TaskYield(TutorialTask* task, const Message& msg) {
  CHECK(!rq_.empty());
  CHECK_EQ(task, rq_.front());
  CHECK(task->running());

  // rq_の先頭のタスクをrq_の最後尾に移動する
  task->SetState(TutorialTaskState::kQueued);
  rq_.pop_front();
  rq_.push_back(task);
}

// 実行状態 ➙ 実行可能状態
void TutorialScheduler::TaskPreempted(TutorialTask* task, const Message& msg) {
  CHECK(!rq_.empty());
  CHECK_EQ(task, rq_.front());
  CHECK(task->running());

  task->SetState(TutorialTaskState::kQueued);
}

} // namespace ghost
