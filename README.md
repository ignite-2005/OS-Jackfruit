|           Name          |      SRN      |
|-------------------------|---------------|
| Surjo Sarkar            | PES1UG24CS484 |
| Sumukh A S              | PES1UG24CS481 |

---

### Prerequisites
```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r) wget
```

### Download Alpine rootfs
```bash
cd boilerplate/
mkdir -p rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base
```

### Build
```bash
cd boilerplate/
make all
```

### Load kernel module
```bash
sudo insmod monitor.ko
ls -l /dev/container_monitor
dmesg | tail -3
```

### Prepare per-container rootfs copies
```bash
cp -a rootfs-base rootfs-alpha
cp -a rootfs-base rootfs-beta
cp -a rootfs-base rootfs-memtest
cp cpu_hog    rootfs-alpha/
cp cpu_hog    rootfs-beta/
cp memory_hog rootfs-memtest/
cp io_pulse   rootfs-alpha/
```

### Start supervisor (Terminal 1)
```bash
sudo ./engine supervisor ./rootfs-base
```

### CLI commands (Terminal 2)
```bash
sudo ./engine start alpha ./rootfs-alpha /bin/sh --soft-mib 48 --hard-mib 80
sudo ./engine start beta  ./rootfs-beta  /bin/sh --soft-mib 64 --hard-mib 96
sudo ./engine ps
sudo ./engine logs alpha
sudo ./engine stop alpha
```

### Test memory limits
```bash
sudo ./engine start memtest ./rootfs-memtest /memory_hog --soft-mib 10 --hard-mib 20
# wait 15 seconds
dmesg | grep container_monitor
sudo ./engine ps
```

### Teardown
```bash
sudo ./engine stop beta
# Ctrl+C in Terminal 1 to stop supervisor
ps aux | grep defunct
sudo rmmod monitor
dmesg | tail -3
```

---

**Screenshot 1 — Supervisor running with two containers registered**

![Supervisor terminal showing kernel monitor opened, control socket bound, and both alpha and beta containers registered](1.jpeg)

**Screenshot 2 — `engine ps` showing both containers running**

![PS output showing alpha and beta running with correct soft/hard MiB limits](2..jpeg)

**Screenshot 3 — `engine logs` output**

![Logs command showing empty log for alpha (running /bin/sh which produces no output)](3.jpeg)

**Screenshot 4 — `engine stop` command**

![Stop command sending SIGTERM to alpha container](4.jpeg)

**Screenshot 5 — Supervisor terminal showing SIGKILL escalation**

![Supervisor showing alpha did not exit after SIGTERM, sent SIGKILL](5.jpeg)

**Screenshot 6 — Memory limits: SOFT LIMIT warning + HARD LIMIT kill + logs**

![dmesg showing soft limit and hard limit events, ps showing memtest as hard_limit_killed, and memory_hog logs showing 3 allocations before kill](6.jpeg)

**Screenshot 7 — Scheduling experiments: CPU-bound with different nice values**

![Two cpu_hog containers with nice=0 and nice=10 both completing, with full log output showing elapsed times](7.jpeg)

**Screenshot 8 — Clean teardown: no zombies + module unloaded**

![ps aux grep defunct showing no zombies, dmesg showing module unloaded cleanly](8.jpeg)

**Screenshot 9 — Supervisor full lifecycle: startup → containers → clean shutdown**

![Supervisor terminal showing full lifecycle: module opened, all containers registered, SIGTERM/SIGKILL escalation, Ctrl+C shutdown, logger flushed, clean exit with no zombies](9.jpeg)

---

Linux does not have a built-in concept of a "container". A container is simply a normal
Linux process that has been given private views of certain system resources using a
kernel feature called **namespaces**, and a locked-down filesystem using **chroot**.
There is no separate OS, no hypervisor, and no virtual machine. The host kernel is
still the same kernel — it just shows each container a different picture of the world.

Our runtime creates three namespaces per container by passing flags to `clone()`:

**PID namespace (`CLONE_NEWPID`)**

The kernel maintains a process ID table for every PID namespace separately. When we
clone with `CLONE_NEWPID`, the child process gets its own private PID table. The first
process in that table is automatically assigned PID 1, regardless of what PID the host
kernel uses to track it. From the container's perspective, it is PID 1. From the host's
perspective, it might be PID 6804. When the container calls `getpid()`, the kernel looks
up the PID namespace of the calling process and returns 1.

