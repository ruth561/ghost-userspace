#include "lib/ghost.h"
using namespace ghost;

// 少々重めの処理
void BusyLoop() {
  for (int i = 0; i < 1000000000; i++);
}

int main() {
  absl::Time program_start = MonotonicNow(); // プログラム開始時間
  absl::Time t1_start, t1_end, t2_start, t2_end;

  GhostThread t1(GhostThread::KernelScheduler::kGhost, [&]() {
    t1_start = MonotonicNow(); // スレッド1の開始時間
    BusyLoop();
    t1_end = MonotonicNow(); // スレッド1の終了時間
  });
  GhostThread t2(GhostThread::KernelScheduler::kGhost, [&]() {
    t2_start = MonotonicNow(); // スレッド2の開始時間
    BusyLoop();
    t2_end = MonotonicNow(); // スレッド2の終了時間
  });

  t1.Join();
  t2.Join();

  std::cout << "計測結果" << std::endl;
  std::cout << std::endl;
  std::cout << "[ t1 ]    start: " << absl::ToInt64Milliseconds(t1_start - program_start) << " ms" << std::endl;
  std::cout << "            end: " << absl::ToInt64Milliseconds(t1_end   - program_start) << " ms" << std::endl;
  std::cout << "        elapsed: " << absl::ToInt64Milliseconds(t1_end   - t1_start     ) << " ms" << std::endl;
  std::cout << std::endl;
  std::cout << "[ t2 ]    start: " << absl::ToInt64Milliseconds(t2_start - program_start) << " ms" << std::endl;
  std::cout << "            end: " << absl::ToInt64Milliseconds(t2_end   - program_start) << " ms" << std::endl;
  std::cout << "        elapsed: " << absl::ToInt64Milliseconds(t2_end   - t2_start     ) << " ms" << std::endl;
}
