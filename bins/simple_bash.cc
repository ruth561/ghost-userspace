#include "lib/ghost.h"
using namespace ghost;

int main() {
  // コンストラクタの第1引数には、スケジューリングポリシーを指定する。
  // ここで指定できるのは、以下の2つである。
  //     - GhostThread::KernelScheduler::kCfs
  //     - GhostThread::KernelScheduler::kGhost
  // 第2引数には、新しいスレッドとして実行したい関数を指定する。
  GhostThread t(GhostThread::KernelScheduler::kGhost, [] {   
    system("/usr/bin/bash");
  });

  // 上記のスレッドが終了するまで待機（通常の thread と同じ）
  t.Join();
}
