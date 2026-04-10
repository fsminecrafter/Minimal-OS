import tkinter as tk
from tkinter import filedialog, messagebox, ttk, simpledialog
import os
import re
from datetime import datetime
from collections import deque
import struct
import subprocess
import sys

BLOCK_SIZE = 4096
MAX_SCAN_BLOCKS = 10000
STREAM_CHUNK = 64 * 1024  # 64KB window

# ==========================================
# THEME SETUP
# ==========================================

def install_theme():
    """Install Sun-Valley theme if needed"""
    try:
        import ttkbootstrap
    except ImportError:
        print("Installing ttkbootstrap (Sun-Valley theme)...")
        subprocess.check_call([sys.executable, "-m", "pip", "install", "ttkbootstrap", "-q"])
        import ttkbootstrap


def setup_theme(root):
    """Setup Sun-Valley dark theme"""
    try:
        from ttkbootstrap import Style
        style = Style(theme="darkly")
        return style
    except:
        # Fallback to basic dark theme
        style = ttk.Style()
        style.theme_use('clam')
        return style


# ==========================================
# DEBUG
# ==========================================

def debug(log_widget, msg):
    log_widget.insert(tk.END, msg + "\n")
    log_widget.see(tk.END)
    print(msg)


# ==========================================
# UNDO/REDO SYSTEM
# ==========================================

class Command:
    """Base class for undo/redo commands"""
    def execute(self):
        pass
    
    def undo(self):
        pass


class CreateFileCommand(Command):
    def __init__(self, app, filename, parent_block, content=""):
        self.app = app
        self.filename = filename
        self.parent_block = parent_block
        self.content = content
        self.created_block = None
    
    def execute(self):
        # Create file in filesystem
        pass
    
    def undo(self):
        # Remove file from filesystem
        pass


class UndoRedoStack:
    def __init__(self, max_size=50):
        self.undo_stack = deque(maxlen=max_size)
        self.redo_stack = deque(maxlen=max_size)
    
    def record(self, command):
        command.execute()
        self.undo_stack.append(command)
        self.redo_stack.clear()
    
    def undo(self):
        if self.undo_stack:
            cmd = self.undo_stack.pop()
            cmd.undo()
            self.redo_stack.append(cmd)
            return True
        return False
    
    def redo(self):
        if self.redo_stack:
            cmd = self.redo_stack.pop()
            cmd.execute()
            self.undo_stack.append(cmd)
            return True
        return False
    
    def can_undo(self):
        return len(self.undo_stack) > 0
    
    def can_redo(self):
        return len(self.redo_stack) > 0


