r"""Video memory sampler of agent vram19: Windows' "GPU Process Memory" and "GPU Adapter Memory" performance counters (PDH), read in a thread every 500 ms.

What the counters are (Windows 10/11, the same numbers Task Manager shows): per process and per adapter (instance names pid_<n>_luid_0x<hi>_0x<lo>_phys_<k> and
luid_0x<hi>_0x<lo>_phys_<k>), in bytes: Dedicated Usage (video memory), Shared Usage (system memory the GPU uses), Local Usage and Non Local Usage (the same by DXGI
segment group), Total Committed. They come from the video memory manager, so a Direct3D 12 and a Vulkan process are counted alike, and they are the numbers DXGI's
QueryVideoMemoryInfo gives the process (CurrentUsage) plus the adapter's total.

    s = VramSampler(target_names=("mcla.exe",)); s.start(); ...; s.stop(); s.write_csv(path)

A row: t_ns (time.perf_counter_ns(), the steady clock of the game's frames.csv "t0"), wall (time.time()), kind (proc / adapter), pid (0 for an adapter), luid, then the
counters in MB (float). A process row is kept only for the target processes (by exe name, found with a process snapshot each sample); adapter rows for every adapter.
Nothing here starts a game.
"""
import ctypes
import csv
import threading
import time
from ctypes import wintypes

PDH_MORE_DATA = 0x800007D2
PDH_NO_DATA = 0x800007D5
PDH_CALC_NEGATIVE_DENOMINATOR = 0x800007D6
PDH_FMT_DOUBLE = 0x00000200

PROC_COUNTERS = ("Dedicated Usage", "Shared Usage", "Local Usage", "Non Local Usage", "Total Committed")
ADAPTER_COUNTERS = ("Dedicated Usage", "Shared Usage", "Total Committed")
MB = 1048576.0


class _PdhValue(ctypes.Structure):
    _fields_ = [("CStatus", wintypes.DWORD), ("doubleValue", ctypes.c_double)]


class _PdhItem(ctypes.Structure):
    _fields_ = [("szName", wintypes.LPWSTR), ("FmtValue", _PdhValue)]


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD), ("th32ProcessID", wintypes.DWORD), ("th32DefaultHeapID", ctypes.c_size_t),
                ("th32ModuleID", wintypes.DWORD), ("cntThreads", wintypes.DWORD), ("th32ParentProcessID", wintypes.DWORD), ("pcPriClassBase", ctypes.c_long),
                ("dwFlags", wintypes.DWORD), ("szExeFile", ctypes.c_wchar * 260)]


def _kernel32():
    k = ctypes.WinDLL("kernel32", use_last_error=True)
    k.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
    k.CreateToolhelp32Snapshot.restype = ctypes.c_void_p
    k.Process32FirstW.argtypes = [ctypes.c_void_p, ctypes.POINTER(PROCESSENTRY32W)]
    k.Process32NextW.argtypes = [ctypes.c_void_p, ctypes.POINTER(PROCESSENTRY32W)]
    k.CloseHandle.argtypes = [ctypes.c_void_p]
    return k


def process_pids(names):
    """{pid: exe name} of the running processes whose exe name is in names (lower case compare)."""
    k = _kernel32()
    snap = k.CreateToolhelp32Snapshot(0x2, 0)
    if snap is None or snap == ctypes.c_void_p(-1).value:
        return {}
    found = {}
    try:
        entry = PROCESSENTRY32W()
        entry.dwSize = ctypes.sizeof(entry)
        ok = k.Process32FirstW(snap, ctypes.byref(entry))
        while ok:
            if entry.szExeFile.lower() in names:
                found[entry.th32ProcessID] = entry.szExeFile
            ok = k.Process32NextW(snap, ctypes.byref(entry))
    finally:
        k.CloseHandle(snap)
    return found


