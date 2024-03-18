#include <cstdio>
#include <cstdlib>
#include <iostream>
#include "lib/base.h"
#include "lib/ghost.h"
#include "shared/prio_table.h"
#include "bins/edf_app/app.h"

using namespace ghost;

// 現在、実行中のスレッド
#define CTX_NONE -1
#define CTX_TASK_1_3 0
#define CTX_TASK_3_5 1
int current = -1;

// 実行するスレッドのWorkClass
//  -   kWcPeriodic1per3: 3秒ごとに1秒のジョブが発生する周期タスク
//  -   kWcPeriodic3per5: 5秒ごとに3秒のジョブが発生する周期タスク
enum WorkClass {
  kWcPeriodic1per3, 
  kWcPeriodic3per5, 
  kWcNum
};

// WorkClassの設定をPrioTableに行う関数。
// 最初にEdfProcessから呼び出される。
inline void SetupWorkClasses(PrioTable* table) {
  struct work_class* wc;

  // kWcPeriodic1per3
  // Periodicとしているが、3秒ごとにONESHOTとタスクとして生成されるように
  // 実装した。kWcPeriodic3per5も同様。
  wc = table->work_class(static_cast<int>(WorkClass::kWcPeriodic1per3));
  wc->id = static_cast<int>(WorkClass::kWcPeriodic1per3);
  wc->flags = WORK_CLASS_ONESHOT;
  wc->qos = 1;
  wc->exectime = absl::ToInt64Nanoseconds(absl::Milliseconds(1000));

  // kWcPeriodic3per5
  wc = table->work_class(static_cast<int>(WorkClass::kWcPeriodic3per5));
  wc->id = static_cast<int>(WorkClass::kWcPeriodic3per5);
  wc->flags = WORK_CLASS_ONESHOT;
  wc->qos = 1;
  wc->exectime = absl::ToInt64Nanoseconds(absl::Milliseconds(3000));
}

// 3ミリ秒ごとに1ミリ秒のタスクが行われる。
void Task1per3(PrioTable *table, int sidx) {
  for (int i = 0; i < 5; i++) {
    for (int _ = 0; _ < 1 * (1000 - 150); _++) {
      current = CTX_TASK_1_3;
      SpinFor(absl::Milliseconds(1));
    }
    current = CTX_NONE;
    MarkSchedItemIdle(table, sidx);
    while (!SchedItemRunnable(table, sidx));
  }
}

// 5ミリ秒ごとに3ミリ秒のタスクが行われる。
void Task3per5(PrioTable *table, int sidx) {
  for (int i = 0; i < 3; i++) {
    for (int _ = 0; _ < 3 * (1000 - 150); _++) {
      current = CTX_TASK_3_5;
      SpinFor(absl::Milliseconds(1));
    }
    current = CTX_NONE;
    MarkSchedItemIdle(table, sidx);
    while (!SchedItemRunnable(table, sidx));
  }
}

// EDF用のプロセス
// プロセスとしてPrioTableを1つ用意する
int EdfProcess(void) {
  // PrioTableの作成
  // これはプロセスごとに1つ用意しておく必要がある。
  // ghOStスレッドを作るよりも先にスレッドグループリーダーが作成しておく。
  PrioTable table(2000,
                  static_cast<int>(WorkClass::kWcNum),
                  PrioTable::StreamCapacity::kStreamCapacity19);
  // WorkClassの設定
  SetupWorkClasses(&table);

  // Task1per3をghOStスレッドとして生成する。
  // wakeup_1_3()が実行されるまでBlocked状態のままとなっている。
  int sidx_1_3 = 0;
  auto task_1_3 = GhostThread(GhostThread::KernelScheduler::kGhost, [&]() {
    Task1per3(&table, sidx_1_3);
  });
  // Task1per3を起こす関数。
  auto wakeup_1_3 = [&]() {
    UpdateSchedItem(&table, 
                    sidx_1_3,
                    static_cast<int>(WorkClass::kWcPeriodic1per3), 
                    SCHED_ITEM_RUNNABLE, 
                    task_1_3.gtid(), 
                    absl::Milliseconds(3000)); // Periodicは関係ない 
  };

  // Task3per5をghOStスレッドとして生成する。
  // wakeup_3_5()が実行されるまでBlocked状態のままとなっている。
  int sidx_3_5 = 1;
  auto task_3_5 = GhostThread(GhostThread::KernelScheduler::kGhost, [&]() {
    Task3per5(&table, sidx_3_5);
  });
  // Task3per5を起こす関数。
  auto wakeup_3_5 = [&]() {
    UpdateSchedItem(&table, 
                    sidx_3_5,
                    static_cast<int>(WorkClass::kWcPeriodic3per5), 
                    SCHED_ITEM_RUNNABLE, 
                    task_3_5.gtid(), 
                    absl::Milliseconds(5000)); // Periodicは関係ない
  };
  
  for (int sec = 0; sec < 15; sec++) {
    printf("\n[ %2ds ]\t", sec);
    if (sec % 3 == 0) { // 3秒に1度、Task1per3を起こす
      if (SchedItemRunnable(&table, sidx_1_3)) {
        printf("\n[!] ERROR: Task1per3 reached to DEADLINE!!\n");
      }
      wakeup_1_3();
    }
    if (sec % 5 == 0) { // 5秒に1度、Task3per5を起こす
      if (SchedItemRunnable(&table, sidx_3_5)) {
        printf("[!] ERROR: Task3per5 reached to DEADLINE!!\n");
      }
      wakeup_3_5();
    }

    // 0.1秒ごとに今実行中のタスクを出力し、1秒たったら終了する。
    for (int i = 0; i < 10; i++) {
      absl::SleepFor(absl::Milliseconds(100));
      switch (current) {
        case CTX_NONE:
          printf("NONE, ");
          fflush(stdout);
          break;
        case CTX_TASK_1_3:
          printf(" 1_3, ");
          fflush(stdout);
          break;
        case CTX_TASK_3_5:
          printf(" 3_5, ");
          fflush(stdout);
          break; 
      }
    }
  }

  task_1_3.Join();
  task_3_5.Join();

  return EXIT_SUCCESS;
}