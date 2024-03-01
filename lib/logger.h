// スケジューラの実装で標準出力やファイルにログを書き出すと、
// システムコールを発行する必要があるので重くなる。
// そこでいくつかの共有バッファを用意し、そこにログを書き出していく
// ようにする。ファイルへの書き込みを行う専用のスレッドを用意し、
// 共有バッファの中身をまとめてファイルに書き出すようにする。
//
// Loggerはスレッドごとにシングルトンで実装されているので、
//
//   Logger::GetInstance()->WriteLog("Hello World");
//
// のように使う。ログは改行が付け加えられるので、WriteLogの文字列に
// 改行文字を渡す必要はない。
//
// ログ書き出しはLOGGERマクロによって簡単に行うこともできる。
// このマクロは可変長引数を受け取り、printfのようにフォーマット文字で
// ログを書き出すことができる。例えば、
//
//    LOGGER("Hello %s", "Tarou");
//
// のようにする。
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <ostream>
#include <sched.h>
#include <string>
#include <sys/types.h>
#include <thread>
#include <iostream>

#include "lib/ghost.h"
#include "lib/topology.h"

#define LOGGER(...) Logger::GetInstance().WriteLog(absl::StrFormat(__VA_ARGS__))

namespace ghost {

// agentスレッドとwriterスレッドからアクセスされるバッファを抽象化したクラス。
// LockとUnLockを利用して、排他処理を行う必要がある。
// バッファに一定のログ（kLogLinesNum）が溜まったら、writerスレッドが起床して、
// バッファの中身をファイルに書き出す。
class LogSheet {
  const static size_t kLogLinesNum = 0x1000;
 public:
  LogSheet() { /* puts("LogSheetコンストラクタ"); */  }
  ~LogSheet() { /* puts("LogSheetデストラクタ"); */ }

  // 排他処理用の関数
  void Lock() { mtx_.lock(); }
  bool TryLock() { return mtx_.try_lock(); }
  void UnLock() { mtx_.unlock(); }
  
  // log_lineを書き込む。
  // もしlog_sheetがいっぱいになったらtrueを返す。
  // Lock()によってロックを獲得していないと呼び出してはならない。
  bool WriteLine(std::string logline) {
    log_lines_[idx_] = logline;
    idx_++;
    if (idx_ == kLogLinesNum) {
      // ログをファイルへ書き出すスレッドを起こし、別のCPUで書いてもらう。
      return true;
    }
    return false;
  }

  // このシートが最後であることを示すフラグを立てる
  void SetFin() {
    fin_flag_.store(true, std::memory_order_acquire);
  }

  // 現在バッファに溜まっているログデータをファイルに書き出す
  // 返り値は、このシートが最後かどうかを意味する。
  // trueのときは、このシートが最後なのでWriterは処理を終了する。
  bool Flush(std::ofstream &ofs) {
    // std::cout << "Flush " << idx_ << " log lines" << std::endl;
    for (int i = 0; i < idx_; i++) {
      ofs << log_lines_[i] << std::endl;
    }
    idx_ = 0;
    return fin_flag_.load(std::memory_order_release);
  }

 private:
  std::string log_lines_[kLogLinesNum]; // ログ用バッファ
  size_t idx_ = 0; // 次にログをlog_lines_に書き込む位置
  std::atomic<bool> fin_flag_ = false; // 終了したかを意味するフラグ
  std::mutex mtx_;
};

// スレッドごとにシングルトン
// Logger::GetInstance()によってグローバルなインスタンスにアクセスできる。
// なので基本的にはスレッドセーフ
// そのため、agent同士がログバッファへの書き込みで競合することはない。
class Logger {
  // シートの枚数
  // 3以上じゃないとデッドロックになるので注意
  const static size_t kLogSheetsNum = 10;
 public:
  static Logger &GetInstance() {
    static thread_local Logger logger = Logger();
    return logger;
  }

  ~Logger() {
    // puts("Loggerデストラクタ");
    log_sheets_[agent_current_sheet_].SetFin(); // 現在のシートに終了フラグを立てる
    log_sheets_[agent_current_sheet_].UnLock();
    writer_thread_.join(); // writerスレッドの処理を待つ
  }