class VramSampler:
    def __init__(self, target_names=("mcla.exe",), interval_s=0.5):
        self.names = {n.lower() for n in target_names}
        self.interval = interval_s
        self.rows = []
        self.errors = []
        self._stop = threading.Event()
        self._thread = None
        pdh = self.pdh = ctypes.WinDLL("pdh")
        handle_out = ctypes.POINTER(ctypes.c_void_p)
        for name, argtypes in (("PdhOpenQueryW", [wintypes.LPCWSTR, ctypes.c_size_t, handle_out]),
                               ("PdhAddEnglishCounterW", [ctypes.c_void_p, wintypes.LPCWSTR, ctypes.c_size_t, handle_out]),
                               ("PdhCollectQueryData", [ctypes.c_void_p]),
                               ("PdhGetFormattedCounterArrayW", [ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD),
                                                                 ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]),
                               ("PdhCloseQuery", [ctypes.c_void_p])):
            f = getattr(pdh, name)
            f.argtypes, f.restype = argtypes, wintypes.DWORD
        self.query = ctypes.c_void_p()
        self._check(pdh.PdhOpenQueryW(None, 0, ctypes.byref(self.query)), "PdhOpenQuery")
        self.counters = {}   # (kind, counter name) -> handle
        for kind, group, names in (("proc", "GPU Process Memory", PROC_COUNTERS), ("adapter", "GPU Adapter Memory", ADAPTER_COUNTERS)):
            for name in names:
                handle = ctypes.c_void_p()
                path = f"\\{group}(*)\\{name}"
                status = pdh.PdhAddEnglishCounterW(self.query, path, 0, ctypes.byref(handle))
                if status:
                    self.errors.append(f"{path}: PDH status 0x{status:08x}")
                    continue
                self.counters[(kind, name)] = handle
        if not self.counters:
            raise OSError("no GPU memory counter could be added: " + "; ".join(self.errors))
        self.pdh.PdhCollectQueryData(self.query)   # the first collection starts the query

    @staticmethod
    def _check(status, what):
        if status:
            raise OSError(f"{what}: PDH status 0x{status:08x}")

    def _read_array(self, handle):
        pdh = self.pdh
        size, count, buf = wintypes.DWORD(0), wintypes.DWORD(0), None
        for _ in range(4):
            status = pdh.PdhGetFormattedCounterArrayW(handle, PDH_FMT_DOUBLE, ctypes.byref(size), ctypes.byref(count), buf)
            if status != PDH_MORE_DATA:
                break
            buf = (ctypes.c_byte * size.value)()
        if status or buf is None:
            return {}
        items = ctypes.cast(buf, ctypes.POINTER(_PdhItem))
        out = {}
        for i in range(count.value):
            it = items[i]
            if it.FmtValue.CStatus in (0, 1):
                out[it.szName] = it.FmtValue.doubleValue
        return out

    @staticmethod
    def _parse(instance):
        """(pid or 0, luid text) from an instance name."""
        pid = 0
        rest = instance
        if instance.startswith("pid_"):
            end = instance.index("_", 4)
            pid = int(instance[4:end])
            rest = instance[end + 1:]
        luid = rest.replace("luid_", "").split("_phys")[0]
        return pid, luid

    def sample(self):
        status = self.pdh.PdhCollectQueryData(self.query)
        if status not in (0, PDH_NO_DATA):
            self.errors.append(f"collect: 0x{status:08x}")
            return
        t_ns, wall = time.perf_counter_ns(), time.time()
        targets = process_pids(self.names)
        table = {}   # (kind, pid, luid) -> {counter: MB}
        for (kind, name), handle in self.counters.items():
            for instance, value in self._read_array(handle).items():
                try:
                    pid, luid = self._parse(instance)
                except ValueError:
                    continue
                if kind == "proc" and pid not in targets:
                    continue
                table.setdefault((kind, pid, luid), {})[name] = value / MB
        for (kind, pid, luid), values in table.items():
            self.rows.append((t_ns, wall, kind, pid, luid, values))

    def _run(self):
        next_t = time.perf_counter()
        while not self._stop.is_set():
            try:
                self.sample()
            except Exception as e:   # a failed sample must not end the run
                self.errors.append(repr(e))
            next_t += self.interval
            self._stop.wait(max(0.0, next_t - time.perf_counter()))

    def start(self):
        self._thread = threading.Thread(target=self._run, name="vram-sampler", daemon=True)
        self._thread.start()
        return self

    def stop(self):
        self._stop.set()
        if self._thread:
            self._thread.join(5)
        self.pdh.PdhCloseQuery(self.query)

    def write_csv(self, path):
        names = sorted({n for _, _, _, _, _, v in self.rows for n in v})
        with open(path, "w", encoding="utf-8", newline="") as f:
            w = csv.writer(f)
            w.writerow(["t_ns", "wall", "kind", "pid", "luid"] + names)
            for t_ns, wall, kind, pid, luid, v in self.rows:
                w.writerow([t_ns, f"{wall:.3f}", kind, pid, luid] + [f"{v[n]:.1f}" if n in v else "" for n in names])
        return len(self.rows)


if __name__ == "__main__":   # self test: three seconds, every process with a GPU memory row, the adapters
    import sys
    s = VramSampler(target_names=tuple(sys.argv[1:]) or ("chrome.exe", "dwm.exe", "explorer.exe", "msedge.exe", "claude.exe", "python.exe"))
    s.start()
    time.sleep(3)
    s.stop()
    print(len(s.rows), "rows; errors:", s.errors[:3])
    last = max(r[0] for r in s.rows)
    for t_ns, wall, kind, pid, luid, v in s.rows:
        if t_ns == last:
            print(kind, pid, luid, {k: round(x, 1) for k, x in v.items()})
