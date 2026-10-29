r"""Exit probe (fork-only dev tool): does mcla.exe leave when the game is quit, and if it stays, what is left of it?

Background (2026-10-29): after "Window closing, shutting down..." the game logs "Title terminated; hard-exiting process." and ends with std::_Exit(0) (src/ui/rex_app.cpp of
the SDK, ReXApp::OnClosing). In some runs the process then stays alive with ONE thread, "D3D Background Thread 0" (launcher/dev/exit-hang-2026-10-29.md). This tool runs
short boot-to-free-roam-and-quit runs, measures the time from that log line to the end of the process, and when the process is still there 3 s later records what can be read from
outside: the process's and the thread's kernel state (NtQuerySystemInformation, ThreadIsTerminated, the last system call, IsProcessDeleting), the GPU engine use of the pid, a
user-mode stack scan of the thread (module+offset and the nearest export), a minidump, and what ends the process (each nudge is logged with the time it was made).

  python -I exit_probe.py batch --plan h1 --out <folder> [--deadline-epoch T]
  python -I exit_probe.py batch --plan h2 --out <folder>             (picks the best mode of the h1 results in the same folder)
  python -I exit_probe.py report --out <folder>
  python -I exit_probe.py selftest

Needs MCLA_GAME_DATA, MCLA_DEV_ROOT (scratch save copy, shader cache, run folders) and MCLA_EXE in the environment BEFORE it starts (devrun.py reads them when it is imported);
the chain sets them. Every run plays on a scratch copy of the save, as devrun.py does, and the real data are fingerprinted before and after.

Cells: U = all threads, D3D12; V = all threads, Vulkan; A = affinity 0x55 (4 logical processors) by a job object; S = 0x55 plus 4 busy processes in the same job (stressdef2's
stress, where 11 of 14 runs did not exit). Modes (options added to the command line): 0 = none (the build's defaults), T = --exit_terminate_process=true, D = --exit_gpu_drain_ms=1500,
DT = both. Quit: pause (the pause menu's QUIT GAME), close (WM_CLOSE to the window, as the close button), end (the script ends: the dev pad's quit).
Cap 120, 1x, windowed, intro fast. Results: <out>\results.jsonl (one row per run), <out>\SUMMARY.txt (report), the run folders (game.log, args.txt, diag.json and dump.dmp of a hang) are in <MCLA_DEV_ROOT>\runs\<tag>.
"""
import argparse
import ctypes
import datetime
import json
import os
import re
import statistics
import struct
import subprocess
import sys
import threading
import time
from ctypes import wintypes
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.dont_write_bytecode = True

SCRIPTS = HERE / "scripts"
PAD_OF = {"pause": "exit-pause.pad", "close": "exit-close.pad", "end": "exit-end.pad"}
CELLS = {
    "U": {"api": "d3d12", "aff": None, "hogs": 0, "what": "all threads, D3D12"},
    "V": {"api": "vulkan", "aff": None, "hogs": 0, "what": "all threads, Vulkan"},
    "A": {"api": "d3d12", "aff": 0x55, "hogs": 0, "what": "affinity 0x55 (4 logical processors), D3D12"},
    "S": {"api": "d3d12", "aff": 0x55, "hogs": 4, "what": "0x55 and 4 busy processes, D3D12"},
}
MODES = {
    "0": [],
    "T": ["--exit_terminate_process=true"],
    "D": ["--exit_gpu_drain_ms=1500"],
    "DT": ["--exit_gpu_drain_ms=1500", "--exit_terminate_process=true"],
}
MODE_WHAT = {"0": "as shipped (std::_Exit)", "T": "TerminateProcess", "D": "GPU command processor stopped first (1500 ms)", "DT": "both"}
GOOD_S = 2.0           # a run "exits" when the process is gone within this many seconds of the log line
DIAG_AT = 3.0          # still alive this long after the line: look at it
HANG_LIMIT_S = float(os.environ.get("EXITPROBE_HANG_LIMIT") or 45.0)   # then the run is given up on (the batch waits a little longer for the process, then stops)
RUN_TIMEOUT_S = float(os.environ.get("EXITPROBE_RUN_TIMEOUT") or 150.0)   # from the start to the quit (the pad takes about 60 s)
EST_RUN_S = 75.0       # a run that exits at once, with its start and its end
EST_HANG_S = 50.0      # what a hang adds


def p(*a):
    print(*a, flush=True)


# ---- the plans -------------------------------------------------------------------------------------------------------------------------
def plan_h1(stage, rows):
    """Hold 1 (one stage): how often it happens as shipped, mostly with all threads (cell U), a few runs on Vulkan (V), at 0x55 (A) and in the stressed cell (S, where it
    hung 11 of 14) so that a hang is there to look at. The first run on each API is a warm-up of the shader storage (counted apart)."""
    if stage > 0:
        return None
    return [
        ("U", "0", "pause", True),    # warm-up of the D3D12 storage
        ("S", "0", "pause", False),   # a hang to look at, early
        ("U", "0", "close", False),
        ("U", "0", "pause", False),
        ("A", "0", "close", False),
        ("U", "0", "close", False),
        ("V", "0", "pause", True),    # warm-up of the Vulkan storage
        ("V", "0", "close", False),
        ("U", "0", "pause", False),
        ("S", "0", "close", False),
        ("U", "0", "close", False),
        ("U", "0", "pause", False),
        ("A", "0", "pause", False),
        ("U", "0", "close", False),
        ("U", "0", "pause", False),
        ("U", "0", "close", False),
    ]


def plan_h2(stage, rows):
    """Hold 2. Stage 0: every fix mode twice in the stressed cell S (where hangs are common) and the control (mode 0) once. Stage 1: the mode that did best (the least
    intrusive with no slow exit in S; DT and D if none did) where hangs are rare (U, the bulk), in S twice more and on Vulkan twice."""
    if stage == 0:
        return [("S", "T", "pause", False), ("S", "D", "close", False), ("S", "DT", "pause", False), ("S", "0", "close", False),
                ("S", "T", "close", False), ("S", "D", "pause", False), ("S", "DT", "close", False)]
    if stage == 1:
        best = choose_best(rows)
        modes = [best] if best else ["DT", "D"]
        seq = []
        for i in range(24):
            m = modes[i % len(modes)]
            q = "pause" if i % 2 == 0 else "close"
            if i in (3, 11):
                seq.append(("S", m, q, False))
            elif i in (6, 14):
                seq.append(("V", m, q, i == 6))
            else:
                seq.append(("U", m, q, False))
        return seq
    return None


def choose_best(rows):
    """The least intrusive mode (T, D, DT) with no hang in cell S in at least 2 runs; None if there is none."""
    s = [r for r in rows if r.get("cell") == "S" and not r.get("warm") and not r.get("setup_error")]
    for mode in ("T", "D", "DT"):
        mine = [r for r in s if r["mode"] == mode]
        if len(mine) >= 2 and not any(is_slow(r) for r in mine):
            return mode
    return None


def is_slow(r):
    return r.get("line_to_exit_s") is None or r["line_to_exit_s"] > GOOD_S


