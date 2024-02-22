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
  
  // ③ 今回もYieldするだけ。
  // 　 Yieldに失敗した場合は、メッセージの処理の最中にカーネル側で barrier の値が更新された
  // 　 ことを意味する。barrier 値が古すぎるよ、ってこと。
  auto req = enclave()->GetRunRequest(cpu);
  req->LocalYield(barrier, 0);
}

void TutorialScheduler::TaskNew(Task<>* task, const Message& msg) {
  std::cout << msg << std::endl;
}

} // namespace ghost
