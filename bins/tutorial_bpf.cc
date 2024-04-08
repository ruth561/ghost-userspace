#include "schedulers/tutorial/bpf/tut_bpf.skel.h"

#include <iostream>

int main()
{
	struct tutorial_bpf *skel;
	int err;
	
	skel = tutorial_bpf__open();
	if (!skel) {
		std::cerr << "Failed to open eBPF program." << std::endl;
		return -1;
	}

	err = tutorial_bpf__load(skel);
	if (err) {
		std::cerr << "Failed to load eBPF program." << std::endl;
		return -1;
	}

	err = tutorial_bpf__attach(skel);
	if (err) {
		std::cerr << "Failed to attach eBPF program." << std::endl;
		return err;
	}

	// 入力を受け取ったら終了する。
	int n;
	std::cin >> n;

	tutorial_bpf__destroy(skel);

	return 0;
}