# ---- Windows plumbing --------------------------------------------------------------------------------------------------------------------
k32 = ctypes.WinDLL("kernel32", use_last_error=True)
ntdll = ctypes.WinDLL("ntdll", use_last_error=True)
user32 = ctypes.WinDLL("user32", use_last_error=True)
HANDLE = wintypes.HANDLE
k32.OpenProcess.restype = HANDLE
k32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
k32.OpenThread.restype = HANDLE
k32.OpenThread.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
k32.CloseHandle.argtypes = [HANDLE]
k32.WaitForSingleObject.argtypes = [HANDLE, wintypes.DWORD]
k32.WaitForSingleObject.restype = wintypes.DWORD
k32.GetExitCodeProcess.argtypes = [HANDLE, ctypes.POINTER(wintypes.DWORD)]
k32.TerminateProcess.argtypes = [HANDLE, wintypes.UINT]
k32.TerminateProcess.restype = wintypes.BOOL
k32.SuspendThread.argtypes = [HANDLE]
k32.SuspendThread.restype = wintypes.DWORD
k32.ResumeThread.argtypes = [HANDLE]
k32.ResumeThread.restype = wintypes.DWORD
k32.GetThreadContext.argtypes = [HANDLE, ctypes.c_void_p]
k32.GetThreadContext.restype = wintypes.BOOL
k32.ReadProcessMemory.argtypes = [HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
k32.ReadProcessMemory.restype = wintypes.BOOL
k32.CreateJobObjectW.restype = HANDLE
k32.CreateJobObjectW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR]
k32.SetInformationJobObject.argtypes = [HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD]
k32.SetInformationJobObject.restype = wintypes.BOOL
k32.AssignProcessToJobObject.argtypes = [HANDLE, HANDLE]
k32.AssignProcessToJobObject.restype = wintypes.BOOL
k32.TerminateJobObject.argtypes = [HANDLE, wintypes.UINT]
k32.TerminateJobObject.restype = wintypes.BOOL
k32.K32EnumProcessModulesEx.argtypes = [HANDLE, ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), wintypes.DWORD]
k32.K32EnumProcessModulesEx.restype = wintypes.BOOL
k32.K32GetModuleFileNameExW.argtypes = [HANDLE, HANDLE, wintypes.LPWSTR, wintypes.DWORD]
k32.K32GetModuleFileNameExW.restype = wintypes.DWORD
ntdll.NtQuerySystemInformation.argtypes = [ctypes.c_int, ctypes.c_void_p, wintypes.ULONG, ctypes.POINTER(wintypes.ULONG)]
ntdll.NtQuerySystemInformation.restype = ctypes.c_long
ntdll.NtQueryInformationThread.argtypes = [HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.ULONG, ctypes.POINTER(wintypes.ULONG)]
ntdll.NtQueryInformationThread.restype = ctypes.c_long
ntdll.NtQueryInformationProcess.argtypes = [HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.ULONG, ctypes.POINTER(wintypes.ULONG)]
ntdll.NtQueryInformationProcess.restype = ctypes.c_long

SYNCHRONIZE, PROCESS_TERMINATE, PROCESS_QUERY_LIMITED = 0x00100000, 0x0001, 0x1000
PROCESS_QUERY, PROCESS_VM_READ = 0x0400, 0x0010
THREAD_SUSPEND_RESUME, THREAD_GET_CONTEXT, THREAD_QUERY_INFORMATION, THREAD_QUERY_LIMITED = 0x0002, 0x0008, 0x0040, 0x0800
STILL_ACTIVE = 259


def open_for_wait(pid):
    return k32.OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED | PROCESS_TERMINATE, False, pid)


def alive(h):
    return k32.WaitForSingleObject(h, 0) == 0x102


def exit_code(h):
    c = wintypes.DWORD()
    return c.value if k32.GetExitCodeProcess(h, ctypes.byref(c)) else None


class IOC(ctypes.Structure):
    _fields_ = [(n, ctypes.c_ulonglong) for n in ("r", "w", "o", "rb", "wb", "ob")]


class JOBBASIC(ctypes.Structure):
    _fields_ = [("PerProcessUserTimeLimit", ctypes.c_longlong), ("PerJobUserTimeLimit", ctypes.c_longlong), ("LimitFlags", wintypes.DWORD),
                ("MinimumWorkingSetSize", ctypes.c_size_t), ("MaximumWorkingSetSize", ctypes.c_size_t), ("ActiveProcessLimit", wintypes.DWORD),
                ("Affinity", ctypes.c_size_t), ("PriorityClass", wintypes.DWORD), ("SchedulingClass", wintypes.DWORD)]


class JOBEXT(ctypes.Structure):
    _fields_ = [("Basic", JOBBASIC), ("Io", IOC), ("ProcessMemoryLimit", ctypes.c_size_t), ("JobMemoryLimit", ctypes.c_size_t),
                ("PeakProcessMemoryUsed", ctypes.c_size_t), ("PeakJobMemoryUsed", ctypes.c_size_t)]


def make_affinity_job(mask):
    """A job object that limits its processes to the logical processors in `mask` (the SDK undoes a plain inherited affinity) and kills them when its handle closes."""
    job = k32.CreateJobObjectW(None, None)
    if not job:
        raise OSError(f"CreateJobObject: {ctypes.get_last_error()}")
    info = JOBEXT()
    info.Basic.LimitFlags = 0x10 | 0x2000   # JOB_OBJECT_LIMIT_AFFINITY | JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
    info.Basic.Affinity = mask
    if not k32.SetInformationJobObject(job, 9, ctypes.byref(info), ctypes.sizeof(info)):
        raise OSError(f"SetInformationJobObject: {ctypes.get_last_error()}")
    return job


# ---- what can be read from outside about a process that does not leave -------------------------------------------------------------------------
class TH(ctypes.Structure):   # SYSTEM_EXTENDED_THREAD_INFORMATION (x64)
    _fields_ = [("KernelTime", ctypes.c_longlong), ("UserTime", ctypes.c_longlong), ("CreateTime", ctypes.c_longlong), ("WaitTime", wintypes.ULONG), ("pad0", wintypes.ULONG),
                ("StartAddress", ctypes.c_void_p), ("UniqueProcess", ctypes.c_void_p), ("UniqueThread", ctypes.c_void_p), ("Priority", ctypes.c_long),
                ("BasePriority", ctypes.c_long), ("ContextSwitches", wintypes.ULONG), ("ThreadState", wintypes.ULONG), ("WaitReason", wintypes.ULONG), ("pad1", wintypes.ULONG),
                ("StackBase", ctypes.c_void_p), ("StackLimit", ctypes.c_void_p), ("Win32StartAddress", ctypes.c_void_p), ("TebBase", ctypes.c_void_p),
                ("Reserved2", ctypes.c_void_p), ("Reserved3", ctypes.c_void_p), ("Reserved4", ctypes.c_void_p)]


