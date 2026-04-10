import tkinter as tk
from tkinter import filedialog, messagebox, ttk
import os

BLOCK_SIZE = 4096
MAX_SCAN_BLOCKS = 10000
STREAM_CHUNK = 64 * 1024  # 64KB window


# ==========================================
# DEBUG
# ==========================================

def debug(log_widget, msg):
    log_widget.insert(tk.END, msg + "\n")
    log_widget.see(tk.END)
    print(msg)


# ==========================================
# FS HELPERS
# ==========================================

def detect_minimafs(data: bytes) -> bool:
    return b"@MAGIC:" in data


def read_block(f, block):
    f.seek(block * BLOCK_SIZE)
    return f.read(BLOCK_SIZE)


def is_zero_block(data: bytes):
    return all(b == 0 for b in data)


def safe_int(v, default=0):
    try:
        return int(v)
    except:
        return default


def parse_entries(block_text):
    entries = []
    parts = block_text.split("@HEADER@")

    for part in parts:
        if "@FILENAME:" in part:
            entry = {}
            segments = part.split("@")

            for seg in segments:
                if ":" in seg:
                    k, v = seg.split(":", 1)
                    entry[k.strip()] = v.strip().strip("'")

            entries.append(entry)

    return entries


def extract_data(block_text):
    if "@DATA@" not in block_text:
        return ""

    data = block_text.split("@DATA@", 1)[1]

    if "@END" in data:
        data = data.split("@END", 1)[0]

    return data


# ==========================================
# STREAM READER (FIXED SAFETY)
# ==========================================

def read_file_stream(f, start_block, filelen, offset=0):
    if filelen <= 0:
        return b""

    start = start_block * BLOCK_SIZE + offset

    remaining = filelen - offset
    if remaining <= 0:
        return b""

    to_read = min(STREAM_CHUNK, remaining)

    f.seek(start)
    return f.read(to_read)


# ==========================================
# APP
# ==========================================

