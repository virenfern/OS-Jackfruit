# Container-Runtime
![Language](https://img.shields.io/badge/Language-C-blue?style=for-the-badge&logo=c&logoColor=white)



A lightweight Docker-like container runtime built from scratch in C. Runs isolated containers using Linux namespaces, captures output through a bounded-buffer logging pipeline, enforces memory limits via a kernel module, and exposes a supervisor CLI.

---

## 1. Team Information

| Name | SRN |
|------|-----|
| Viren James Fernandes | PES2UG24CS590|
| Divyanshu Jha| PES2UG24CS915|

---

## 2. Build, Load, and Run Instructions

### Dependencies

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

### Prepare Root Filesystem

```bash
cd Container-Runtime
mkdir rootfs
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs
```

### Build

```bash
cd boilerplate
make
```

This builds `engine`, `monitor.ko`, `memory_hog`, `cpu_hog`, and `io_pulse`.

# 1. Prep and Build
# Move to the project root and prep workloads
```bash
cd ~/OS-Jackfruit/boilerplate
sudo make clean && make
cp cpu_hog io_pulse memory_hog ../rootfs/
```

# 2. Kernel and Supervisor (Terminal 1) 
# Load the monitor and start the supervisor
```bash
sudo insmod monitor.ko
# Verify device creation
ls -l /dev/container_monitor
# Launch supervisor (pointing to rootfs one level up)
sudo ./engine supervisor ../rootfs
```
#  3. Basic CLI Operations (Terminal 2)
# Run these while Terminal 1 is active
```bash
sudo ./engine start alpha ../rootfs /bin/hostname
sudo ./engine ps
sudo ./engine logs alpha
sudo ./engine stop alpha
```
# 4. TASK 5: Scheduling Experiments

# Experiment A: Nice Value Competition (Priority)
```
time sudo ./engine run cpu_normal ../rootfs /cpu_hog 10 --nice 0 & \
time sudo ./engine run cpu_nice ../rootfs /cpu_hog 10 --nice 15 &
wait
```

# Experiment B: CPU-Bound vs I/O-Bound
```
time sudo ./engine run cpu_exp ../rootfs /cpu_hog 10 --nice 0 & \
time sudo ./engine run io_exp ../rootfs /io_pulse 20 200 &
wait
```
#Divyanshu
# 5. TASK 6: Memory Limit Enforcement
# Soft limit 3MB, Hard limit 6MB
```
sudo ./engine start memtest ../rootfs /memory_hog 1 500 --soft-mib 3 --hard-mib 6
# Check kernel logs for the kill event
sudo dmesg | tail -n 20
```
# 6. Clean Teardown
# Stop supervisor with Ctrl+C in Terminal 1, then:
```bash
sudo rmmod monitor
sudo rm -f /tmp/mini_runtime.sock
```
## 3. Demo Screenshots

### Screenshot 1 — Multi-container supervision
Two containers (alpha, beta) running under a single supervisor process alongside previously tracked containers.

![screenshot1](screenshots/1.png)

### Screenshot 2 — Metadata tracking
`ps` output showing container ID, state, host PID, and start time for all tracked containers.

![screenshot2](screenshots/2.png)

### Screenshot 3 — Bounded-buffer logging
Log file contents captured through the pipe → bounded buffer → logging thread pipeline. Container hostname output routed to its log file.

![screenshot3](screenshots/3.png)

### Screenshot 4 — CLI and IPC
`stop` command issued from the CLI client, routed to the supervisor over a UNIX domain socket, supervisor responds and updates state.

![screenshot4](screenshots/4.png)

### Screenshot 5 — Soft-limit warning
`dmesg` output showing the kernel module emitting a SOFT LIMIT warning when the container's RSS exceeded 3MB.

![screenshot5](screenshots/5.png)

### Screenshot 5 — Hard-limit enforcement
`dmesg` output showing the kernel module killing the container when RSS exceeded 6MB. Supervisor metadata updates state to `killed`.

![screenshot5](screenshots/5.png)

### Screenshot 7 — Scheduling experiment
Two CPU-bound containers run simultaneously with nice=0 and nice=15. The lower-priority container took 2x longer to complete the same workload.

![screenshot7](screenshots/7.png)

### Screenshot 8 — Clean teardown
Supervisor exits cleanly on SIGINT. No zombie processes remain. Container states reflect final exit conditions.

![screenshot8](screenshots/8.png)

---

## 4. Engineering Analysis

### 4.1 Isolation Mechanisms

Isolation is enforced via clone() flags like CLONE_NEWPID and CLONE_NEWNS. The OS works this way because namespaces allow the kernel to provide different "views" of system resources to different processes. Our project exercises this by ensuring a container cannot see the host's process tree or modify the host's filesystem.

### 4.2 Supervisor and Process Lifecycle

The long-running supervisor is essential because it maintains the authoritative state of all containers. Without a persistent parent, there is no process to reap children (causing zombies), no place to store metadata, and no endpoint for the CLI to talk to.

Each container is created via `clone()` making the supervisor its direct parent. When a container exits, the kernel delivers `SIGCHLD` to the supervisor. The `sigchld_handler` calls `waitpid(-1, &status, WNOHANG)` in a loop to reap all exited children and update their metadata. The `stop_requested` flag distinguishes a graceful stop (supervisor sent SIGTERM) from a hard limit kill (kernel sent SIGKILL) from a natural exit.

### 4.3 IPC, Threads, and Synchronization

The runtime uses two IPC mechanisms. A pipe per container carries stdout/stderr from the container to a `pipe_reader_thread` in the supervisor. A UNIX domain socket (`/tmp/mini_runtime.sock`) carries CLI commands and responses between the client process and the supervisor.

The bounded buffer sits between the pipe reader threads (producers) and the logging thread (consumer). Without synchronization, producers and the consumer would race on `head`, `tail`, and `count`, causing lost data or corruption. A `pthread_mutex` protects all accesses to the buffer struct. Two condition variables — `not_empty` and `not_full` — allow producers to block when the buffer is full and the consumer to block when it is empty, avoiding busy-waiting. The container metadata list is protected by a separate `metadata_lock` mutex, kept distinct from the buffer lock to avoid deadlock between the logging path and the CLI command path.

### 4.4 Memory Management and Enforcement

RSS (Resident Set Size) measures the physical memory currently mapped and present in RAM for a process. It does not include swapped-out pages, shared libraries counted once across processes, or memory that has been allocated but not yet touched (due to lazy allocation). RSS is therefore a conservative but practical measure of actual memory pressure a process is causing.

Soft and hard limits serve different purposes. The soft limit is a warning threshold — it signals that a container is approaching its budget without terminating it, giving the runtime or operator a chance to react. The hard limit is a hard enforcement boundary — the process is killed when it crosses it. Enforcement belongs in kernel space because user-space monitoring is inherently racy: by the time a user-space monitor reads RSS and decides to kill a process, the process could have allocated significantly more memory. The kernel module's timer fires every second and can send SIGKILL atomically within the same execution context as the RSS check.

### 4.5 Scheduling Behavior

Linux uses the Completely Fair Scheduler (CFS) which allocates CPU time proportionally based on each task's weight. The `nice` value maps to a weight: nice=0 gets a baseline weight of 1024, while nice=15 gets a significantly lower weight, meaning CFS gives it proportionally less CPU time when competing with a higher-priority process.

Our experiments confirmed this. Two identical `cpu_hog` processes running for 10 seconds each completed in 9.3s (nice=0) and 19.3s (nice=15) respectively when running simultaneously — the lower-priority container effectively got half the CPU share. In the CPU vs I/O experiment, the CPU-bound container finished in 4s while the I/O-bound container took 13s. The CPU-bound process got more CPU time because the I/O-bound process was frequently sleeping between write iterations, voluntarily yielding the CPU. CFS correctly identified the I/O-bound process as less CPU-hungry and prioritized the CPU-bound one when it was runnable.

---

## 5. Design Decisions and Tradeoffs

### Namespace Isolation
**Choice:** `CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS` with `chroot`.  
**Tradeoff:** `chroot` is simpler to implement than `pivot_root` but is less secure — a privileged process inside the container could potentially escape. `pivot_root` would be the production choice.  
**Justification:** For this project's scope, `chroot` provides the required filesystem isolation without the complexity of `pivot_root`.

### Supervisor Architecture
**Choice:** Single long-running supervisor process with a UNIX socket accept loop.  
**Tradeoff:** The accept loop handles one request at a time sequentially. Concurrent CLI commands (e.g. two simultaneous `start` calls) are serialized. A threaded accept loop would handle this better.  
**Justification:** Sequential handling is safe, simple, and sufficient for the demo workload. It avoids concurrency bugs in the command dispatch path.

### IPC and Logging
**Choice:** Pipes for log data, UNIX domain socket for control commands.  
**Tradeoff:** Log data is limited to `CONTROL_MESSAGE_LEN` bytes in the response. For large logs, streaming over a second socket would be better.  
**Justification:** Two distinct channels keeps log throughput and control latency independent. A single channel for both would mean large log reads could block CLI responsiveness.

### Kernel Monitor
**Choice:** Mutex-protected linked list with a 1-second periodic timer.  
**Tradeoff:** A 1-second polling interval means a process could exceed its hard limit by up to 1 second's worth of allocations before being killed.  
**Justification:** A mutex is appropriate here because the timer callback and ioctl handler can sleep (they run in process context), making a spinlock unnecessary. The 1-second interval is a reasonable tradeoff between enforcement latency and kernel overhead.

### Scheduling Experiments
**Choice:** Used `nice` values rather than CPU affinity for priority experiments.  
**Tradeoff:** `nice` affects scheduling weight but both processes still run on all CPUs. CPU affinity would isolate them more strictly but would require a multi-core setup to observe meaningful differences.  
**Justification:** `nice` directly exercises CFS weight-based scheduling which is the core Linux scheduling mechanism, making the results more illustrative of scheduler behavior.

---

## 6. Scheduler Experiment Results

### Experiment 1 — Two CPU-bound containers with different priorities

Both containers ran `/cpu_hog 10` (burn CPU for 10 seconds) simultaneously.

| Container | Nice Value | Wall Time |
|-----------|-----------|-----------|
| cpu_normal | 0 | 9.295s |
| cpu_nice | 15 | 19.295s |

The nice=15 container took approximately 2x longer to complete the same workload. CFS assigned cpu_normal roughly twice the CPU share of cpu_nice due to the weight difference between nice=0 and nice=15.

### Experiment 2 — CPU-bound vs I/O-bound container

| Container | Type | Wall Time |
|-----------|------|-----------|
| cpu_exp | CPU-bound (nice=0, 10s) | 4.063s |
| io_exp | I/O-bound (20 iterations, 200ms sleep) | 13.169s |

The CPU-bound container finished significantly faster than its 10-second target because the I/O-bound container spent most of its time sleeping between write iterations. CFS detected that the I/O-bound process was not consuming its full CPU quota and gave the CPU-bound process more time. The I/O-bound container took longer than its expected 4 seconds (20 × 200ms) because of scheduling delays when it woke up from sleep and had to wait for the CPU-bound container to be preempted.

These results demonstrate two core CFS properties: weight-based fairness under contention, and throughput-oriented behavior where sleeping processes do not block CPU-hungry ones.