STATE = ["Initialized", "Ready", "Running", "Standby", "Terminated", "Waiting", "Transition", "DeferredReady", "GateWait", "WaitingForProcessInSwap"]
WAIT = ["Executive", "FreePage", "PageIn", "PoolAllocation", "DelayExecution", "Suspended", "UserRequest", "WrExecutive", "WrFreePage", "WrPageIn", "WrPoolAllocation",
        "WrDelayExecution", "WrSuspended", "WrUserRequest", "WrEventPair", "WrQueue", "WrLpcReceive", "WrLpcReply", "WrVirtualMemory", "WrPageOut", "WrRendezvous",
        "WrKeyedEvent", "WrTerminated", "WrProcessInSwap", "WrCpuRateControl", "WrCalloutStack", "WrKernel", "WrResource", "WrPushLock", "WrMutex", "WrQuantumEnd",
        "WrDispatchInt", "WrPreempted", "WrYieldExecution", "WrFastMutex", "WrGuardedMutex", "WrRundown", "WrAlertByThreadId", "WrDeferredPreempt"]


def system_threads(pid):
    """-> (process dict, [thread dict]) for pid from SystemExtendedProcessInformation, or (None, [])."""
    size = 1 << 20
    for _ in range(8):
        buf = ctypes.create_string_buffer(size)
        need = wintypes.ULONG()
        st = ntdll.NtQuerySystemInformation(57, buf, size, ctypes.byref(need))
        if st == 0:
            break
        if st & 0xFFFFFFFF == 0xC0000004:   # STATUS_INFO_LENGTH_MISMATCH
            size = max(size * 2, need.value + (1 << 16))
            continue
        return None, []
    else:
        return None, []
    off = 0
    raw = buf.raw
    while True:
        nxt, nthreads = struct.unpack_from("<II", raw, off)
        this_pid = struct.unpack_from("<Q", raw, off + 80)[0]
        if this_pid == pid:
            handles = struct.unpack_from("<I", raw, off + 96)[0]
            ws, priv = struct.unpack_from("<QQ", raw, off + 144)[0], struct.unpack_from("<Q", raw, off + 200)[0]
            threads = []
            t_off = off + 256
            for i in range(nthreads):
                t = TH.from_buffer_copy(raw, t_off + i * ctypes.sizeof(TH))
                threads.append({
                    "tid": t.UniqueThread, "state": STATE[t.ThreadState] if t.ThreadState < len(STATE) else t.ThreadState,
                    "wait_reason": WAIT[t.WaitReason] if t.WaitReason < len(WAIT) else t.WaitReason, "wait_time_ticks": t.WaitTime,
                    "ctx_switches": t.ContextSwitches, "priority": t.Priority, "base_priority": t.BasePriority,
                    "kernel_ms": round(t.KernelTime / 1e4, 1), "user_ms": round(t.UserTime / 1e4, 1),
                    "kernel_start": hex(t.StartAddress or 0), "win32_start": t.Win32StartAddress or 0, "stack_base": t.StackBase or 0, "stack_limit": t.StackLimit or 0,
                    "teb": hex(t.TebBase or 0)})
            return {"handles": handles, "working_set": ws, "private": priv, "threads": nthreads}, threads
        if not nxt:
            return None, []
        off += nxt


def thread_extra(tid):
    """ThreadIsTerminated and the last system call of a thread."""
    out = {}
    h = k32.OpenThread(THREAD_QUERY_INFORMATION | THREAD_QUERY_LIMITED, False, tid)
    if not h:
        return {"open_error": ctypes.get_last_error()}
    try:
        term = wintypes.ULONG()
        st = ntdll.NtQueryInformationThread(h, 20, ctypes.byref(term), 4, None)   # ThreadIsTerminated
        out["is_terminated"] = term.value if st == 0 else f"status {st & 0xFFFFFFFF:#x}"
        buf = ctypes.create_string_buffer(32)
        st = ntdll.NtQueryInformationThread(h, 21, buf, 32, None)                 # ThreadLastSystemCall
        if st == 0:
            first_arg, number = struct.unpack_from("<QH", buf.raw, 0)
            out["last_syscall"] = number
            out["last_syscall_first_arg"] = hex(first_arg)
        else:
            out["last_syscall"] = f"status {st & 0xFFFFFFFF:#x}"
    finally:
        k32.CloseHandle(h)
    return out


def process_flags(pid):
    """PROCESS_EXTENDED_BASIC_INFORMATION: the exit status and the flags (IsProcessDeleting is bit 2)."""
    h = k32.OpenProcess(PROCESS_QUERY_LIMITED, False, pid)
    if not h:
        return {"open_error": ctypes.get_last_error()}
    try:
        buf = ctypes.create_string_buffer(64)
        struct.pack_into("<Q", buf, 0, 64)
        st = ntdll.NtQueryInformationProcess(h, 0, buf, 64, None)
        if st != 0:
            return {"status": hex(st & 0xFFFFFFFF)}
        exit_status = struct.unpack_from("<i", buf.raw, 8)[0]
        flags = struct.unpack_from("<I", buf.raw, 56)[0]
        names = ["protected", "wow64", "deleting", "cross_session_create", "frozen", "background", "strongly_named", "secure", "subsystem"]
        return {"exit_status": exit_status, "flags": hex(flags), "flags_set": [n for i, n in enumerate(names) if flags & (1 << i)]}
    finally:
        k32.CloseHandle(h)


_exports_cache = {}


def pe_exports(path):
    """[(rva, name)] sorted, from the file on disk (the export names of a DLL, for the nearest-symbol lookup)."""
    if path in _exports_cache:
        return _exports_cache[path]
    out = []
    try:
        data = Path(path).read_bytes()
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        n_sections = struct.unpack_from("<H", data, pe + 6)[0]
        opt_size = struct.unpack_from("<H", data, pe + 20)[0]
        opt = pe + 24
        magic = struct.unpack_from("<H", data, opt)[0]
        dd = opt + (112 if magic == 0x20B else 96)
        exp_rva, exp_size = struct.unpack_from("<II", data, dd)
        sections = []
        for i in range(n_sections):
            s = opt + opt_size + i * 40
            vsize, vaddr, rsize, raddr = struct.unpack_from("<IIII", data, s + 8)
            sections.append((vaddr, max(vsize, rsize), raddr))

        def rva2off(rva):
            for va, sz, ra in sections:
                if va <= rva < va + sz:
                    return rva - va + ra
            return None
        if exp_rva:
            e = rva2off(exp_rva)
            n_funcs, n_names, f_rva, n_rva, o_rva = struct.unpack_from("<IIIII", data, e + 20)
            f_off, n_off, o_off = rva2off(f_rva), rva2off(n_rva), rva2off(o_rva)
            for i in range(n_names):
                name_rva = struct.unpack_from("<I", data, n_off + 4 * i)[0]
                ordinal = struct.unpack_from("<H", data, o_off + 2 * i)[0]
                func_rva = struct.unpack_from("<I", data, f_off + 4 * ordinal)[0]
                no = rva2off(name_rva)
                name = data[no:data.index(b"\0", no)].decode("ascii", "replace")
                if not (exp_rva <= func_rva < exp_rva + exp_size):   # not a forwarder
                    out.append((func_rva, name))
    except Exception:
        pass
    out.sort()
    _exports_cache[path] = out
    return out