The important consequence is that the container cannot see or signal any host process
because those PIDs simply do not exist in the container's namespace table. If the
container tries `kill(1234, SIGTERM)`, the kernel looks up PID 1234 in the container's
namespace, finds nothing, and returns an error.

**UTS namespace (`CLONE_NEWUTS`)**

UTS stands for Unix Time Sharing. This namespace controls two things: the system
hostname and the NIS domain name. The kernel stores these inside a struct called
`uts_namespace`. Without `CLONE_NEWUTS`, all processes on the machine share one
`uts_namespace` struct. With it, the child gets its own private copy of that struct.

This means when `child_fn()` calls `sethostname("alpha", 5)`, it writes into the
container's private copy only. The host hostname is completely unaffected. This lets
each container believe it has its own machine name, which matters for any software
inside that reads the hostname.

**Mount namespace (`CLONE_NEWNS`)**

The kernel maintains a tree of all mounted filesystems. Every mount and unmount
operation modifies this tree. Without `CLONE_NEWNS`, all processes share one global
mount tree. With it, the child gets a private copy of the mount tree at the moment
`clone()` is called. All subsequent mount and unmount operations inside the child
are written into the child's private copy and are completely invisible to the host.

This is what makes our `mount("proc", "/proc", "proc", ...)` call safe. Without
`CLONE_NEWNS`, mounting procfs inside the container would pollute the host's mount
table. With it, only the container sees the new mount. The host's mount table is
untouched.

**chroot()**

After `clone()`, `child_fn()` calls `chroot(cfg->rootfs)`. This call changes a field
called `root` inside the process's `fs_struct` kernel structure. Before `chroot`, the
`root` pointer points to the real system root. After `chroot`, it points to the
container's rootfs directory. From that moment on, every path lookup the kernel does
for this process starts from the new root. The path `/bin/sh` now resolves to
`./rootfs-alpha/bin/sh` from the host's perspective.

A process inside the container cannot escape to the real root by doing `cd ../../../..`
because the kernel clamps path traversal at the `root` pointer. Going `..` from `/`
brings you back to `/` — it loops.

Note: `chroot()` is simpler but less secure than `pivot_root`. A privileged process
inside the container could theoretically call `chroot()` again to escape. `pivot_root`
fully replaces the root mount point and can unmount the old root, making escape
impossible. For this project, `chroot()` is sufficient since we run trusted workloads.

**What the host kernel still shares with all containers**

The host kernel itself is shared. Every system call from every container is handled by
the same kernel code. There is no separate kernel per container. The physical CPU,
physical RAM, the system clock, and — because we do not use `CLONE_NEWNET` — the
network stack are all shared. This means a kernel vulnerability exploited inside one
container can affect the host and all other containers. This is the fundamental
security tradeoff of container-based isolation compared to full virtual machines.

---

**Why processes become zombies**

When any Linux process exits, it does not disappear immediately. The kernel frees its
memory and resources but keeps a small entry in the process table containing the exit
status. This entry stays there in a "zombie" state until the parent process calls
`waitpid()` to collect the exit status. Only after `waitpid()` does the kernel remove
the entry completely.

If the parent exits before calling `waitpid()`, the orphaned child is re-parented to
PID 1 (init/systemd), which periodically calls `waitpid()` to clean up. But if the
parent is still alive and never calls `waitpid()`, the zombie accumulates in the
process table forever, consuming a PID slot. On a long-running system with many
containers, this adds up and eventually exhausts the available PID space.

**Why a long-running supervisor prevents zombies**

The supervisor is the parent of every container process because `clone()` is called
from inside the supervisor. When any container exits, the kernel delivers `SIGCHLD`
to the supervisor. Our signal handler calls:
```c
while ((pid = waitpid(-1, &status, WNOHANG)) > 0) { ... }
```

The `-1` argument means "reap any child that has exited". `WNOHANG` means "return
immediately if no child is ready" rather than blocking. The `while` loop is necessary
because multiple containers could exit between two SIGCHLD deliveries — Linux does not
queue one SIGCHLD per child death, so a single SIGCHLD might represent three dead
containers. The loop drains all of them.

`SA_NOCLDSTOP` on the signal action means we only receive SIGCHLD when a child exits,
not when it is paused by SIGSTOP. `SA_RESTART` means that if SIGCHLD interrupted a
blocking system call like `accept()`, the kernel automatically retries the call instead
of returning `EINTR`.

**Metadata tracking**