# ==========================================
# DEBUG
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
        self.root.title("MinimaFS Explorer - v4")
        self.root.geometry("1400x900")

        self.file = None
        self.file_path = None
        self.nodes = {}
        self.file_blocks_cache = {}  # Cache for file data

        self.total_blocks = 0
        self.raw_mode = False

        self.current_file = None
        self.current_block = 0
        self.current_offset = 0

        # Edit system
        self.edit_allowed = False
        self.edit_requested_this_session = False
        self.clipboard = None
        self.clipboard_mode = None
        self.modified_files = {}  # Track modifications
        
        # Undo/redo
        self.history = UndoRedoStack()

        # Setup theme
        install_theme()
        self.setup_theme()
        
        self.build_ui()
        self.setup_keybindings()

    def setup_theme(self):
        """Configure modern theme"""
        try:
            from ttkbootstrap import Style
            self.style = Style(theme="darkly")
        except:
            self.style = ttk.Style()
            self.style.theme_use('clam')

    def setup_keybindings(self):
        """Setup keyboard shortcuts"""
        self.root.bind('<Control-z>', lambda e: self.undo_edit())
        self.root.bind('<Control-y>', lambda e: self.redo_edit())
        self.root.bind('<Control-c>', lambda e: self.copy_file())
        self.root.bind('<Control-x>', lambda e: self.cut_file())
        self.root.bind('<Control-v>', lambda e: self.paste_file())
        self.root.bind('<Control-s>', lambda e: self.save_fs())

    # --------------------------------------

    def build_ui(self):
        """Build modern UI layout"""
        # MENU BAR
        menubar = tk.Menu(self.root)
        self.root.config(menu=menubar)

        # File menu
        file_menu = tk.Menu(menubar, tearoff=0)
        menubar.add_cascade(label="File", menu=file_menu)
        file_menu.add_command(label="Open IMG", command=self.open_img)
        file_menu.add_command(label="New File", command=self.new_file_dialog)
        file_menu.add_command(label="New Folder", command=self.new_folder_dialog)
        file_menu.add_separator()
        file_menu.add_command(label="Save (Ctrl+S)", command=self.save_fs)
        file_menu.add_separator()
        file_menu.add_command(label="Exit", command=self.root.quit)

        # Edit menu
        edit_menu = tk.Menu(menubar, tearoff=0)
        menubar.add_cascade(label="Edit", menu=edit_menu)
        edit_menu.add_command(label="Undo (Ctrl+Z)", command=self.undo_edit)
        edit_menu.add_command(label="Redo (Ctrl+Y)", command=self.redo_edit)
        edit_menu.add_separator()
        edit_menu.add_command(label="Copy (Ctrl+C)", command=self.copy_file)
        edit_menu.add_command(label="Cut (Ctrl+X)", command=self.cut_file)
        edit_menu.add_command(label="Paste (Ctrl+V)", command=self.paste_file)
        edit_menu.add_separator()
        edit_menu.add_command(label="Search", command=self.open_search_window)

        # TOP TOOLBAR
        top_frame = ttk.Frame(self.root)
        top_frame.pack(fill="x", padx=5, pady=3)

        ttk.Button(top_frame, text="📂 Open IMG", command=self.open_img).pack(side="left", padx=3)
        ttk.Button(top_frame, text="🔍 Search", command=self.open_search_window).pack(side="left", padx=3)
        ttk.Button(top_frame, text="➕ New File", command=self.new_file_dialog).pack(side="left", padx=3)
        ttk.Button(top_frame, text="📁 New Folder", command=self.new_folder_dialog).pack(side="left", padx=3)
        
        ttk.Separator(top_frame, orient="vertical").pack(side="left", fill="y", padx=10)
        
        ttk.Button(top_frame, text="💾 Save", command=self.save_fs).pack(side="left", padx=3)
        ttk.Button(top_frame, text="↶ Undo", command=self.undo_edit).pack(side="left", padx=3)
        ttk.Button(top_frame, text="↷ Redo", command=self.redo_edit).pack(side="left", padx=3)
        
        ttk.Separator(top_frame, orient="vertical").pack(side="left", fill="y", padx=10)
        
        ttk.Button(top_frame, text="🔄 Toggle RAW", command=self.toggle_raw).pack(side="left", padx=3)

        # STATUS BAR
        status_frame = ttk.Frame(self.root)
        status_frame.pack(fill="x", padx=5, pady=2)
        
        self.status = ttk.Label(status_frame, text="Ready | Mode: Parsed | Edit: OFF")
        self.status.pack(side="left")

        # MAIN CONTENT
        main = ttk.PanedWindow(self.root, orient="horizontal")
        main.pack(fill="both", expand=True, padx=3, pady=2)

        # LEFT PANEL - FILE TREE
        left_frame = ttk.LabelFrame(main, text="Filesystem Tree", padding=5)
        
        self.tree = ttk.Treeview(left_frame, height=30)
        self.tree.bind("<<TreeviewSelect>>", self.on_select)
        self.tree.bind("<Button-3>", self.on_right_click)

        scroll_y = ttk.Scrollbar(left_frame, orient="vertical", command=self.tree.yview)
        self.tree.configure(yscrollcommand=scroll_y.set)

        self.tree.pack(side="left", fill="both", expand=True)
        scroll_y.pack(side="right", fill="y")

        main.add(left_frame, weight=1)

        # RIGHT PANEL - FILE VIEW
        right_frame = ttk.LabelFrame(main, text="File Content", padding=5)

        # Info bar
        info_frame = ttk.Frame(right_frame)
        info_frame.pack(fill="x", padx=5, pady=5)

        ttk.Label(info_frame, text="Block:").pack(side="left", padx=5)
        self.block_label = ttk.Label(info_frame, text="N/A")
        self.block_label.pack(side="left", padx=5)

        ttk.Separator(info_frame, orient="vertical").pack(side="left", fill="y", padx=10)

        ttk.Label(info_frame, text="Part:").pack(side="left", padx=5)
        self.part_var = tk.IntVar(value=0)
        self.part_spin = ttk.Spinbox(info_frame, from_=0, to=0, textvariable=self.part_var, width=8)
        self.part_spin.pack(side="left", padx=5)
        ttk.Button(info_frame, text="View", command=self.view_part, width=8).pack(side="left", padx=5)

        # Text editor
        text_frame = ttk.Frame(right_frame)
        text_frame.pack(fill="both", expand=True, padx=5, pady=5)

        self.text = tk.Text(text_frame, wrap="word", bg="#2d2d2d", fg="#e0e0e0", font=("Courier", 10))
        
        yscroll = ttk.Scrollbar(text_frame, orient="vertical", command=self.text.yview)
        xscroll = ttk.Scrollbar(text_frame, orient="horizontal", command=self.text.xview)

        self.text.configure(yscrollcommand=yscroll.set, xscrollcommand=xscroll.set)

        self.text.grid(row=0, column=0, sticky="nsew")
        yscroll.grid(row=0, column=1, sticky="ns")
        xscroll.grid(row=1, column=0, sticky="ew")

        text_frame.grid_rowconfigure(0, weight=1)
        text_frame.grid_columnconfigure(0, weight=1)

        main.add(right_frame, weight=2)

        # BOTTOM LOG PANEL
        log_frame = ttk.LabelFrame(self.root, text="Debug Log", padding=3)
        log_frame.pack(fill="x", padx=3, pady=2, ipady=3)

        self.log = tk.Text(log_frame, height=6, bg="#0a0a0a", fg="#00ff00", font=("Courier", 9))
        log_scroll = ttk.Scrollbar(log_frame, orient="vertical", command=self.log.yview)
        self.log.configure(yscrollcommand=log_scroll.set)
        
        self.log.pack(side="left", fill="both", expand=True)
        log_scroll.pack(side="right", fill="y")

    # --------------------------------------

    def get_chunk_size(self):
        """Get chunk size based on current file size and raw mode"""
        if not self.current_file:
            return STREAM_CHUNK

        filelen = safe_int(self.current_file["entry"].get("FILELEN", "0"))

        # For large files in raw mode, use block-sized chunks
        if self.raw_mode and filelen > 10 * 1024:  # > 10KB
            return BLOCK_SIZE

        return STREAM_CHUNK

    # --------------------------------------

    def update_part_spin(self):
        """Update the part spinbox range based on current file"""
        if not self.current_file:
            self.part_spin.config(to=0)
            return

        filelen = safe_int(self.current_file["entry"].get("FILELEN", "0"))
        chunk_size = self.get_chunk_size()

        if filelen <= 0:
            max_parts = 0
        else:
            max_parts = max(0, (filelen - 1) // chunk_size)

        self.part_spin.config(to=max_parts)

    # --------------------------------------

    def open_img(self):
        path = filedialog.askopenfilename(filetypes=[("IMG", "*.img"), ("All Files", "*")])
        if not path:
            return

        # Open in read-write mode
        self.file = open(path, "r+b")
        self.file_path = path

        size = os.path.getsize(path)
        self.total_blocks = size // BLOCK_SIZE

        self.part_spin.config(to=max(0, self.total_blocks - 1))

        debug(self.log, f"✓ Opened: {path}")
        debug(self.log, f"✓ Blocks: {self.total_blocks} ({size/(1024*1024):.2f} MB)")

        b0 = read_block(self.file, 0)

        if detect_minimafs(b0):
            debug(self.log, "✓ MinimaFS filesystem detected")
        else:
            debug(self.log, "⚠ WARNING: Not recognized as MinimaFS")

        self.load_fs()
        self.update_status()

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

        self.part_var.set(0)
        self.update_part_spin()
        self.show_file()

    # --------------------------------------

    def show_file(self):
        if not self.current_file:
            return

        entry = self.current_file["entry"]
        block = self.current_block

        filelen = safe_int(entry.get("FILELEN", "0"))

        # CLEAR TEXT AREA FIRST
        self.text.config(state="normal")
        self.text.delete("1.0", tk.END)

        debug(self.log, f"Viewing: {entry.get('FILENAME', 'unknown')} | Block {block} | Size {filelen}")
        self.block_label.config(text=str(block))

        data = read_file_stream(self.file, block, filelen, self.current_offset)

        # Display header
        self.text.insert(tk.END, f"{'='*60}\n")
        self.text.insert(tk.END, f"File: {entry.get('FILENAME', 'unknown')}\n")
        self.text.insert(tk.END, f"Block: {block} | Size: {filelen} bytes | Type: {entry.get('FILETYPE', 'unknown')}\n")
        self.text.insert(tk.END, f"Created: {entry.get('CREATEDDATE', 'unknown')} | Modified: {entry.get('LASTCHANGED', 'unknown')}\n")
        self.text.insert(tk.END, f"{'='*60}\n\n")

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
            # PARSED VIEW
            self.text.insert(tk.END, data.decode(errors="ignore"))

        # Set text widget state based on edit mode
        if self.edit_allowed:
            self.text.config(state="normal")
        else:
            self.text.config(state="disabled")

    # --------------------------------------

    def view_part(self):
        if not self.current_file:
            return

        chunk_size = self.get_chunk_size()
        self.current_offset = self.part_var.get() * chunk_size
        self.show_file()

    # --------------------------------------

    def toggle_raw(self):
        self.raw_mode = not self.raw_mode
        debug(self.log, f"RAW mode = {self.raw_mode}")

        self.update_part_spin()
        self.update_status()
        self.show_file()

    # ==========================================
    # FILESYSTEM OPERATIONS
    # ==========================================

    def create_file_entry(self, filename, file_type, file_format, content, parent_folder="", runnable=False):
        """Create a MinimaFS file entry"""
        filelen = len(content) + 256  # Estimate with header
        created_date = datetime.now().strftime("%d%b%Y").lower()
        
        entry = f"""@HEADER@
@FILETYPE:{file_type}@
@FILEFORMAT:{file_format}@
@FILELEN:{filelen}@
@FILENAME:'{filename.ljust(32)}'@
@CREATEDDATE:'{created_date}'@
@LASTCHANGED:'{created_date}'@
@PARENTFOLDER:'{parent_folder}'@
@RUNNABLE:{str(runnable)}@
@HIDDEN:False@
@DATA@
{content}
@END
"""
        return entry

    def find_free_block(self):
        """Find first available free block"""
        for i in range(self.total_blocks):
            block_data = read_block(self.file, i)
            if is_zero_block(block_data):
                return i
        return None

    def write_block(self, block_num, data):
        """Write data to a specific block"""
        if len(data) > BLOCK_SIZE:
            data = data[:BLOCK_SIZE]
        elif len(data) < BLOCK_SIZE:
            data = data + b'\x00' * (BLOCK_SIZE - len(data))
        
        self.file.seek(block_num * BLOCK_SIZE)
        self.file.write(data)
        self.modified_files[block_num] = True

    def update_folder_desc(self, parent_block, new_entry_block):
        """Update folder.desc file with new entry"""
        folder_desc_data = read_block(self.file, parent_block)
        folder_desc_text = folder_desc_data.decode(errors="ignore")
        
        # Add new child block reference
        if "@CHILDREN@" not in folder_desc_text:
            folder_desc_text += "\n@CHILDREN@\n"
        
        folder_desc_text += f"block:{new_entry_block}\n"
        
        self.write_block(parent_block, folder_desc_text.encode())

    def update_disk_desc(self):
        """Update disk.desc with filesystem changes"""
        # This would typically be stored at block 0 or a fixed location
        debug(self.log, "Updated disk.desc metadata")

    def write_file_to_fs(self, filename, file_type, file_format, content):
        """Write a new file to the filesystem"""
        # Find free block
        free_block = self.find_free_block()
        if free_block is None:
            messagebox.showerror("Error", "No free blocks available")
            return False
        
        # Create entry
        entry = self.create_file_entry(filename, file_type, file_format, content)
        
        # Write to block
        self.write_block(free_block, entry.encode())
        
        # Update folder.desc (assuming root is at block 1)
        self.update_folder_desc(1, free_block)
        
        # Update disk.desc
        self.update_disk_desc()
        
        debug(self.log, f"Written file '{filename}' to block {free_block}")
        return True

    # ==========================================
    # END FILESYSTEM OPERATIONS
    # ==========================================

    # --------------------------------------

    def search_fs(self):
        q = self.search_entry.get()
        if not q:
            return

        whole_word = self.whole_word_var.get()
        match_case = self.match_case_var.get()

        if not match_case:
            q_search = q.lower()
        else:
            q_search = q

        results = []

        for node, info in self.nodes.items():
            name = info["entry"].get("FILENAME", "")

            # Search in filename
            if not match_case:
                search_name = name.lower()
            else:
                search_name = name

            found_in_name = False
            if whole_word:
                if re.search(r'\b' + re.escape(q_search) + r'\b', search_name):
                    found_in_name = True
            else:
                if q_search in search_name:
                    found_in_name = True

            if found_in_name:
                results.append((name, "filename"))
                continue

            # Search in file content
            filelen = safe_int(info["entry"].get("FILELEN", "0"))
            if filelen > 0:
                block = info["block"]
                data = read_file_stream(self.file, block, filelen, 0)

                if not match_case:
                    search_data = data.decode(errors="ignore").lower()
                else:
                    search_data = data.decode(errors="ignore")

                found_in_content = False
                if whole_word:
                    if re.search(r'\b' + re.escape(q_search) + r'\b', search_data):
                        found_in_content = True
                else:
                    if q_search in search_data:
                        found_in_content = True

                if found_in_content:
                    results.append((name, "content"))

        # CLEAR TEXT AREA BEFORE SHOWING RESULTS
        self.text.config(state="normal")
        self.text.delete("1.0", tk.END)

        self.text.insert(tk.END, f"{'='*60}\n")
        self.text.insert(tk.END, f"SEARCH RESULTS: '{q}'\n")
        self.text.insert(tk.END, f"Found {len(results)} result(s)\n")
        self.text.insert(tk.END, f"{'='*60}\n\n")

        for r in results:
            name, location = r
            self.text.insert(tk.END, f"📄 {name} ({location})\n")

        self.text.config(state="disabled")
        debug(self.log, f"Search found {len(results)} results")

    # NEW SEARCH WINDOW
    def open_search_window(self):
        """Open search in a separate window"""
        search_win = tk.Toplevel(self.root)
        search_win.title("Search Files")
        search_win.geometry("450x250")
        search_win.transient(self.root)
        search_win.grab_set()

        ttk.Label(search_win, text="Search Query:").pack(padx=15, pady=10)
        search_entry = ttk.Entry(search_win, width=50)
        search_entry.pack(padx=15, pady=5)
        search_entry.focus()

        self.whole_word_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(search_win, text="Whole Word Match", variable=self.whole_word_var).pack(anchor="w", padx=20, pady=5)

        self.match_case_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(search_win, text="Match Case (Sensitive)", variable=self.match_case_var).pack(anchor="w", padx=20, pady=5)

        def do_search():
            self.search_entry = search_entry
            self.search_fs()
            search_win.destroy()

        button_frame = ttk.Frame(search_win)
        button_frame.pack(pady=20)
        
        ttk.Button(button_frame, text="Search", command=do_search).pack(side="left", padx=5)
        ttk.Button(button_frame, text="Cancel", command=search_win.destroy).pack(side="left", padx=5)

    # EDIT PERMISSION
    def check_edit_allowed(self):
        """Check if editing is allowed, prompt if first time in session"""
        if self.edit_allowed:
            return True

        if not self.edit_requested_this_session:
            self.edit_requested_this_session = True
            result = messagebox.askyesno("Allow Editing?", 
                "This filesystem is read-only by default.\n\nAllow editing for this session?")
            if result:
                self.edit_allowed = True
                self.update_status()
                debug(self.log, "Editing enabled for this session")
                return True
            else:
                debug(self.log, "Editing denied")
                return False
        else:
            messagebox.showwarning("Editing Disabled", "Editing is disabled for this session.\nRestart to enable again.")
            return False

    def update_status(self):
        """Update status bar"""
        mode = "RAW" if self.raw_mode else "Parsed"
        edit = "ON" if self.edit_allowed else "OFF"
        modified = f" | {len(self.modified_files)} unsaved" if self.modified_files else ""
        self.status.config(text=f"Ready  |  Mode: {mode}  |  Edit: {edit}{modified}")

    # COPY/CUT/PASTE
    def on_right_click(self, event):
        """Handle right-click context menu"""
        item = self.tree.identify("item", event.x, event.y)
        if item:
            self.tree.selection_set(item)
            self.on_select(None)

            menu = tk.Menu(self.root, tearoff=0)
            menu.add_command(label="Copy", command=self.copy_file)
            menu.add_command(label="Cut", command=self.cut_file)
            menu.add_command(label="Paste", command=self.paste_file)
            menu.add_separator()
            menu.add_command(label="Delete (Zero Block)", command=self.delete_file)
            menu.post(event.x_root, event.y_root)

    def copy_file(self):
        """Copy selected file to clipboard"""
        if self.current_file:
            self.clipboard = self.current_file
            self.clipboard_mode = "copy"
            debug(self.log, f"Copied: {self.current_file['entry'].get('FILENAME', 'unknown')}")

    def cut_file(self):
        """Cut selected file to clipboard"""
        if not self.check_edit_allowed():
            return

        if self.current_file:
            self.clipboard = self.current_file
            self.clipboard_mode = "cut"
            debug(self.log, f"Cut: {self.current_file['entry'].get('FILENAME', 'unknown')}")

    def paste_file(self):
        """Paste file from clipboard"""
        if not self.check_edit_allowed():
            return

        if not self.clipboard:
            messagebox.showinfo("Paste", "Nothing to paste")
            return

        debug(self.log, f"Pasted: {self.clipboard['entry'].get('FILENAME', 'unknown')}")
        # TODO: Implement actual paste logic

    def delete_file(self):
        """Delete selected file"""
        if not self.check_edit_allowed():
            return

        if self.current_file:
            name = self.current_file["entry"].get("FILENAME", "unknown")
            result = messagebox.askyesno("Delete", f"Permanently delete '{name}'? This will zero the block.")
            if result:
                block = self.current_block
                # Zero out the block
                self.write_block(block, b'\x00' * BLOCK_SIZE)
                debug(self.log, f"✓ Deleted and zeroed block {block}: {name}")
                messagebox.showinfo("Deleted", f"File '{name}' has been deleted and block {block} zeroed.")
                self.load_fs()  # Reload filesystem
                self.text.config(state="normal")
                self.text.delete("1.0", tk.END)
                self.text.config(state="disabled")

    # CREATE NEW FILE/FOLDER
    def new_file_dialog(self):
        """Create new file dialog"""
        if not self.check_edit_allowed():
            return

        dialog = tk.Toplevel(self.root)
        dialog.title("Create New File")
        dialog.geometry("500x350")
        dialog.transient(self.root)
        dialog.grab_set()

        ttk.Label(dialog, text="Filename:").pack(pady=5, padx=20, anchor="w")
        filename_entry = ttk.Entry(dialog, width=50)
        filename_entry.pack(padx=20, pady=5)

        ttk.Label(dialog, text="File Type:").pack(pady=5, padx=20, anchor="w")
        filetype_var = tk.StringVar(value="text")
        ttk.Combobox(dialog, textvariable=filetype_var, values=["text", "binary", "executable"], state="readonly", width=47).pack(padx=20)

        ttk.Label(dialog, text="File Format:").pack(pady=5, padx=20, anchor="w")
        fileformat_var = tk.StringVar(value="txt")
        ttk.Entry(dialog, textvariable=fileformat_var, width=50).pack(padx=20, pady=5)

        ttk.Label(dialog, text="Content:").pack(pady=5, padx=20, anchor="w")
        content_text = tk.Text(dialog, height=8, width=60, bg="#2d2d2d", fg="#e0e0e0")
        content_text.pack(padx=20, pady=5)

        def create():
            filename = filename_entry.get().strip()
            if not filename:
                messagebox.showwarning("Error", "Filename cannot be empty")
                return
            
            file_type = filetype_var.get()
            file_format = fileformat_var.get()
            content = content_text.get("1.0", tk.END).strip()
            
            if self.write_file_to_fs(filename, file_type, file_format, content):
                debug(self.log, f"✓ Created file: {filename}")
                messagebox.showinfo("Success", f"File '{filename}' created successfully!")
                self.load_fs()  # Reload filesystem
                dialog.destroy()
            else:
                messagebox.showerror("Error", "Failed to create file")

        button_frame = ttk.Frame(dialog)
        button_frame.pack(pady=15)
        
        ttk.Button(button_frame, text="Create", command=create).pack(side="left", padx=5)
        ttk.Button(button_frame, text="Cancel", command=dialog.destroy).pack(side="left", padx=5)

    def new_folder_dialog(self):
        """Create new folder dialog"""
        if not self.check_edit_allowed():
            return

        folder_name = simpledialog.askstring("Create Folder", "Folder name:", parent=self.root)
        if folder_name:
            # Create folder entry (just an empty descriptor file)
            self.write_file_to_fs(folder_name, "folder", "desc", "")
            debug(self.log, f"✓ Created folder: {folder_name}")
            self.load_fs()

    # SAVE FILESYSTEM
    def save_fs(self):
        """Save changes back to .img file"""
        if not self.modified_files:
            messagebox.showinfo("No Changes", "No changes to save")
            return

        if not self.file_path:
            messagebox.showwarning("Error", "No file open")
            return

        result = messagebox.askyesno("Save", f"Save {len(self.modified_files)} modified block(s) to image?")
        if result:
            try:
                self.file.flush()
                debug(self.log, f"✓ Saved {len(self.modified_files)} block(s) to {self.file_path}")
                messagebox.showinfo("Success", f"Changes saved! ({len(self.modified_files)} blocks modified)")
                self.modified_files.clear()
                self.update_status()
            except Exception as e:
                messagebox.showerror("Error", f"Failed to save: {str(e)}")
                debug(self.log, f"✗ Save error: {str(e)}")

    # UNDO/REDO
    def undo_edit(self):
        """Undo last change"""
        if self.history.undo():
            debug(self.log, "Undo executed")
        else:
            messagebox.showinfo("Undo", "Nothing to undo")

    def redo_edit(self):
        """Redo last change"""
        if self.history.redo():
            debug(self.log, "Redo executed")
        else:
            messagebox.showinfo("Redo", "Nothing to redo")


# ==========================================
# RUN
# ==========================================

if __name__ == "__main__":
    root = tk.Tk()
    root.geometry("1300x800")
    app = App(root)
    root.mainloop()