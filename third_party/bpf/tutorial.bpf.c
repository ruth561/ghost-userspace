#include "kernel/vmlinux_ghost_5_11.h"
#include <libbpf/bpf_helpers.h>
#include <libbpf/bpf_tracing.h>


int cnt = 0;

SEC("kprobe/__x64_sys_execve")
int BPF_KPROBE(kprobe_do_execve, struct filename *filename) {
	bpf_printk("__x64_sys_execve: cnt = %d", cnt++);
	bpf_printk("Hello, World!!");
	return 0;
}

char LICENSE[] SEC("license") = "Dual BSD/GPL";