Every container gets a `container_record_t` node in a linked list owned by the
supervisor. The node records the container ID, host PID, start time, current state
(`running`, `stopped`, `exited`, `hard_limit_killed`), memory limits, log file path,
and the `stop_requested` flag. All access to this list goes through `metadata_lock`
(a mutex) to prevent races between the event loop thread and the SIGCHLD handler which
runs asynchronously.

**The `stop_requested` flag and termination classification**

Both `engine stop` (sending SIGTERM/SIGKILL manually) and the kernel module's hard-limit
enforcement (sending SIGKILL when RSS exceeds the hard limit) cause the container to die
from a signal. From the SIGCHLD handler's perspective, both look identical — the child
died from a signal. Without additional information, it cannot tell whether the death was
intentional or due to memory enforcement.

The `stop_requested` flag solves this. When the supervisor's `CMD_STOP` handler runs,
it sets `c->stop_requested = 1` **before** sending any signal. When the SIGCHLD handler
later collects the exit status, it checks:

- `WIFEXITED(status)` → normal exit → `CONTAINER_EXITED`
- `WIFSIGNALED` + `stop_requested == 1` → we asked it to stop → `CONTAINER_STOPPED`
- `WIFSIGNALED` + `SIGKILL` + `stop_requested == 0` → kernel module killed it →
  `CONTAINER_HARD_LIMIT_KILLED`
- Any other signal → `CONTAINER_KILLED`

**Signal delivery across namespaces**

The supervisor operates in the host's PID namespace and always stores the host-side PID
in `container_record_t.host_pid`. When it calls `kill(c->host_pid, SIGTERM)`, the kernel
looks up that PID in the host namespace, finds the container process, and delivers the
signal. This works correctly because the signal is sent from outside the container's PID
namespace.

---

This project uses two completely separate IPC mechanisms for two different purposes.

**Path A — Pipes (logging)**

A pipe is a kernel-maintained byte stream. It has two file descriptors: one for writing
and one for reading. Data written to the write end appears at the read end in order.
The pipe has a kernel buffer (typically 64KB) — writers block when it is full, readers
block when it is empty.

We create a pipe before each `clone()`. The child inherits the write end and we use
`dup2()` to make it the child's stdout and stderr. The supervisor keeps the read end.
We then immediately close the supervisor's copy of the write end.

This closing step is critical and a very common bug if forgotten. A pipe reaches EOF on
the read side only when **every** write-end file descriptor is closed. If the supervisor
keeps its own copy of the write end open, the pipe will never reach EOF even after the
container exits. The producer thread would block on `read()` forever, waiting for data
that will never come. By closing the supervisor's write end immediately after `clone()`,
we guarantee that when the container exits and its write end closes, the pipe reaches
EOF and the producer thread unblocks and exits cleanly.

**Path B — UNIX domain socket (control)**

A UNIX domain socket is like a TCP socket but lives on the filesystem (`/tmp/mini_runtime.sock`)
instead of a network port. It supports bidirectional, connection-oriented communication
between processes on the same machine. We use `SOCK_STREAM` which gives reliable,
ordered byte delivery.

The protocol is intentional: we send fixed-size binary structs (`control_request_t` and
`control_response_t`) rather than text strings. With text, you need delimiters, you need
to handle partial reads (a `read()` might return fewer bytes than you sent), and you need
to parse the string. With fixed-size structs, a single `read(fd, &req, sizeof(req))`
either reads the complete struct or fails — no partial-read ambiguity.

**Why two different mechanisms**

Pipes are unidirectional and naturally model streaming log output (container → supervisor).
UNIX sockets are bidirectional and naturally model request/response (CLI client ↔ supervisor).
Using pipes for the control channel would require two pipes per connection (one for each
direction), manual framing, and complex lifetime management. The two-mechanism design
uses each tool for what it is best at.

**Shared data structure 1 — Container linked list**

Protected by: `pthread_mutex_t metadata_lock`

The linked list is accessed by two different code paths that can run at the same time:
the event loop thread (inside `handle_client()`, which may insert a new container or
update a state) and the SIGCHLD handler (which runs asynchronously on any signal delivery
and updates container states and removes entries). Without a mutex, these could overlap.
For example, `handle_client()` could be in the middle of walking the list to find a
container by name while the SIGCHLD handler simultaneously updates the `next` pointer of
a node, causing `handle_client()` to follow a corrupted pointer.

