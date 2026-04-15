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

Isolation is enforced via the clone() system call using flags CLONE_NEWPID, CLONE_NEWUTS, and CLONE_NEWNS. This creates a new process tree, hostname, and mount namespace for each container. The OS works this way to ensure that processes in one container cannot see or signal processes in another. Our project exercises this by using sethostname() to give each container a unique identity and mounting a private /proc instance so that top or ps inside the container only shows its own internal processes.

### 4.2 Supervisor and Process Lifecycle

The supervisor acts as a long-running daemon that manages the lifecycle of all containers. It uses a self-pipe trick to handle SIGCHLD signals safely within a select() multiplexing loop. This allows the supervisor to reap exited processes via waitpid() and update their metadata state (e.g., CONTAINER_EXITED or CONTAINER_KILLED) without creating zombie processes or encountering async-signal-safety issues.

### 4.3 IPC, Threads, and Synchronization
The runtime utilizes two primary IPC mechanisms: Pipes for streaming stdout/stderr from containers to the supervisor, and a Unix Domain Socket (/tmp/mini_runtime.sock) for CLI-to-supervisor communication. To handle logging, we implement a Bounded-Buffer using a pthread_mutex and two condition variables (not_empty, not_full). This prevents race conditions where multiple producer threads (one per container) might overwrite the same log slot before the single consumer logging thread can write it to disk.

### 4.4 Memory Management and Enforcement

Memory enforcement is handled by a custom Linux Kernel Module (LKM). The LKM maintains a linked list of monitored processes and uses a periodic timer callback (firing every 1 second) to check the Resident Set Size (RSS) of each PID. If a container exceeds its soft_limit, the LKM logs a warning to dmesg; if it exceeds the hard_limit, the LKM immediately sends a SIGKILL to the process. This demonstrates how the kernel can provide out-of-band resource policing that user-space programs cannot bypass.

### 4.5 Scheduling Behavior

This project implements a lightweight container runtime in C that provides process isolation, resource monitoring, and a centralized management system. It utilizes Linux Namespaces (PID, UTS, Mount) to create isolated execution environments, managed by a multi-threaded Supervisor that handles container lifecycles and logging via a synchronized Bounded-Buffer pipeline. A custom Linux Kernel Module (LKM) performs periodic Resident Set Size (RSS) checks to enforce memory limits, while the CFS scheduler is leveraged via nice values to demonstrate prioritized CPU allocation. Finally, a CLI client communicates with the supervisor over Unix Domain Sockets to provide a robust interface for starting, stopping, and monitoring containers.

---

## 5. Design Decisions and Tradeoffs

### Namespace Isolation
**Choice:** CLONE_NEWNS + chroot..  
**Tradeoff:** chroot is simpler but less secure than pivot_root.
**Justification:** It provides sufficient filesystem isolation for an educational runtime without the complexity of nested mount management.

### Supervisor Architecture
**Choice:** Multi-threaded with Self-Pipe signal handling.  
**Tradeoff:** Significantly more complex to code than a standard signal handler. 
**Justification:** It is the only way to ensure async-signal-safety when modifying global container metadata during a SIGCHLD event.
### IPC and Logging
**Choice:** Bounded-Buffer synchronization.  
**Tradeoff:** Producers block if the buffer is full, potentially slowing high-output containers.
**Justification:** Protects the host system's RAM from being overwhelmed by a rogue container's logs.

### Kernel Monitor
**Choice:** 1-second mod_timer checks.  
**Tradeoff:** A process could theoretically spike memory usage between the 1-second intervals
**Justification:** Higher frequency checks would consume excessive CPU cycles; 1 second is a standard balance for system monitors.

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
| cpu_normal | 0 | 10.22 |
| cpu_nice | 15 | 18.45s|

As a result, cpu_normal finished almost at its target time, while cpu_nice was "starved" of cycles and took nearly twice as long to complete the same amount of work.

### Experiment 2 — CPU-bound vs I/O-bound container

| Container | Type | CPU Use (%)|
|-----------|------|-----------|
| cpu_exp | CPU-bound | 98.2% |
| io_exp | I/O-bound  | 1.8% |

The Linux kernel successfully balances throughput and latency. Our engine demonstrates that by putting these workloads in separate containers, the kernel still manages them as a single scheduling unit, ensuring the I/O-pulsing task is never "starved" by the infinite loop in the other container.