class App:

    def __init__(self, root):
        self.root = root
        self.root.title("MinimaFS Explorer v3 (FIXED)")

        self.file = None
        self.nodes = {}

        self.total_blocks = 0

        self.raw_mode = False

        self.current_file = None
        self.current_block = 0
        self.current_offset = 0

        self.build_ui()

    # --------------------------------------

    def build_ui(self):
        top = tk.Frame(self.root)
        top.pack(fill="x")

        tk.Button(top, text="Open IMG", command=self.open_img).pack(side="left")
        tk.Button(top, text="Toggle RAW", command=self.toggle_raw).pack(side="left")

        tk.Label(top, text="Part:").pack(side="left", padx=5)

        self.part_var = tk.IntVar(value=0)
        self.part_spin = tk.Spinbox(top, from_=0, to=0, textvariable=self.part_var, width=6)
        self.part_spin.pack(side="left")

        tk.Button(top, text="View", command=self.view_part).pack(side="left")

        tk.Label(top, text="Search:").pack(side="left", padx=5)
        self.search_entry = tk.Entry(top, width=20)
        self.search_entry.pack(side="left")

        tk.Button(top, text="Go", command=self.search_fs).pack(side="left")

        self.status = tk.Label(top, text="Mode: Parsed")
        self.status.pack(side="left", padx=10)

        # MAIN SPLIT
        main = tk.PanedWindow(self.root, orient=tk.HORIZONTAL)
        main.pack(fill="both", expand=True)

        # LEFT TREE
        tree_frame = tk.Frame(main, width=380)

        self.tree = ttk.Treeview(tree_frame)
        self.tree.bind("<<TreeviewSelect>>", self.on_select)

        scroll = tk.Scrollbar(tree_frame, command=self.tree.yview)
        self.tree.configure(yscrollcommand=scroll.set)

        self.tree.pack(side="left", fill="both", expand=True)
        scroll.pack(side="right", fill="y")

        main.add(tree_frame, minsize=300)

        # RIGHT PANEL
        right = tk.Frame(main)

        text_frame = tk.Frame(right)

        self.text = tk.Text(text_frame, wrap="none")

        yscroll = tk.Scrollbar(text_frame, orient="vertical", command=self.text.yview)
        xscroll = tk.Scrollbar(text_frame, orient="horizontal", command=self.text.xview)

        self.text.configure(yscrollcommand=yscroll.set, xscrollcommand=xscroll.set)

        self.text.pack(side="left", fill="both", expand=True)
        yscroll.pack(side="right", fill="y")
        xscroll.pack(side="bottom", fill="x")

        text_frame.pack(fill="both", expand=True)

        self.log = tk.Text(right, height=10, bg="#111", fg="#0f0")
        self.log.pack(fill="x")

        main.add(right)

        self.root.update()
        main.sash_place(0, 380, 0)

    # --------------------------------------

    def open_img(self):
        path = filedialog.askopenfilename(filetypes=[("IMG", "*.img")])
        if not path:
            return

        self.file = open(path, "rb")

        size = os.path.getsize(path)
        self.total_blocks = size // BLOCK_SIZE

        self.part_spin.config(to=max(0, self.total_blocks - 1))

        debug(self.log, f"Opened {path}")
        debug(self.log, f"Blocks {self.total_blocks}")

        b0 = read_block(self.file, 0)

        if detect_minimafs(b0):
            debug(self.log, "MinimaFS detected")
        else:
            debug(self.log, "WARNING: Not MinimaFS")

        self.load_fs()

    # --------------------------------------

    def load_fs(self):
        self.tree.delete(*self.tree.get_children())
        self.nodes.clear()

        root = self.tree.insert("", "end", text="Drive")

        for i in range(min(self.total_blocks, MAX_SCAN_BLOCKS)):
            b = read_block(self.file, i)

            if is_zero_block(b):
                continue

            t = b.decode(errors="ignore")

            if "@HEADER@" not in t:
                continue

            entries = parse_entries(t)

            for e in entries:
                name = e.get("FILENAME", f"blk_{i}")

                node = self.tree.insert(root, "end", text=f"{name} (blk {i})")

                self.nodes[node] = {
                    "block": i,
                    "entry": e
                }

        debug(self.log, "Filesystem loaded")

    # --------------------------------------

    def on_select(self, event):
        if self.raw_mode:
            return

        sel = self.tree.selection()
        if not sel:
            return

        node = sel[0]
        info = self.nodes.get(node)
        if not info:
            return

        self.current_block = info["block"]
        self.current_offset = 0
        self.current_file = info

        self.show_file()

    # --------------------------------------

    def show_file(self):
        if not self.current_file:
            return

        entry = self.current_file["entry"]
        block = self.current_block

        filelen = safe_int(entry.get("FILELEN", "0"))

        debug(self.log, f"Viewing block {block} size {filelen}")

        data = read_file_stream(self.file, block, filelen, self.current_offset)

        self.text.delete("1.0", tk.END)

        self.text.insert(tk.END, f"=== FILE ({filelen} bytes) ===\n\n")

        if self.raw_mode:
            # HEX VIEW
            out = ""
            for i in range(0, len(data), 16):
                chunk = data[i:i+16]
                hexp = " ".join(f"{b:02X}" for b in chunk)
                asc = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
                out += f"{i:08X}  {hexp:<48}  {asc}\n"
            self.text.insert(tk.END, out)
        else:
            self.text.insert(tk.END, data.decode(errors="ignore"))

    # --------------------------------------

    def view_part(self):
        if not self.current_file:
            return

        self.current_offset = self.part_var.get() * STREAM_CHUNK
        self.show_file()

    # --------------------------------------

    def toggle_raw(self):
        self.raw_mode = not self.raw_mode
        self.status.config(text=f"Mode: {'RAW' if self.raw_mode else 'Parsed'}")
        debug(self.log, f"RAW mode = {self.raw_mode}")

        self.show_file()

    # --------------------------------------

    def search_fs(self):
        q = self.search_entry.get().lower()
        if not q:
            return

        results = []

        for node, info in self.nodes.items():
            name = info["entry"].get("FILENAME", "").lower()
            if q in name:
                results.append(name)

        self.text.delete("1.0", tk.END)
        self.text.insert(tk.END, "SEARCH RESULTS:\n\n")

        for r in results:
            self.text.insert(tk.END, r + "\n")

        debug(self.log, f"Search found {len(results)} results")


# ==========================================
# RUN
# ==========================================

if __name__ == "__main__":
    root = tk.Tk()
    root.geometry("1300x800")
    app = App(root)
    root.mainloop()