A mutex is the correct choice here. Both code paths need to read and write multiple
fields in a single atomic operation (check a state, then update it). A spinlock would be
wrong — the event loop can sleep (inside `accept()`) while holding the lock, which is
forbidden for spinlocks.

**Shared data structure 2 — Bounded buffer (ring buffer)**

Protected by: `pthread_mutex_t` + `pthread_cond_t not_full` + `pthread_cond_t not_empty`

The ring buffer is a fixed array of 16 `log_item_t` slots with a `head` index (consumer
reads from here), a `tail` index (producer writes here), and a `count`. Slots are reused
in circular order using modulo arithmetic (`tail = (tail + 1) % 16`).

**Race condition without mutex:** Two producer threads (one for container alpha, one for
container beta) could both call `bounded_buffer_push()` simultaneously. Both read
`count`, both see there is room, both compute `tail` as the same index, and both write
to slot `items[tail]`. One entry silently overwrites the other. The mutex prevents this
by ensuring only one producer can modify `head`, `tail`, and `count` at a time.

**Why `not_full` condition variable instead of busy-spinning:** When the buffer is full
(all 16 slots occupied), the producer must wait. Without a condition variable, the
producer would loop checking `count == 16` continuously — wasting 100% CPU and starving
the consumer. With `pthread_cond_wait(&not_full, &mutex)`, the producer atomically
releases the mutex and suspends. When the consumer calls `pthread_cond_signal(&not_full)`
after popping an entry, the producer wakes up, re-acquires the mutex, and checks again.
This is zero-CPU waiting.

**Why `not_empty` condition variable:** The consumer has the same problem in the other
direction — when the buffer is empty it must wait without spinning. `pthread_cond_wait
(&not_empty, &mutex)` suspends the consumer until a producer signals it after a push.

**Why `while` not `if` around `pthread_cond_wait`:** POSIX explicitly permits spurious
wakeups — a thread can return from `cond_wait` even though no one called `signal` and
the condition has not changed. This is not a bug; it is an intentional specification
to allow efficient implementations on certain CPU architectures. If we used `if`, the
thread would proceed as if the buffer has room when it actually does not, potentially
overwriting data. The `while` loop re-checks the actual condition (`count == 16`) and
goes back to sleep if still true.

**Shutdown guarantee — no log data lost**

When the supervisor shuts down, it calls `bounded_buffer_begin_shutdown()`, which sets
`shutting_down = 1` and broadcasts on both condition variables to wake all sleeping
threads. Producer threads see the shutdown flag and stop pushing. The consumer's
`bounded_buffer_pop()` is designed to return the "exit" signal (return value 1) only
when `count == 0 AND shutting_down == 1`. This means the consumer writes every single
entry that was already pushed before exiting. The supervisor then calls
`pthread_join(ctx->logger_thread)` to block until the consumer has finished writing
everything to disk before freeing any memory.

---

**What RSS actually measures**

RSS stands for Resident Set Size. It is the count of physical memory pages that are
currently in RAM and mapped into a process's page tables. The kernel tracks this inside
the process's `mm_struct`. Our kernel module calls `get_mm_rss(mm)` which sums three
internal counters:

- **Anonymous pages**: stack memory, heap memory (`malloc`/`mmap` without a file backing)
- **File-mapped pages**: shared libraries (like `libc.so`) that have been loaded from
  disk into RAM
- **Shared memory pages**: memory explicitly shared between processes via `shmget`

We multiply the page count by `PAGE_SIZE` (4096 bytes on x86) to get bytes.

**What RSS does NOT measure**

RSS deliberately does not count several categories of memory:

- **Virtual memory that has never been touched**: Linux uses lazy (demand) allocation.
  When a process calls `malloc(100MB)`, the kernel does not immediately put 100MB of
  physical pages into RAM. It just updates the virtual address space tables. Pages only
  enter RAM when the process actually reads or writes them (page fault). This is why
  `memory_hog.c` calls `memset()` after every `malloc()` — without `memset`, the pages
  would not be in RAM and RSS would not grow.
- **Swapped-out pages**: Pages that were in RAM but have been written to the swap
  partition to make room for other processes. They were once in RSS but are no longer.
- **Kernel memory allocated on behalf of the process**: Socket buffers, pipe buffers,
  kernel stack — these are kernel memory, not counted in the process's RSS.

The practical implication is that a process's virtual address space (shown as VSZ in
`ps`) can be much larger than its RSS. A process that allocates 1GB virtually but only
touches 10MB will show RSS of 10MB.