def nearest_export(path, rva):
    ex = pe_exports(path)
    best = None
    for r, n in ex:
        if r <= rva:
            best = (r, n)
        else:
            break
    return f"{best[1]}+{rva - best[0]:#x}" if best and rva - best[0] < 0x10000 else None


_syscalls = {}


def syscall_names():
    """{number: name} from the Nt*/Zw* stubs of ntdll.dll and win32u.dll on this PC (mov r10, rcx; mov eax, N)."""
    if _syscalls:
        return _syscalls
    sysdir = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32"
    for dll in ("ntdll.dll", "win32u.dll"):
        path = sysdir / dll
        try:
            data = path.read_bytes()
        except OSError:
            continue
        # export rva -> file offset through the same section walk as pe_exports
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        n_sections = struct.unpack_from("<H", data, pe + 6)[0]
        opt_size = struct.unpack_from("<H", data, pe + 20)[0]
        sections = []
        for i in range(n_sections):
            s = pe + 24 + opt_size + i * 40
            vsize, vaddr, rsize, raddr = struct.unpack_from("<IIII", data, s + 8)
            sections.append((vaddr, max(vsize, rsize), raddr))
        for rva, name in pe_exports(str(path)):
            off = None
            for va, sz, ra in sections:
                if va <= rva < va + sz:
                    off = rva - va + ra
                    break
            if off is None or data[off:off + 4] != b"\x4c\x8b\xd1\xb8":
                continue
            _syscalls.setdefault(struct.unpack_from("<I", data, off + 4)[0], name)
    return _syscalls


def modules_of(h):
    """[(base, size, path)] of a process, or [] when it can't be read."""
    arr = (ctypes.c_void_p * 2048)()
    need = wintypes.DWORD()
    if not k32.K32EnumProcessModulesEx(h, arr, ctypes.sizeof(arr), ctypes.byref(need), 3):
        return []
    out = []
    psapi = ctypes.WinDLL("psapi", use_last_error=True)
    class MI(ctypes.Structure):
        _fields_ = [("base", ctypes.c_void_p), ("size", wintypes.DWORD), ("entry", ctypes.c_void_p)]
    psapi.GetModuleInformation.argtypes = [HANDLE, HANDLE, ctypes.POINTER(MI), wintypes.DWORD]
    for i in range(min(need.value // ctypes.sizeof(ctypes.c_void_p), 2048)):
        mi = MI()
        name = ctypes.create_unicode_buffer(520)
        if psapi.GetModuleInformation(h, arr[i], ctypes.byref(mi), ctypes.sizeof(mi)) and k32.K32GetModuleFileNameExW(h, arr[i], name, 520):
            out.append((mi.base or 0, mi.size, name.value))
    return out


def where(mods, addr):
    for base, size, path in mods:
        if base <= addr < base + size:
            rva = addr - base
            return {"module": Path(path).name, "rva": hex(rva), "near": nearest_export(path, rva)}
    return None


def stack_scan(pid, th, deadline_s=8.0):
    """The lone thread's stack from the outside: its RIP/RSP (if it can be suspended and its context read) and the values on its stack that point into a module (a heuristic:
    return addresses and stale ones both show up), lowest address first = most recent."""
    out = {"tid": th["tid"]}
    h = k32.OpenProcess(PROCESS_QUERY | PROCESS_VM_READ | PROCESS_QUERY_LIMITED, False, pid)
    if not h:
        out["process_open_error"] = ctypes.get_last_error()
        return out
    try:
        mods = modules_of(h)
        out["modules"] = len(mods)
        th_h = k32.OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, False, th["tid"])
        rsp = rip = 0
        if th_h:
            try:
                n = k32.SuspendThread(th_h)
                out["suspend_prev_count"] = n if n != 0xFFFFFFFF else f"failed {ctypes.get_last_error()}"
                if n != 0xFFFFFFFF:
                    try:
                        raw = ctypes.create_string_buffer(1232 + 32)
                        addr = ctypes.addressof(raw)
                        aligned = (addr + 15) & ~15
                        ctypes.c_uint32.from_address(aligned + 0x30).value = 0x100003   # CONTEXT_AMD64 | CONTROL | INTEGER
                        if k32.GetThreadContext(th_h, ctypes.c_void_p(aligned)):
                            rsp = ctypes.c_uint64.from_address(aligned + 0x98).value
                            rip = ctypes.c_uint64.from_address(aligned + 0xF8).value
                            out["rip"], out["rsp"] = hex(rip), hex(rsp)
                            out["rip_in"] = where(mods, rip)
                        else:
                            out["context_error"] = ctypes.get_last_error()
                    finally:
                        k32.ResumeThread(th_h)
            finally:
                k32.CloseHandle(th_h)
        else:
            out["thread_open_error"] = ctypes.get_last_error()
        base = rsp or max(0, (th["stack_base"] or 0) - 0x8000)
        out["scan_from"] = hex(base)
        want = 0x8000 if not rsp else 0x4000
        buf = ctypes.create_string_buffer(want)
        got = ctypes.c_size_t()
        if base and k32.ReadProcessMemory(h, ctypes.c_void_p(base), buf, want, ctypes.byref(got)) or got.value:
            n = got.value // 8
            vals = struct.unpack_from(f"<{n}Q", buf.raw, 0)
            hits = []
            for i, v in enumerate(vals):
                if v < 0x10000:
                    continue
                w = where(mods, v)
                if w:
                    hits.append({"at": f"rsp+{i * 8:#x}" if rsp else f"base+{i * 8:#x}", "value": hex(v), **w})
                if len(hits) >= 40:
                    break
            out["stack_hits"] = hits
        else:
            out["stack_read_error"] = ctypes.get_last_error()
    finally:
        k32.CloseHandle(h)
    return out


def write_dump(pid, path, limit_s=25.0):
    """A minidump of the process (stacks, thread info, handles, no heap), in a helper thread so a dump that blocks costs limit_s and not the batch."""
    result = {"path": str(path)}

    def work():
        h = k32.OpenProcess(0x1F0FFF, False, pid)
        if not h:
            result["error"] = f"OpenProcess {ctypes.get_last_error()}"
            return
        try:
            dbghelp = ctypes.WinDLL("dbghelp", use_last_error=True)
            dbghelp.MiniDumpWriteDump.argtypes = [HANDLE, wintypes.DWORD, HANDLE, wintypes.DWORD, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
            dbghelp.MiniDumpWriteDump.restype = wintypes.BOOL
            import msvcrt
            with open(path, "wb") as f:
                ok = dbghelp.MiniDumpWriteDump(h, pid, msvcrt.get_osfhandle(f.fileno()), 0x1924, None, None, None)
                result["ok"] = bool(ok)
                if not ok:
                    result["error"] = f"MiniDumpWriteDump {ctypes.get_last_error()}"
        except Exception as e:  # noqa: BLE001
            result["error"] = repr(e)
        finally:
            k32.CloseHandle(h)

    t = threading.Thread(target=work, daemon=True)
    t.start()
    t.join(limit_s)
    if t.is_alive():
        result["error"] = f"still writing after {limit_s:.0f} s (given up on)"
    try:
        result["bytes"] = Path(path).stat().st_size
    except OSError:
        pass
    return result


def snapshot(pid, label, gpu=None, mods_h=None):
    """What the kernel says about the process and its threads right now."""
    snap = {"label": label, "t": time.time()}
    proc, threads = system_threads(pid)
    snap["process"] = proc
    snap["process_flags"] = process_flags(pid)
    sysnames = syscall_names()
    rows = []
    for th in threads:
        row = {k: v for k, v in th.items() if k not in ("win32_start", "stack_base", "stack_limit")}
        row.update(thread_extra(th["tid"]))
        n = row.get("last_syscall")
        if isinstance(n, int):
            row["last_syscall_name"] = sysnames.get(n, "?")
        rows.append(row)
    snap["threads"] = rows
    if gpu is not None:
        try:
            _, usage = gpu.read()
            snap["gpu_percent_of_pid"] = round(usage.get(pid, -1.0), 2)
        except Exception as e:  # noqa: BLE001
            snap["gpu_error"] = repr(e)
    return snap, threads


def diagnose(pid, since, gpu, run_dir):
    """The whole look at a process that is still there DIAG_AT seconds after the line: two snapshots 2 s apart (context switches say how often the thread wakes), the stack
    scan of every thread (normally one), a minidump."""
    diag = {"pid": pid}
    snap1, threads = snapshot(pid, f"+{time.time() - since:.1f}s", gpu)
    diag["snapshots"] = [snap1]
    if gpu is not None:
        try:
            gpu.read()
        except Exception:  # noqa: BLE001
            pass
    time.sleep(2.0)
    snap2, threads2 = snapshot(pid, f"+{time.time() - since:.1f}s", gpu)
    diag["snapshots"].append(snap2)
    for a in snap1["threads"]:
        for b in snap2["threads"]:
            if a["tid"] == b["tid"]:
                b["ctx_switches_per_s"] = round((b["ctx_switches"] - a["ctx_switches"]) / max(1e-3, snap2["t"] - snap1["t"]), 1)
    diag["stacks"] = [stack_scan(pid, th) for th in (threads2 or threads)[:4]]
    diag["dump"] = write_dump(pid, run_dir / "dump.dmp")
    return diag


# ---- one run -----------------------------------------------------------------------------------------------------------------------------
TS = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3})\]")


