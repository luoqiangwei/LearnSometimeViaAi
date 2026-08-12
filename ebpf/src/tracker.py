#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
tracker.py — eBPF 进程监控与内存泄漏追踪（用户态主控）

功能：
  * 进程/线程生命周期、D 态过长、等锁（futex）过长事件跟踪
  * 目标进程 RSS 双阈值管理：
      超阈值 1 -> 写 armed map，BPF 开始在页错误路径抓用户栈
      超阈值 2 -> 先符号化堆栈生成 Top10 报告，再 SIGKILL（--no-kill 可关）
  * 可选 --track-locks：uprobe 统计 pthread_mutex 持锁时长
  * 可选 --kprobe：用 kprobe 挂点替代 tracepoint（开销对比实验，
    D 态事件在此模式下不抓内核栈，见 bpf_program_kprobe.c 头注释）
  * 可选 --raw-tp：用 raw tracepoint 挂点（绕过 perf 框架，功能与
    经典版完全对齐，见 bpf_program_rawtp.c 头注释）

用法示例：
  sudo python3 src/tracker.py --comm leak_demo --rss-t1 64 --rss-t2 96
  sudo python3 src/tracker.py --pid 1234 --track-locks
  sudo python3 src/tracker.py --raw-tp --comm leak_demo
"""
import argparse
import ctypes
import glob
import os
import re
import signal
import sys
import threading
import time
from datetime import datetime

from bcc import BPF

EV_NAME = {1: "FORK", 2: "EXEC", 3: "EXIT", 4: "D-STATE", 5: "FUTEX-WAIT", 6: "LOCK-HOLD"}
PAGE = os.sysconf("SC_PAGE_SIZE")
_SRC_DIR = os.path.dirname(os.path.abspath(__file__))
BPF_SRC = os.path.join(_SRC_DIR, "bpf_program.c")
BPF_SRC_KPROBE = os.path.join(_SRC_DIR, "bpf_program_kprobe.c")
BPF_SRC_RAWTP = os.path.join(_SRC_DIR, "bpf_program_rawtp.c")

# kprobe 模式下与 7 个 tracepoint 对应的挂点（见 bpf_program_kprobe.c 头注释）
KPROBE_HOOKS = [
    ("kprobe", "wake_up_new_task", "kprobe_fork"),
    ("kprobe", "begin_new_exec", "kprobe_exec"),
    ("kprobe", "do_exit", "kprobe_exit"),
    ("kprobe", "finish_task_switch.isra.0", "kprobe_sched_switch"),
    ("kprobe", "__x64_sys_futex", "kprobe_futex_enter"),
    ("kretprobe", "__x64_sys_futex", "kprobe_futex_exit"),
    ("kprobe", "handle_mm_fault", "kprobe_page_fault"),
]

# raw tracepoint 模式的挂点（见 bpf_program_rawtp.c 头注释；
# futex 走全系统调用的 sys_enter/sys_exit，BPF 内按 __NR_futex 过滤）
RAWTP_HOOKS = [
    ("sched_process_fork", "raw_tp_fork"),
    ("sched_process_exec", "raw_tp_exec"),
    ("sched_process_exit", "raw_tp_exit"),
    ("sched_switch", "raw_tp_sched_switch"),
    ("sys_enter", "raw_tp_sys_enter"),
    ("sys_exit", "raw_tp_sys_exit"),
    ("page_fault_user", "raw_tp_page_fault"),
]


def now_str():
    return datetime.now().strftime("%H:%M:%S.%f")[:-3]


def fmt_dur(ns):
    if ns >= 1e9:
        return "%.2fs" % (ns / 1e9)
    return "%.1fms" % (ns / 1e6)


def read_rss_mb(tgid):
    """读 /proc/<pid>/stat 的 rss 字段（第 24 列），返回 MB；进程消失返回 None"""
    try:
        with open("/proc/%d/stat" % tgid) as f:
            data = f.read()
        # comm 可能含空格/括号：取最后一个 ')' 之后的部分
        rest = data[data.rindex(")") + 2:]
        fields = rest.split()
        rss_pages = int(fields[21])  # stat 第 24 列
        return rss_pages * PAGE / 1048576.0
    except (OSError, ValueError, IndexError):
        return None


def read_comm(tgid):
    try:
        with open("/proc/%d/comm" % tgid) as f:
            return f.read().strip()
    except OSError:
        return None


def tid_to_tgid(tid):
    """解析线程 id -> 进程 tgid；失败返回 tid 本身"""
    try:
        with open("/proc/%d/status" % tid) as f:
            for line in f:
                if line.startswith("Tgid:"):
                    return int(line.split()[1])
    except OSError:
        pass
    return tid


def find_libc():
    for p in ("/lib/x86_64-linux-gnu/libc.so.6", "/lib64/libc.so.6"):
        if os.path.exists(p):
            return p
    hits = glob.glob("/lib/**/libc.so.6", recursive=True)
    return hits[0] if hits else None


class ProcState:
    NORMAL, ARMED, KILLED, REPORTED, GONE = range(5)
    NAME = {0: "正常", 1: "已武装抓栈", 2: "已杀死", 3: "已报告(未杀)", 4: "已退出"}


class Tracker:
    def __init__(self, args):
        self.args = args
        self.targets = {}            # tgid -> {"comm": str, "state": int, "history": [...]}
        self.event_buffer = []       # (ts, line) 近期事件，写报告时引用
        self.log_fp = None
        self.stop = threading.Event()

        os.makedirs(args.report_dir, exist_ok=True)
        log_path = os.path.join(args.report_dir,
                                "events_%s.log" % datetime.now().strftime("%Y%m%d"))
        self.log_fp = open(log_path, "a")

        if args.kprobe and args.raw_tp:
            print("--kprobe 与 --raw-tp 只能二选一", file=sys.stderr)
            sys.exit(1)
        bpf_src = BPF_SRC_KPROBE if args.kprobe else (
            BPF_SRC_RAWTP if args.raw_tp else BPF_SRC)
        with open(bpf_src) as f:
            text = f.read()
        self.b = BPF(text=text)

        if args.kprobe:
            self._attach_kprobes()
        elif args.raw_tp:
            self._attach_raw_tracepoints()

        cfg = self.b["config"]
        cfg[ctypes.c_int(0)] = ctypes.c_uint64(int(args.d_threshold_ms * 1e6))
        cfg[ctypes.c_int(1)] = ctypes.c_uint64(int(args.futex_threshold_ms * 1e6))
        cfg[ctypes.c_int(2)] = ctypes.c_uint64(max(1, args.stack_sample))
        cfg[ctypes.c_int(3)] = ctypes.c_uint64(int(args.lock_threshold_ms * 1e6))

        if args.track_locks:
            self._attach_lock_uprobes()

        self.b["events"].open_perf_buffer(self._on_event)

    # ---------- 初始化 ----------

    def _attach_kprobes(self):
        for kind, event, fn in KPROBE_HOOKS:
            try:
                if kind == "kprobe":
                    self.b.attach_kprobe(event=event, fn_name=fn)
                else:
                    self.b.attach_kretprobe(event=event, fn_name=fn)
            except Exception as ex:
                print("[fatal] kprobe 挂载失败 %s:%s -> %s: %s" % (kind, event, fn, ex))
                sys.exit(1)
        print("[init] kprobe 模式：已挂载 %d 个 kprobe/kretprobe" % len(KPROBE_HOOKS))

    def _attach_raw_tracepoints(self):
        for tp, fn in RAWTP_HOOKS:
            try:
                self.b.attach_raw_tracepoint(tp=tp, fn_name=fn)
            except Exception as ex:
                print("[fatal] raw tracepoint 挂载失败 %s -> %s: %s" % (tp, fn, ex))
                sys.exit(1)
        print("[init] raw tracepoint 模式：已挂载 %d 个 raw tracepoint"
              % len(RAWTP_HOOKS))

    def _attach_lock_uprobes(self):
        if not self.args.pid:
            print("[warn] --track-locks 需要配合 --pid 使用，跳过 uprobe 挂载")
            return
        libc = find_libc()
        if not libc:
            print("[warn] 找不到 libc，跳过 uprobe 挂载")
            return
        for pid in self.args.pid:
            try:
                self.b.attach_uprobe(name=libc, sym="pthread_mutex_lock",
                                     fn_name="uprobe_mutex_lock_entry", pid=pid)
                self.b.attach_uretprobe(name=libc, sym="pthread_mutex_lock",
                                        fn_name="uprobe_mutex_lock_ret", pid=pid)
                self.b.attach_uprobe(name=libc, sym="pthread_mutex_unlock",
                                     fn_name="uprobe_mutex_unlock_entry", pid=pid)
                print("[init] pid=%d 已挂载 pthread_mutex uprobe (%s)" % (pid, libc))
            except Exception as ex:
                print("[warn] pid=%d uprobe 挂载失败: %s" % (pid, ex))

    def _banner(self):
        a = self.args
        mode = "kprobe 模式" if a.kprobe else (
            "raw tracepoint 模式" if a.raw_tp else "tracepoint 模式")
        print("=" * 72)
        print("eBPF tracker 启动 @ %s  [%s]" % (
            datetime.now().strftime("%F %T"), mode))
        if a.pid:
            print("  目标 pid : %s" % a.pid)
        if a.comm:
            print("  目标 comm: %s (含新创建匹配进程)" % a.comm)
        if not a.pid and not a.comm:
            print("  目标     : 未指定（仅跟踪 D 态/等锁事件，不做 RSS 阈值管理）")
        print("  RSS 阈值 : T1=%dMB(开始抓栈)  T2=%dMB(生成报告%s)  采样间隔=%.1fs"
              % (a.rss_t1, a.rss_t2, "+杀死" if not a.no_kill else ",不杀死", a.rss_interval))
        print("  事件阈值 : D态=%dms  等锁=%dms  持锁=%dms%s  抓栈采样=1/%d"
              % (a.d_threshold_ms, a.futex_threshold_ms, a.lock_threshold_ms,
                 "" if a.track_locks else "(uprobe未开)", a.stack_sample))
        print("  报告目录 : %s" % os.path.abspath(a.report_dir))
        print("=" * 72)

    # ---------- 目标管理 ----------

    def rescan_targets(self):
        """按 --pid / --comm 扫描 /proc，更新目标集合"""
        a = self.args
        found = {}
        if a.pid:
            for pid in a.pid:
                comm = read_comm(pid)
                if comm:
                    found[pid] = comm
        if a.comm:
            for entry in os.listdir("/proc"):
                if not entry.isdigit():
                    continue
                pid = int(entry)
                comm = read_comm(pid)
                if comm and any(c in comm for c in a.comm):
                    found[pid] = comm
        # 新增目标
        for pid, comm in found.items():
            if pid not in self.targets:
                self.targets[pid] = {"comm": comm, "state": ProcState.NORMAL,
                                     "history": []}
                self._log("[target] 开始跟踪 pid=%d comm=%s" % (pid, comm), quiet=True)
        # 退出目标
        for pid in list(self.targets):
            if pid not in found and self.targets[pid]["state"] != ProcState.GONE:
                self.targets[pid]["state"] = ProcState.GONE
                self._log("[target] pid=%d 已退出" % pid)

    def _match(self, pid, tgid):
        """事件是否匹配目标过滤（未指定目标则全部匹配）"""
        if not self.args.pid and not self.args.comm:
            return True
        real_tgid = tgid if tgid in self.targets else tid_to_tgid(tgid)
        return real_tgid in self.targets

    def _match_comm(self, comm):
        """comm 是否匹配 --comm 子串"""
        return bool(self.args.comm) and any(c in comm for c in self.args.comm)

    def _match_lifecycle(self, ev, comm):
        """FORK/EXEC/EXIT 的匹配：目标集合、comm 子串、父进程归属 任一命中即可。
        进程刚创建时可能尚未被 rescan 收入 targets，需靠 comm/parent 兜底。"""
        if self._match(ev.pid, ev.tgid) or self._match_comm(comm):
            return True
        if ev.type == 1 and tid_to_tgid(ev.aux) in self.targets:  # FORK: parent 是目标
            return True
        if ev.type == 1 and self._match_comm(read_comm(ev.aux) or ""):
            return True
        return False

    # ---------- 事件处理 ----------

    def _log(self, line, quiet=False):
        stamped = "%s %s" % (now_str(), line)
        if not quiet:
            print(stamped)
        self.log_fp.write(stamped + "\n")
        self.log_fp.flush()
        self.event_buffer.append(stamped)
        if len(self.event_buffer) > 2000:
            del self.event_buffer[:1000]

    def _on_event(self, cpu, data, size):
        ev = self.b["events"].event(data)
        name = EV_NAME.get(ev.type, "?")
        comm = ev.comm.decode(errors="replace").rstrip("\x00")
        comm2 = ev.comm2.decode(errors="replace").rstrip("\x00")

        if ev.type in (1, 2, 3):      # FORK/EXEC/EXIT
            if not self._match_lifecycle(ev, comm):
                return
            if ev.type == 1:
                # raw tracepoint 版内核直接填了 child_comm(comm2)；否则从 /proc 补
                ccomm = comm2 or read_comm(ev.pid) or "?"
                line = "[%-10s] parent=%d -> child=%d (%s)" % (
                    name, ev.aux, ev.pid, ccomm)
            else:
                line = "[%-10s] pid=%d tgid=%d comm=%s" % (name, ev.pid, ev.tgid, comm)
            self._log(line)
            return

        if ev.type == 4:              # D-STATE
            if not self._match(ev.pid, ev.tgid):
                return
            self._log("[%-10s] tid=%d comm=%s D态持续 %s" % (
                name, ev.pid, comm, fmt_dur(ev.dur_ns)))
            if ev.stack_id >= 0 and self.args.verbose:
                for i, addr in enumerate(self.b["stack_traces"].walk(ev.stack_id)):
                    if i >= 6:
                        break
                    self._log("    %s" % self.b.ksym(addr).decode(errors="replace"))
            return

        if ev.type in (5, 6):         # FUTEX-WAIT / LOCK-HOLD
            if not self._match(ev.pid, ev.tgid):
                return
            self._log("[%-10s] tid=%d tgid=%d comm=%s 时长 %s" % (
                name, ev.pid, ev.tgid, comm, fmt_dur(ev.dur_ns)))
            return

    # ---------- RSS 轮询与阈值状态机 ----------

    def rss_loop(self):
        while not self.stop.is_set():
            self.rescan_targets()
            for pid, info in list(self.targets.items()):
                if info["state"] in (ProcState.KILLED, ProcState.REPORTED,
                                     ProcState.GONE):
                    continue
                rss = read_rss_mb(pid)
                if rss is None:
                    info["state"] = ProcState.GONE
                    self._log("[target] pid=%d 已退出" % pid)
                    continue
                info["history"].append((time.time(), rss))
                self._check_thresholds(pid, info, rss)
            self.stop.wait(self.args.rss_interval)

    def _check_thresholds(self, pid, info, rss):
        a = self.args
        state = info["state"]
        if state == ProcState.NORMAL and rss >= a.rss_t1:
            # 阈值 1：武装抓栈
            self.b["armed"][ctypes.c_uint32(pid)] = ctypes.c_uint8(1)
            info["state"] = ProcState.ARMED
            info["armed_at"] = time.time()
            self._log("[THRESH-1] pid=%d comm=%s RSS=%.1fMB >= %dMB，开始抓取用户态堆栈"
                      % (pid, info["comm"], rss, a.rss_t1))
        elif state == ProcState.ARMED and rss >= a.rss_t2:
            # 阈值 2：先解析堆栈出报告（进程还活着，maps 可读），再处置
            self._log("[THRESH-2] pid=%d comm=%s RSS=%.1fMB >= %dMB，生成泄漏报告..."
                      % (pid, info["comm"], rss, a.rss_t2))
            report = self._write_report(pid, info, rss)
            self._disarm(pid)
            if a.no_kill:
                info["state"] = ProcState.REPORTED
                self._log("[ACTION] --no-kill 生效，不杀进程。报告: %s" % report)
            else:
                try:
                    os.kill(pid, signal.SIGKILL)
                    info["state"] = ProcState.KILLED
                    self._log("[ACTION] 已 SIGKILL pid=%d。报告: %s" % (pid, report))
                except OSError as ex:
                    info["state"] = ProcState.GONE
                    self._log("[ACTION] kill pid=%d 失败(%s)。报告: %s"
                              % (pid, ex, report))

    def _disarm(self, pid):
        try:
            del self.b["armed"][ctypes.c_uint32(pid)]
        except Exception:
            pass

    # ---------- 报告生成 ----------

    def _load_maps(self, tgid):
        """解析 /proc/<pid>/maps 为 [(lo, hi, file_off, path)]，用于符号化兜底"""
        maps = []
        try:
            with open("/proc/%d/maps" % tgid) as f:
                for line in f:
                    parts = line.split()
                    if len(parts) < 5:
                        continue
                    lo, hi = [int(x, 16) for x in parts[0].split("-")]
                    off = int(parts[2], 16)
                    path = parts[5] if len(parts) > 5 else "[anon]"
                    maps.append((lo, hi, off, path))
        except OSError:
            pass
        return maps

    def _sym(self, addr, tgid, maps):
        """b.sym 符号化；对 [unknown] 的地址（如 libc 内部符号）
        用 maps 计算模块内偏移兜底，输出 module+0xoffset 形式"""
        sym = self.b.sym(addr, tgid, show_module=True, show_offset=True)
        sym = sym.decode(errors="replace")
        if not sym.startswith("[unknown]"):
            return sym
        for lo, hi, off, path in maps:
            if lo <= addr < hi:
                return "%s+0x%x" % (os.path.basename(path), addr - lo + off)
        return sym

    def _top_stacks(self, tgid, topn=10):
        """聚合该进程的栈计数，返回 ([(count, [符号行...])], 总采样数) 降序"""
        counts = []
        for k, v in self.b["stack_counts"].items():
            if k.tgid == tgid and v.value > 0:
                counts.append((k.stack_id, v.value))
        counts.sort(key=lambda x: -x[1])
        maps = self._load_maps(tgid)   # 进程还活着时解析，kill 后 maps 就消失了
        out = []
        for sid, cnt in counts[:topn]:
            lines = []
            for addr in self.b["stack_traces"].walk(sid):
                lines.append(self._sym(addr, tgid, maps))
            out.append((cnt, lines))
        return out, sum(c for _, c in counts)

    def _write_report(self, pid, info, rss):
        a = self.args
        ts = datetime.now().strftime("%Y%m%d_%H%M%S")
        path = os.path.join(a.report_dir, "leak_report_%s_%d_%s.md"
                            % (info["comm"], pid, ts))
        top, total = self._top_stacks(pid, a.top)
        sample = max(1, a.stack_sample)

        with open(path, "w") as f:
            f.write("# 内存泄漏诊断报告\n\n")
            f.write("- 进程: `%s` (pid=%d)\n" % (info["comm"], pid))
            f.write("- 生成时间: %s\n" % datetime.now().strftime("%F %T"))
            f.write("- 触发时 RSS: %.1fMB（阈值 T1=%dMB / T2=%dMB）\n"
                    % (rss, a.rss_t1, a.rss_t2))
            if "armed_at" in info:
                f.write("- 从武装抓栈到触发: %.1fs\n" % (time.time() - info["armed_at"]))
            f.write("- 处置: %s\n\n" % ("SIGKILL" if not a.no_kill else "仅报告(--no-kill)"))

            f.write("## RSS 轨迹（采样间隔 %.1fs）\n\n```\n" % a.rss_interval)
            for t, r in info["history"][-40:]:
                f.write("%s  %8.1f MB  %s\n"
                        % (datetime.fromtimestamp(t).strftime("%H:%M:%S"), r,
                           "#" * int(r / 4)))
            f.write("```\n\n")

            f.write("## Top %d 泄漏堆栈\n\n" % a.top)
            f.write("按用户态页错误采样数排序（采样率 1/%d，"
                    "每采样约对应 %dKB 的 RSS 增长）。共采样 %d 次。\n\n"
                    % (sample, 4 * sample, total))
            for i, (cnt, lines) in enumerate(top, 1):
                est = cnt * 4 * sample / 1024.0
                f.write("### #%d  采样 %d 次（约 %.1fMB，占 %.1f%%）\n\n```\n"
                        % (i, cnt, est, 100.0 * cnt / total if total else 0))
                for line in lines:
                    f.write("%s\n" % line)
                f.write("```\n\n")

            f.write("## 关联事件（最近 50 条）\n\n```\n")
            related = [l for l in self.event_buffer
                       if str(pid) in l or info["comm"] in l][-50:]
            f.write("\n".join(related) if related else "（无）")
            f.write("\n```\n")
        return path

    # ---------- 主循环 ----------

    def run(self):
        self._banner()
        t = threading.Thread(target=self.rss_loop, daemon=True)
        t.start()
        try:
            while True:
                self.b.perf_buffer_poll(timeout=200)
        except KeyboardInterrupt:
            print("\n停止 tracker")
            self.stop.set()
            t.join(timeout=2)
            self.log_fp.close()


def main():
    p = argparse.ArgumentParser(description="eBPF 进程监控与内存泄漏追踪")
    p.add_argument("--pid", type=int, action="append",
                   help="目标进程 pid（可多次指定）")
    p.add_argument("--comm", action="append",
                   help="目标进程名（子串匹配，可多次指定）")
    p.add_argument("--rss-t1", type=float, default=64, help="RSS 阈值1(MB)，超过开始抓栈")
    p.add_argument("--rss-t2", type=float, default=96, help="RSS 阈值2(MB)，超过出报告并杀死")
    p.add_argument("--rss-interval", type=float, default=1.0, help="RSS 采样间隔(秒)")
    p.add_argument("--d-threshold-ms", type=float, default=500, help="D 态告警阈值(毫秒)")
    p.add_argument("--futex-threshold-ms", type=float, default=200, help="等锁告警阈值(毫秒)")
    p.add_argument("--lock-threshold-ms", type=float, default=800, help="持锁告警阈值(毫秒)")
    p.add_argument("--stack-sample", type=int, default=1, help="抓栈采样率：每 N 次页错误抓 1 次")
    p.add_argument("--top", type=int, default=10, help="报告中的 TopN 堆栈数")
    p.add_argument("--track-locks", action="store_true", help="uprobe 跟踪持锁时长(需 --pid)")
    p.add_argument("--no-kill", action="store_true", help="只出报告不杀进程")
    p.add_argument("--report-dir", default="reports", help="报告输出目录")
    p.add_argument("--kprobe", action="store_true",
                   help="用 kprobe 挂点替代 tracepoint（功能对照/开销对比）")
    p.add_argument("--raw-tp", action="store_true",
                   help="用 raw tracepoint 挂点（绕过 perf 框架，生产优化方向）")
    p.add_argument("--verbose", action="store_true", help="D 态事件附带内核栈")
    args = p.parse_args()

    if os.geteuid() != 0:
        print("需要 root 运行（eBPF 需要特权）", file=sys.stderr)
        sys.exit(1)

    Tracker(args).run()


if __name__ == "__main__":
    main()