  // 現在のシートにログを書き込む
  void WriteLog(std::string logline) {
    LogSheet *log_sheet = &log_sheets_[agent_current_sheet_];
    if (log_sheet->WriteLine(logline)) { // シートがいっぱいになった場合
      // シートを1つ進め、直前のシートのデータはファイルに書き出す。
      AgentNextSheet();
    }
  }
  
 private:
  // writer_thread_cpu_affinity_はハードコーディングしている。
  // ＴＯＤＯ：
  Logger() : writer_thread_cpu_affinity_(MachineTopology()->ParseCpuStr("5")) {
    /* puts("Loggerコンストラクタ"); */

    // agent_tid_をセットする
    agent_tid_ = Gtid::Current().tid();

    // agentは最初に使うシートのロックを獲得しておく
    log_sheets_[agent_current_sheet_].Lock();

    // 書き込み用スレッドを生成する
    // 書き込み用スレッドはスケジューラと干渉しないように、
    // スケジューラの管理対象外のCPUにアフィニティを設定する。
    writer_thread_ = std::thread([this]{
      GhostHelper()->SchedSetAffinity(
        Gtid::Current(), 
        writer_thread_cpu_affinity_);
      WriterThread();
    });
  }

  // シートがいっぱいになったので、次のシートに移動するときに呼ぶ関数
  // agent用
  // ロックの解放とロックの獲得の順序が重要！
  // 
  // agentはロックの獲得に失敗するようなことがあってはならないので、
  // もしロックの獲得に失敗した場合はエラーメッセージを発行する。
  void AgentNextSheet() {
    int agent_next_sheet = (agent_current_sheet_ + 1) % kLogSheetsNum;
    if (!log_sheets_[agent_next_sheet].TryLock()) {
      std::cerr << "agentはLogSheetのロックの獲得に一度失敗しました\n";
      log_sheets_[agent_next_sheet].Lock();
    }
    log_sheets_[agent_current_sheet_].UnLock();
    agent_current_sheet_ = agent_next_sheet;
  }

  // シートがいっぱいになったので、次のシートに移動するときに呼ぶ関数
  // writer用
  // ロックの解放とロックの獲得の順序が重要！
  void WriterNextSheet() {
    int writer_next_sheet = (writer_current_sheet_ + 1) % kLogSheetsNum;
    log_sheets_[writer_next_sheet].Lock();
    log_sheets_[writer_current_sheet_].UnLock();
    writer_current_sheet_ = writer_next_sheet;
  }

  // writerスレッドのボディ部分
  void WriterThread() {
    int mycpu = sched_getcpu();
    CHECK(writer_thread_cpu_affinity_.IsSet(mycpu));
    CpuList cpulist = MachineTopology()->EmptyCpuList();
    GhostHelper()->SchedGetAffinity(Gtid::Current(), cpulist);
    CHECK_EQ(cpulist, writer_thread_cpu_affinity_);

    // ログを書き出すファイルの名前はlog/<agent_tid>.txt
    std::string log_file_name("log/");
    log_file_name += std::to_string(agent_tid_);
    log_file_name += ".txt";
    std::cout << "Write log to the file '" << log_file_name << "'\n";

    // ログファイル用のファイルストリームを開く
    std::ofstream ofs(log_file_name);

    bool is_fin = false; // このフラグがセットされるまで無限ループ
    log_sheets_[writer_current_sheet_].Lock();
    while (!is_fin) {
      // ファイルへの書き出しを行う。
      is_fin = log_sheets_[writer_current_sheet_].Flush(ofs);
      WriterNextSheet(); // UnLock & Lock
    }

    // ファイルを閉じて終了
    ofs.close();
  }

  std::thread writer_thread_; // ファイルへの書き込みを行うスレッド
  int agent_tid_; // agentのtid
  int agent_current_sheet_ = 0; // agentが現在データを書き込み中のシートの番号
  int writer_current_sheet_ = 0; // writerが次にデータを取り出すシートの番号
  LogSheet log_sheets_[kLogSheetsNum];

  // WriterThreadのCPUアフィニティ
  // WriterThreadのCPUアフィニティは、書き込み処理がスケジューラの性能に影響を
  // 与えないように、Enclaveが管理しているCPU以外から選ぶべきである。
  CpuList writer_thread_cpu_affinity_;
};

}; // namespace ghost