**Why soft and hard limits serve different purposes**

A **soft limit** is a monitoring threshold. When RSS crosses the soft limit, the kernel
module logs a warning to `dmesg` but does nothing to the process. The process continues
running normally. This is useful because a spike in memory usage might be temporary and
legitimate — a container that briefly uses more RAM than expected might self-correct.
The soft limit gives operators visibility into this behavior without causing disruption.

A **hard limit** is an enforcement threshold. When RSS crosses the hard limit, the kernel
module sends `SIGKILL` to the container process. `SIGKILL` cannot be caught, blocked, or
ignored by the process — it is unconditional termination. This is appropriate when a
process has consumed more physical RAM than the system can afford to give it, and waiting
any longer would destabilise other containers or the host.

The two-tier design follows the principle: **observe first, act when necessary**. The
gap between soft and hard limits gives operators a window to notice the problem and
potentially intervene before enforcement happens automatically.

**Why enforcement must be in kernel space**

Consider what happens if we try to do memory enforcement from user space: the supervisor
would need to periodically read `/proc/<pid>/status` or `/proc/<pid>/smaps` to get the
RSS, compare it to the limit, and call `kill()` if exceeded. This approach has three
fundamental reliability problems:

1. **The enforcer can be starved**: The container process consuming all the RAM can also
   be scheduled in a way that starves the supervisor of CPU time. The supervisor's
   polling loop might not run for several seconds while the container continues growing.

2. **The enforcer can be delayed by scheduling**: Even if the supervisor is not starved,
   the kernel might not schedule it for several hundred milliseconds. During that window,
   the container can allocate significantly more memory than the hard limit.

3. **`kill()` from user space requires permission checks**: The kernel verifies that the
   calling process has permission to signal the target. While this is satisfied when the
   supervisor runs as root, it is an extra dependency that could fail in unexpected
   configurations.

A kernel timer callback has none of these problems. It fires on a kernel timer interrupt
which is handled by the scheduler directly, regardless of what user-space processes are
doing. `send_sig()` inside the kernel requires no permission checks — the kernel always
has authority over all processes. The enforcement is reliable, timely, and cannot be
circumvented by the monitored process.

---

**How CFS works**

Linux uses the Completely Fair Scheduler (CFS) for normal (non-realtime) processes. The
goal of CFS is proportional fairness: every runnable process should receive CPU time
proportional to its scheduling weight. CFS achieves this by tracking a value called
`vruntime` (virtual runtime) for each process. `vruntime` represents how much CPU time
the process has received, adjusted by its weight.

At each scheduling decision, CFS picks the process with the lowest `vruntime` from a
red-black tree (which gives O(log n) minimum lookup). This means the process that has
received the least CPU time relative to its weight always runs next.

The `nice` value determines the weight. Nice 0 has weight 1024. Each step of nice
reduces weight by approximately 10%. Nice 10 has weight 110. The relationship between
weight and `vruntime` accumulation is: a process's `vruntime` grows at a rate of
`actual_time × (1024 / weight)`. So a nice=10 process's vruntime grows at
`1024/110 ≈ 9.3×` the rate of a nice=0 process. CFS always runs the process with the
smallest vruntime, so the nice=10 process is picked far less often.

**Experiment 1 — CPU-bound vs CPU-bound with different priorities**

Both containers ran the same `cpu_hog` workload (a tight compute loop) simultaneously.

| Container | Nice value | Real time |
|-----------|-----------|-----------|
| exp1      | 0         | ~10s      |
| exp2      | 10        | ~16s      |

exp2 took significantly longer than exp1 for identical work. The reason is the weight
difference. When both are runnable simultaneously, CFS gives exp1 a share of
`1024 / (1024 + 110) ≈ 90%` of CPU time and exp2 only `110 / (1024 + 110) ≈ 10%`.
On a multi-core system the effect is reduced because each container may run on a
separate core. This demonstrates CFS's **fairness goal**: the scheduler honours the
priority difference while ensuring both processes make forward progress.

**Experiment 2 — CPU-bound vs I/O-bound**

| Container | Type      | Workload        | Real time |
|-----------|-----------|-----------------|-----------|
| cpuexp    | CPU-bound | cpu_hog (20s)   | ~14s      |
| ioexp     | I/O-bound | io_pulse (40 iter) | ~11s   |