def line_epoch(line):
    m = TS.match(line)
    if not m:
        return None
    return datetime.datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S.%f").timestamp()


class LogTail:
    def __init__(self, path):
        self.path = Path(path)
        self.pos = 0
        self.rest = b""
        self.close_t = self.line_t = self.exec_t = None
        self.drain = None
        self.lines = 0

    def poll(self):
        try:
            with open(self.path, "rb") as f:
                f.seek(self.pos)
                data = f.read()
                self.pos += len(data)
        except OSError:
            return
        data = self.rest + data
        parts = data.split(b"\n")
        self.rest = parts.pop()
        for raw in parts:
            self.lines += 1
            s = raw.decode("utf-8", "replace")
            if "Window closing, shutting down" in s and self.close_t is None:
                self.close_t = line_epoch(s)
            elif "Execution complete" in s:
                self.exec_t = line_epoch(s)
            elif "Exit: the GPU command processor stopped in" in s:
                self.drain = {"ok": True, "ms": int(re.search(r"in (\d+) ms", s).group(1))}
            elif "Exit: the GPU command processor did not stop within" in s:
                self.drain = {"ok": False, "ms": int(re.search(r"within (\d+) ms", s).group(1))}
            elif "hard-exiting process" in s and self.line_t is None:
                self.line_t = line_epoch(s)


def game_args(run, out, exe_dir):
    ns = argparse.Namespace(
        backend=CELLS[run["cell"]]["api"], res="1", fps="120", fps_fg=None, vsync="false", fullscreen=False, intro="fast", present_log=False,
        cache_root=None, script=str(SCRIPTS / PAD_OF[run["quit"]]), keep_open=False, pipe="", fixed_dt=0, record=None, track=False, extra=list(MODES[run["mode"]]))
    return ns, devrun.game_args(ns, out)


def other_mcla_pids():
    return [pid for pid, (parent, name) in devrun.process_table().items() if name.lower() == "mcla.exe"]


