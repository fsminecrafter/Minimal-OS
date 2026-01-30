# trace_fast.py
import gdb
from datetime import datetime

class DeferredTraceWithLog(gdb.Command):
    def __init__(self):
        super().__init__("tracefast", gdb.COMMAND_USER)
        self.tracing = False
        self.bp = None
        self.prev_regs = {}
        self.log_file = open("tracefast.log", "w")

        self.regs = [
            "rip", "rax", "rbx", "rcx", "rdx", "rsi", "rdi",
            "rsp", "rbp", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
            "cs", "ss", "ds", "es", "fs", "gs",
            "cr0", "cr2", "cr3", "cr4", "eflags"
        ]

        gdb.events.stop.connect(self.on_stop)

    def get_reg(self, reg):
        try:
            val = int(gdb.parse_and_eval(f"${reg}")) & 0xFFFFFFFFFFFFFFFF
            return val
        except:
            return None

    def log(self, line):
        print(line)
        self.log_file.write(line + "\n")
        self.log_file.flush()

    def start_trace_loop(self):
        self.log("▶️  Starting fast trace. Ctrl+C to stop.\n")
        count = 0
        try:
            while True:
                rip = self.get_reg("rip")
                if rip is None:
                    self.log(f"[#{count}] RIP unavailable")
                else:
                    instr = gdb.execute(f"x/i {rip}", to_string=True).strip()
                    self.log(f"[#{count}] {instr}")

                for reg in self.regs:
                    val = self.get_reg(reg)
                    if val is None:
                        self.log(f"  {reg.upper():<5} = <unavailable>")
                    elif reg == "rsi" and val == 0:
                        self.log(f"  RSI Zero")
                    else:
                        self.log(f"  {reg.upper():<5} = 0x{val:016x}")

                count += 1
                gdb.execute("si", to_string=True)

        except KeyboardInterrupt:
            self.log("\n⏹️  Stopped by user.")
        except Exception as e:
            self.log(f"🛑 Exception: {e}")
        finally:
            self.log_file.close()

    def on_stop(self, event):
        if not self.tracing and isinstance(event, gdb.BreakpointEvent):
            for bp in event.breakpoints:
                if bp == self.bp:
                    now = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
                    self.log(f"\n[🛑 Breakpoint hit at {bp.location} @ {now} — Starting trace]")
                    self.tracing = True
                    self.start_trace_loop()

    def invoke(self, arg, from_tty):
        label = arg.strip() or "long_mode_start"
        if self.bp is None:
            self.log(f"[⏳] Setting breakpoint at symbol: {label}")
            self.bp = gdb.Breakpoint(label, internal=False)
            self.log("[ℹ️ ] Waiting for the OS to reach that point...")
        else:
            self.log("[⚠️ ] Breakpoint already set.")

DeferredTraceWithLog()
