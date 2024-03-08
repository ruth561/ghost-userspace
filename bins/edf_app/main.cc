#include "lib/base.h"
#include "lib/enclave.h"
#include "lib/ghost.h"
#include "lib/topology.h"
#include "schedulers/edf/edf_scheduler.h"
#include "shared/prio_table.h"
#include "bins/edf_app/app.h"

using namespace ghost;


// EDFスケジューラで管理されるスレッドを生成するプロセス
// app.ccで実装される
int EdfProcess(void);

// EDFスケジューラの使い方は、単体で起動するのではなく、
// EDFスケジューラによって管理されたいプログラム群から実行される。
// 処理を簡略化するために、EDFのアプリケーションは別のプロセスで
// 実行することにする➙EdfProcess
int main() {
  // EDFスケジューラを起動する。
  // Centralized型で管理するCPUが2つしかもたないようにしており、
  // 基本的にはCPU0がSatellite CPU、CPU1がGlobal CPUとして動作させる。
  auto topo = ghost::MachineTopology();
  ghost::GlobalConfig cfg(
    topo,
    topo->ParseCpuStr("0,1"),
    topo->cpu(1));
  auto uap = new AgentProcess<GlobalEdfAgent<LocalEnclave>, GlobalConfig>(cfg);

  // EDFスレッド用のプロセスを生成
  ForkedProcess fp(EdfProcess);

  // 対話形式のUIで、RPCを通して手元のプロセスとAgent用プロセスの間で通信を行う。
  // qが入力されたら、プログラム全体を終了させる。
  while (1) {
    std::string s;
    std::cout << "> ";
    std::cin >> s;
    if (s == "q" || s == "quit")
      break;
    try {
      // RPCは数値によって呼び出す処理を指定する。
      int req = std::stoi(s);
      long ret = uap->Rpc(req);
      std::cout << "RpcResponse: " << ret << std::endl;
    } catch (...) {
      std::cerr << "[ Error ] Invalid operation: " << s << std::endl;
    }
  }

  // EDFプロセスを先に終了させ、その後、Agentプロセスを終了させる。
  fp.KillChild(SIGKILL);
  delete uap;

  return 0;
}