def run_one(run):
    """-> the result row. Never raises for a run that went wrong: the row says what."""
    tag = run["tag"]
    out = devrun.RUNS / tag
    out.mkdir(parents=True, exist_ok=True)
    row = dict(run)
    row.update({"t_start": time.time(), "host": os.environ.get("COMPUTERNAME")})
    cell = CELLS[run["cell"]]
    # no other mcla.exe: the owner's game or a zombie of ours would share the GPU (and the screen)
    waited = 0
    while other_mcla_pids() and waited < 120:
        time.sleep(2)
        waited += 2
    if other_mcla_pids():
        row["setup_error"] = f"another mcla.exe is alive (pids {other_mcla_pids()}) after {waited} s"
        return row
    devrun.prepare()
    devrun.restore_work()
    before = devrun.fingerprint()
    toml_backup = devrun.TOML.read_bytes() if devrun.TOML.exists() else None
    ns, args = game_args(run, out, None)
    (out / "args.txt").write_text(" ".join(args), encoding="utf-8")
    env = dict(os.environ, REX_PERF_LOG_CSV=str(out / "perf.csv"), REX_USER_DATA_ROOT=str(devrun.WORK), REX_CACHE_ROOT=str(devrun.CACHE),
               REX_MCLA_GPU_BACKEND=ns.backend, REX_GAME_DATA_ROOT=str(devrun.GAME_DATA))
    job = hogs = None
    proc = None
    gpu = devrun.gpu_counters()
    try:
        if cell["aff"]:
            job = make_affinity_job(cell["aff"])
        fake = os.environ.get("EXITPROBE_FAKE_GAME")   # the tool's own test: a Python script stands in for the game (tools\fake_game.py of the author)
        exe_cmd = [sys.executable, "-I", fake, *args] if fake else [str(devrun.EXE), *args]
        proc = subprocess.Popen(exe_cmd, cwd=None if fake else devrun.EXE.parent, env=env)
        if job and not k32.AssignProcessToJobObject(job, int(proc._handle)):
            row["setup_error"] = f"AssignProcessToJobObject {ctypes.get_last_error()}"
        if cell["hogs"] and not row.get("setup_error"):
            hogs = []
            code = os.environ.get("EXITPROBE_HOG_CODE") or "while True: pass"
            for _ in range(cell["hogs"]):
                hp = subprocess.Popen([sys.executable, "-I", "-c", code], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                if not k32.AssignProcessToJobObject(job, int(hp._handle)):
                    row["setup_error"] = f"hog into the job {ctypes.get_last_error()}"
                hogs.append(hp)
        pid = proc.pid
        row["pid"] = pid
        h = open_for_wait(pid)
        log = LogTail(out / "game.log")
        t0 = time.time()
        if gpu:
            try:
                gpu.read()   # the first read only primes the rates
            except Exception:  # noqa: BLE001
                pass
        diag_done = False
        t_diag_end = 0.0
        nudges = []
        state = "running"
        t_dead = None
        while True:
            log.poll()
            now = time.time()
            if not alive(h):
                t_dead = now
                break
            if log.line_t is None and now - t0 > RUN_TIMEOUT_S:
                row["quit_failed"] = f"no 'hard-exiting process' {RUN_TIMEOUT_S:.0f} s after the start"
                devrun.close_windows(pid)
                k = time.time()
                while alive(h) and time.time() - k < 30:
                    time.sleep(0.2)
                if alive(h):
                    k32.TerminateProcess(h, 1)
                    time.sleep(2)
                t_dead = time.time()
                break
            if log.line_t is not None:
                since = now - log.line_t
                if since >= DIAG_AT and not diag_done:
                    diag_done = True
                    row["diag"] = diagnose(pid, log.line_t, gpu, out)
                    (out / "diag.json").write_text(json.dumps(row["diag"], indent=1, default=str), encoding="utf-8")
                    log.poll()
                    t_diag_end = time.time()
                # the nudges: what ends it? Each is made once, 4, 9, 14 and 19 s after the diagnosis ended, and logged with the time since the line.
                due = [(4.0, "TerminateProcess(handle, 1)"), (9.0, "close_windows (WM_CLOSE)"), (14.0, "TerminateJobObject"), (19.0, "Popen.kill()")]
                for at, what in due:
                    if diag_done and now - t_diag_end >= at and what not in [n["what"] for n in nudges]:
                        rec = {"what": what, "at_s": round(since, 1)}
                        if what.startswith("TerminateProcess"):
                            rec["ok"] = bool(k32.TerminateProcess(h, 1))
                            if not rec["ok"]:
                                rec["error"] = ctypes.get_last_error()
                        elif what.startswith("close_windows"):
                            devrun.close_windows(pid)
                        elif what == "TerminateJobObject" and job:
                            rec["ok"] = bool(k32.TerminateJobObject(job, 1))
                            if not rec["ok"]:
                                rec["error"] = ctypes.get_last_error()
                        elif what == "Popen.kill()":
                            try:
                                proc.kill()
                                rec["ok"] = True
                            except Exception as e:  # noqa: BLE001
                                rec["ok"] = False
                                rec["error"] = repr(e)
                        nudges.append(rec)
                if since > HANG_LIMIT_S:
                    row["zombie"] = True
                    break
            time.sleep(0.02 if log.line_t is None or now - log.line_t < DIAG_AT else 0.1)
        log.poll()
        if nudges:
            row["nudges"] = nudges
        row["exit_code"] = exit_code(h) if t_dead else None
        row["t_close_line"], row["t_hard_exit_line"], row["t_exec_complete"] = log.close_t, log.line_t, log.exec_t
        if log.drain:
            row["gpu_stop"] = log.drain
        if t_dead is not None and log.line_t is None and not row.get("quit_failed"):
            row["no_line"] = "the process ended without 'hard-exiting process' in its log (crash?)"
        if t_dead is not None and log.line_t is not None:
            row["line_to_exit_s"] = round(max(0.0, t_dead - log.line_t), 3)
        if t_dead is not None and log.close_t is not None:
            row["close_to_exit_s"] = round(max(0.0, t_dead - log.close_t), 3)
        if log.close_t is not None and log.line_t is not None:
            row["close_to_line_s"] = round(log.line_t - log.close_t, 3)
        if row.get("zombie"):
            # give the batch a chance to go on: wait a while longer for the process, then the batch stops
            k = time.time()
            while alive(h) and time.time() - k < 60:
                time.sleep(0.5)
            row["zombie_gone_after_limit_s"] = round(time.time() - k, 1) if not alive(h) else None
            if not alive(h):
                row["zombie_total_s"] = round(time.time() - (log.line_t or t0), 1)
        k32.CloseHandle(h)
        row["seconds"] = round(time.time() - t0, 1)
        try:
            text = (out / "game.log").read_text(encoding="utf-8", errors="replace")
            row["script_status"] = [ln.split("dev pad: script ", 1)[1].strip() for ln in text.splitlines() if "dev pad: script " in ln][-1:]
            evidence = {"pause": "settings menu: quit requested", "close": "exit: window close", "end": "exit: end of the script"}[run["quit"]]
            if evidence not in text and not row.get("quit_failed"):
                row["quit_failed"] = f"the pad did not quit by {run['quit']} (no {evidence!r} in the log; script: {row['script_status']}); the dev pad's end-of-script quit ended the game"
        except (OSError, IndexError):
            pass
    except Exception as e:  # noqa: BLE001
        import traceback
        row["error"] = traceback.format_exc()[-1500:]
    finally:
        if hogs:
            for hp in hogs:
                try:
                    hp.kill()
                except Exception:  # noqa: BLE001
                    pass
        if job:
            # KILL_ON_JOB_CLOSE ends what is left of the job (the hogs; a game that hangs is not touched by it)
            k32.CloseHandle(job)
        try:
            if devrun.TOML.exists():
                (out / "mcla.toml.after").write_bytes(devrun.TOML.read_bytes())
        finally:
            if toml_backup is not None:
                devrun.TOML.write_bytes(toml_backup)
            elif devrun.TOML.exists():
                devrun.TOML.unlink()
    after = devrun.fingerprint()
    if before != after:
        row["REAL_DATA_CHANGED"] = [k for k in set(before) | set(after) if before.get(k) != after.get(k)]
    row["t_end"] = time.time()
    return row


# ---- the batch and the report ------------------------------------------------------------------------------------------------------------
def read_results(out):
    f = Path(out) / "results.jsonl"
    rows = []
    if f.exists():
        for ln in f.read_text(encoding="utf-8").splitlines():
            try:
                rows.append(json.loads(ln))
            except ValueError:
                pass
    return rows


def batch(args):
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    planner = {"h1": plan_h1, "h2": plan_h2}.get(args.plan)
    if planner is None:
        raise SystemExit(f"unknown plan {args.plan!r}")
    deadline = args.deadline_epoch or (time.time() + 25 * 60)
    p(f"exit_probe: plan {args.plan}, deadline {datetime.datetime.fromtimestamp(deadline):%H:%M:%S}; exe {devrun.EXE}")
    zombie_left = stop = False
    rows = read_results(out)
    pause_broken = any(r.get("quit") == "pause" and r.get("quit_failed") for r in rows)
    failed_in_a_row = 0
    for stage in range(4):
        rows = read_results(out)
        seq = planner(stage, rows)
        if seq is None or stop:
            break
        done = {r["tag"] for r in rows if not r.get("setup_error")}
        p(f"exit_probe: stage {stage}: {len(seq)} runs planned")
        for i, (cell, mode, quit_how, warm) in enumerate(seq, 1):
            tag = f"{args.plan}-{stage}{i:02d}-{cell}{mode}-{quit_how}"
            if tag in done:
                continue
            if quit_how == "pause" and pause_broken:
                quit_how = "close"   # the pause menu pad did not quit once: the rest of the budget goes to runs that do
                tag = f"{args.plan}-{stage}{i:02d}-{cell}{mode}-close-instead-of-pause"
            remaining = deadline - time.time()
            need = EST_RUN_S + (EST_HANG_S if mode == "0" and cell in ("S", "A") else 0)
            if remaining < need:
                p(f"{tag}: not started, {remaining:.0f} s left and the run needs about {need:.0f} s")
                continue
            run = {"tag": tag, "plan": args.plan, "stage": stage, "cell": cell, "mode": mode, "quit": quit_how, "warm": warm}
            p(f"{tag}: {CELLS[cell]['what']}; mode {mode} ({MODE_WHAT[mode]}); quit by {quit_how}{' (warm-up)' if warm else ''}")
            row = run_one(run)
            with open(out / "results.jsonl", "a", encoding="utf-8") as f:
                f.write(json.dumps(row, default=str) + "\n")
            p(f"{tag}: line_to_exit_s={row.get('line_to_exit_s')} close_to_line_s={row.get('close_to_line_s')} gpu_stop={row.get('gpu_stop')} exit_code={row.get('exit_code')}"
              f" zombie={row.get('zombie', False)}"
              f" {('ERROR ' + row['error'].splitlines()[-1]) if row.get('error') else ''}{(' SETUP ' + row['setup_error']) if row.get('setup_error') else ''}"
              f"{(' QUIT-FAILED') if row.get('quit_failed') else ''}{(' NO-LINE') if row.get('no_line') else ''}")
            if row.get("REAL_DATA_CHANGED"):
                p(f"STOP: the real data changed: {row['REAL_DATA_CHANGED']}")
                stop = True
                break
            if row.get("zombie") and row.get("zombie_gone_after_limit_s") is None:
                p(f"STOP: pid {row.get('pid')} is still alive {HANG_LIMIT_S + 60:.0f} s after the line; the batch ends (a zombie holds the GPU and the shader storage)")
                zombie_left = stop = True
                break
            if row.get("quit_failed") and run["quit"] == "pause":
                pause_broken = True
            failed_in_a_row = failed_in_a_row + 1 if (row.get("error") or row.get("quit_failed") or row.get("no_line")) else 0
            if failed_in_a_row >= 3:
                p("STOP: three runs in a row failed (an error, a quit that did not happen, or a crash before the exit line); see results.jsonl")
                stop = True
                break
            if row.get("setup_error"):
                p(f"STOP: {row['setup_error']}")
                stop = True
                break
    text = report(read_results(out))
    (out / "SUMMARY.txt").write_text(text, encoding="utf-8")
    p(text)
    return 3 if zombie_left else 0


def pct(sorted_vals, q):
    if not sorted_vals:
        return None
    i = min(len(sorted_vals) - 1, int(round(q * (len(sorted_vals) - 1))))
    return sorted_vals[i]


def report(rows):
    lines = []
    L = lines.append
    good = [r for r in rows if not r.get("setup_error") and not r.get("error") and not r.get("quit_failed") and not r.get("no_line")]
    bad = [r for r in rows if r not in good]
    L("EXITHANG runs: %d (%d usable, %d with a setup error, a script error or a quit that did not happen)" % (len(rows), len(good), len(bad)))
    for r in bad:
        why = r.get("setup_error") or r.get("quit_failed") or r.get("no_line") or ((r.get("error") or "").splitlines() or ["?"])[-1]
        L("EXITHANG   not usable: %s: %s" % (r["tag"], why))
    cells = {}
    for r in good:
        cells.setdefault((r["cell"], r["mode"], bool(r.get("warm"))), []).append(r)
    L("EXITHANG per cell and mode (line_to_exit_s = from 'Title terminated; hard-exiting process.' to the end of the process; SLOW = more than %.0f s or never):" % GOOD_S)
    for (cell, mode, warm), rs in sorted(cells.items(), key=lambda kv: (kv[0][0], kv[0][1], kv[0][2])):
        xs = sorted(r["line_to_exit_s"] for r in rs if r.get("line_to_exit_s") is not None)
        slow = [r for r in rs if is_slow(r)]
        by_quit = {}
        for r in rs:
            by_quit.setdefault(r["quit"], [0, 0])
            by_quit[r["quit"]][0] += 1
            by_quit[r["quit"]][1] += 1 if is_slow(r) else 0
        L("EXITHANG   %s%s mode %-2s %-34s runs %2d  SLOW %2d  exit s: %s  [by quit: %s]%s" % (
            cell, " warm-up" if warm else "", mode, "(" + MODE_WHAT[mode] + ")", len(rs), len(slow),
            ("median %.3f max %.3f" % (statistics.median(xs), xs[-1])) if xs else "none ended",
            ", ".join("%s %d/%d slow" % (k, v[1], v[0]) for k, v in sorted(by_quit.items())),
            "  [cells: %s]" % CELLS[cell]["what"]))
    stopping = []
    for r in good:
        gs = r.get("gpu_stop")
        if gs:
            stopping.append((r["tag"], gs))
    if stopping:
        L("EXITHANG GPU stop (modes D, DT): " + "; ".join("%s %s %d ms" % (t, "stopped" if g["ok"] else "TIMED OUT", g["ms"]) for t, g in stopping))
    cts = sorted(r["close_to_line_s"] for r in good if r.get("close_to_line_s") is not None)
    if cts:
        L("EXITHANG 'Window closing' to the hard-exit line (TerminateTitle and, with D, the GPU stop): median %.3f s, max %.3f s over %d runs" % (statistics.median(cts), cts[-1], len(cts)))
    odd = [(r["tag"], r.get("exit_code")) for r in good if r.get("exit_code") not in (0, None)]
    if odd:
        L("EXITHANG NONZERO EXIT CODE (a crash on the way out?): " + "; ".join("%s code %s" % (t, c) for t, c in odd))
    # the frequency without starvation
    u = [r for r in good if r["cell"] in ("U", "V") and r["mode"] == "0" and not r.get("warm")]
    if u:
        L("EXITHANG FREQUENCY, all threads, as shipped (modes 0 in cells U and V, warm-ups left out): %d of %d runs SLOW" % (sum(1 for r in u if is_slow(r)), len(u)))
    for cell in ("A", "S"):
        a = [r for r in good if r["cell"] == cell and r["mode"] == "0" and not r.get("warm")]
        if a:
            L("EXITHANG FREQUENCY, cell %s as shipped: %d of %d runs SLOW" % (cell, sum(1 for r in a if is_slow(r)), len(a)))
    # what a hang looked like
    hangs = [r for r in rows if r.get("diag")]
    for r in hangs:
        d = r["diag"]
        s2 = d["snapshots"][-1] if d.get("snapshots") else {}
        L("EXITHANG HANG %s (cell %s mode %s quit %s): ended %s s after the line%s; exit code %s" % (
            r["tag"], r["cell"], r["mode"], r["quit"], r.get("line_to_exit_s"), " (NEVER within %.0f s; the batch gave it %s s more)" % (HANG_LIMIT_S, r.get("zombie_gone_after_limit_s")) if r.get("zombie") else "",
            r.get("exit_code")))
        L("EXITHANG    process flags: %s; threads left: %s" % (s2.get("process_flags"), len(s2.get("threads", []))))
        for t in s2.get("threads", []):
            L("EXITHANG    thread %s: %s/%s, wakes/s %s, is_terminated %s, last syscall %s (%s), kernel %s ms user %s ms" % (
                t["tid"], t["state"], t["wait_reason"], t.get("ctx_switches_per_s"), t.get("is_terminated"), t.get("last_syscall"), t.get("last_syscall_name"), t["kernel_ms"], t["user_ms"]))
        if "gpu_percent_of_pid" in s2:
            L("EXITHANG    GPU engine use of the lingering pid: %s %%" % s2["gpu_percent_of_pid"])
        for st in d.get("stacks", []):
            L("EXITHANG    stack of %s: rip %s %s; context error %s" % (st.get("tid"), st.get("rip"), st.get("rip_in"), st.get("context_error")))
            for hit in (st.get("stack_hits") or [])[:12]:
                L("EXITHANG      %s %s %s+%s %s" % (hit["at"], hit["value"], hit["module"], hit["rva"], hit.get("near") or ""))
        L("EXITHANG    dump: %s" % d.get("dump"))
        L("EXITHANG    nudges (what was done and when): %s" % r.get("nudges"))
    # does a mode fix it
    for mode in ("T", "D", "DT"):
        rs = [r for r in good if r["mode"] == mode and not r.get("warm")]
        if not rs:
            continue
        s_rs = [r for r in rs if r["cell"] == "S"]
        all_fast = all(not is_slow(r) for r in rs)
        L("EXITHANG MODE %s (%s): %d runs (%d in the stressed cell S), exit within %.0f s in every run: %s" % (mode, MODE_WHAT[mode], len(rs), len(s_rs), GOOD_S, "YES" if all_fast else "NO"))
    best = choose_best(good)
    L("EXITHANG BEST MODE so far (least intrusive with no slow exit in at least 2 stressed runs): %s" % (best or "none"))
    mine = [r for r in good if best and r["mode"] == best and not r.get("warm")]
    s_mine = [r for r in mine if r["cell"] == "S"]
    u_mine = [r for r in mine if r["cell"] in ("U", "V")]
    if best and mine and all(not is_slow(r) for r in mine) and len(s_mine) >= 4 and len(u_mine) >= 8:
        L("EXITHANG DEFAULT: mode %s (%s) exited within %.0f s in all %d runs (%d stressed, %d unstarved): its option can be turned on by default" % (
            best, MODE_WHAT[best], GOOD_S, len(mine), len(s_mine), len(u_mine)))
    else:
        L("EXITHANG DEFAULT: NOT enough to turn an option on by default (needs the best mode %s with every run within %.0f s, at least 4 in cell S and 8 unstarved): it has %d runs, %d in S, %d unstarved, %s" % (
            best or "-", GOOD_S, len(mine), len(s_mine), len(u_mine), "all fast" if mine and all(not is_slow(r) for r in mine) else "some slow or none"))
    return "\n".join(lines) + "\n"


# ---- self test (no game) -----------------------------------------------------------------------------------------------------------------
def selftest(args):
    """The plumbing without the game: the system queries on a stand-in process (a Python that sleeps), the stack scan, a dump, the log parser, the report."""
    child = subprocess.Popen([sys.executable, "-I", "-c", "import time; time.sleep(60)"])
    time.sleep(1.0)
    try:
        proc, threads = system_threads(child.pid)
        assert proc and threads, "no thread info"
        print("process:", proc)
        for t in threads[:2]:
            t2 = {k: v for k, v in t.items() if k not in ("win32_start", "stack_base", "stack_limit")}
            t2.update(thread_extra(t["tid"]))
            print("thread:", t2)
        sysn = syscall_names()
        print("syscall names:", len(sysn), "e.g.", list(sysn.items())[:2])
        print("process flags:", process_flags(child.pid))
        st = stack_scan(child.pid, threads[0])
        print("stack scan:", {k: v for k, v in st.items() if k != "stack_hits"}, "hits", len(st.get("stack_hits") or []))
        for hit in (st.get("stack_hits") or [])[:5]:
            print("  ", hit)
        d = write_dump(child.pid, Path(os.environ.get("TEMP", ".")) / "exit_probe_selftest.dmp")
        print("dump:", d)
        try:
            os.unlink(d["path"])
        except OSError:
            pass
    finally:
        child.kill()
    tmp = Path(os.environ.get("TEMP", ".")) / "exit_probe_selftest.log"
    tmp.write_text("[2026-10-29 11:36:55.964] [info] [core] [t25092] Window closing, shutting down...\n"
                   "[2026-10-29 11:36:55.990] [info] [core] [t25092] Exit: the GPU command processor stopped in 12 ms.\n"
                   "[2026-10-29 11:36:55.992] [info] [core] [t25092] Title terminated; hard-exiting process.\n", encoding="utf-8")
    lt = LogTail(tmp)
    lt.poll()
    assert lt.close_t and lt.line_t and lt.drain == {"ok": True, "ms": 12}, (lt.close_t, lt.line_t, lt.drain)
    assert abs((lt.line_t - lt.close_t) - 0.028) < 1e-3
    fake = [
        {"tag": "t1", "cell": "S", "mode": "0", "quit": "pause", "warm": False, "line_to_exit_s": 0.4},
        {"tag": "t2", "cell": "S", "mode": "T", "quit": "close", "warm": False, "line_to_exit_s": 0.5},
        {"tag": "t3", "cell": "S", "mode": "T", "quit": "pause", "warm": False, "line_to_exit_s": 0.6},
        {"tag": "t4", "cell": "S", "mode": "D", "quit": "pause", "warm": False, "line_to_exit_s": None, "zombie": True},
    ]
    print(report(fake))
    assert choose_best(fake) == "T"
    assert plan_h2(1, fake)[1][1] == "T"
    print("selftest ok")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("batch")
    b.add_argument("--plan", required=True, choices=["h1", "h2"])
    b.add_argument("--out", required=True)
    b.add_argument("--deadline-epoch", type=float)
    r = sub.add_parser("report")
    r.add_argument("--out", required=True)
    sub.add_parser("selftest")
    a = ap.parse_args()
    if a.cmd == "report":
        text = report(read_results(a.out))
        (Path(a.out) / "SUMMARY.txt").write_text(text, encoding="utf-8")
        print(text)
        return 0
    if a.cmd == "selftest":
        return selftest(a)
    return batch(a)


# devrun reads its paths from the environment when it is imported: the tool needs them set before it starts.
for _name in ("MCLA_EXE", "MCLA_DEV_ROOT", "MCLA_GAME_DATA"):
    if not os.environ.get(_name) and len(sys.argv) > 1 and sys.argv[1] == "batch":
        raise SystemExit(f"{_name} is not set (the chain sets MCLA_EXE, MCLA_DEV_ROOT and MCLA_GAME_DATA)")
os.environ.setdefault("MCLA_DEV_ROOT", os.path.join(os.environ.get("TEMP", "."), "exit_probe_dev"))
os.environ.setdefault("MCLA_EXE", os.path.join(os.environ.get("TEMP", "."), "none", "mcla.exe"))
import devrun  # noqa: E402

if __name__ == "__main__":
    sys.exit(main())