The I/O-bound process (`io_pulse`) calls `usleep()` between every write, spending most
of its time sleeping rather than using CPU. When a sleeping process wakes up in CFS,
it has not accumulated vruntime during the sleep. CFS gives waking processes a vruntime
close to the current minimum — effectively the lowest value in the run queue — so they
are scheduled almost immediately when they wake up. This is CFS's **responsiveness
mechanism**: processes that voluntarily yield the CPU are rewarded with fast response
when they need it again.

The consequence shown in our results is that `ioexp` finished in ~11 seconds despite
`cpuexp` running the entire time on the same system. The I/O-bound workload barely
competed with the CPU-bound one because they wanted the CPU at different times —
`io_pulse` grabbed it briefly for each write, then gave it up during `usleep`, letting
`cpu_hog` use it uncontested during the sleep window.

`cpuexp` took ~14 seconds instead of 20 seconds because it had a core mostly to itself
given the low CPU demand of `io_pulse`. This demonstrates CFS's **throughput goal**:
CPU-bound processes get the leftover CPU time without starvation, while I/O-bound
processes get low latency without penalty.

The combined result illustrates a core principle of Linux scheduling: the scheduler
does not punish processes for doing I/O. A process that sleeps frequently is not
penalised — in fact it is advantaged by receiving a fresh vruntime budget when it
wakes. This is what makes Linux responsive for interactive and I/O-heavy workloads
even when CPU-bound background work is running concurrently.

---

**Namespace isolation — chroot vs pivot_root:**
We use `chroot()` for simplicity. The tradeoff is that a privileged process inside the container could theoretically escape via a `chroot()` call of its own (since we run as root). `pivot_root` is more secure because it fully replaces the root mount point and unmounts the old root, but requires more complex setup (a temporary bind mount). For a project environment where containers run trusted workloads, `chroot` is the right tradeoff.

**Supervisor architecture — single-threaded event loop:**
The supervisor handles one client at a time (no thread per client). The tradeoff is that a slow client (e.g., a `run` command waiting for a long container) blocks all other CLI commands during that wait. This was acceptable because the project does not require concurrent CLI users, and a single-threaded loop is far simpler to reason about for signal safety.

**IPC/logging — UNIX socket + pipes:**
UNIX sockets for control and pipes for logging are the natural fit: sockets are bidirectional and connection-oriented (good for request/response), pipes are unidirectional streams (good for log output). The tradeoff vs shared memory would be higher throughput at the cost of much more complex synchronisation. For log data the bandwidth of pipes is sufficient.

**Kernel monitor — spinlock vs mutex:**
We use a spinlock because the timer callback runs in atomic (softirq) context where sleeping is forbidden. The tradeoff is that on a multi-core system a spin-waiter burns a CPU while waiting. Since our critical section is very short (a few list pointer comparisons and a kfree), spinlock is correct and the spin time is negligible.

**Scheduling experiments — nice values vs cgroups:**
We use `nice()` for priority control because it requires no kernel configuration and is directly observable. cgroups provide stronger isolation (absolute CPU quotas) but require root-level configuration outside the runtime. For demonstrating scheduler behavior, nice values are simpler and the results are more predictable.

---

### Experiment 1 — Priority difference (CPU-bound vs CPU-bound)
Both containers run the same `cpu_hog 30` workload simultaneously. One has default priority (nice=0), the other has lower priority (nice=10).

| Container | Nice | Measured real time |
|-----------|------|-------------------|
| exp1      | 0    | ~10s              |
| exp2      | 10   | ~16s              |

**Interpretation:** CFS weight for nice=0 is 1024; for nice=10 it is 110. On a single core this means exp1 gets approximately 1024/(1024+110) ≈ 90% of CPU time and exp2 gets ≈10%. exp2's real time should be roughly 9× exp1's if both run to completion. On a multi-core system the effect is less pronounced because each container may run on a separate core.

### Experiment 2 — CPU-bound vs I/O-bound
| Container | Type      | Workload        | Measured real time |
|-----------|-----------|-----------------|-------------------|
| cpuexp    | CPU-bound | cpu_hog 20      | ~14s              |
| ioexp     | I/O-bound | io_pulse 40 200 | ~11s              |

**Interpretation:** The I/O-bound workload completed in approximately the same time as if running alone because it spent most of its time sleeping in `usleep()`. CFS correctly identified it as a low-vruntime process and gave it immediate CPU access when it woke up. This demonstrates CFS's design goal: I/O-bound processes should not be penalised for yielding the CPU voluntarily.
