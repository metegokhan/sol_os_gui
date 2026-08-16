#!/usr/bin/env python3
"""
SolarOS Video & GIF Converter GUI
Author: Antigravity / SolarOS Team
Target Device: Waveshare ESP32-S3 4.2" RLCD (400x300 ST7305, ES8311 Audio DAC)

Features:
- Convert MP4/MKV/AVI/MOV/WEBM to optimized 400x300 Animated GIF (with dithering & RAM safety limits).
- Convert Video with Audio to SolarOS MJPEG + WAV synchronized pair for SD card streaming.
- Convert Video to Silent SolarOS MJPEG.
- Convert Video to Native SolarOS 2-bit Raw Stream (.slv) for 30-50 FPS playback.
- Auto-detects ffmpeg or helps download/locate it.
"""

import os
import sys
import shutil
import subprocess
import threading
import urllib.request
import zipfile
import re
import tkinter as tk
from tkinter import ttk, filedialog, messagebox, scrolledtext

class SolarVideoConverterApp:
    def __init__(self, root):
        self.root = root
        self.root.title("SolarOS RLCD Video & GIF Converter")
        self.root.geometry("760x680")
        self.root.minsize(700, 620)

        # Style configuration
        self.style = ttk.Style()
        try:
            self.style.theme_use("clam")
        except Exception:
            pass

        self.input_file = tk.StringVar()
        self.input_files = []
        self.input_summary = tk.StringVar()
        self.output_dir = tk.StringVar()
        self.output_mjpeg = tk.BooleanVar(value=True)
        self.output_mjpeg_audio = tk.BooleanVar(value=True)
        self.output_slv = tk.BooleanVar(value=False)
        self.output_gif = tk.BooleanVar(value=False)
        self.resolution = tk.StringVar(value="400x300 (Full RLCD)")
        self.aspect_mode = tk.StringVar(value="Fit (Letterbox)")
        self.fps = tk.StringVar(value="20")
        self.dither = tk.StringVar(value="bayer (Crisp Pattern)")
        self.trim_start = tk.StringVar(value="0")
        self.trim_duration = tk.StringVar(value="5")
        self.enable_trim = tk.BooleanVar(value=False)
        self.ffmpeg_path = self.find_ffmpeg()

        self.create_widgets()
        self.on_output_changed()

    def find_ffmpeg(self):
        found = shutil.which("ffmpeg")
        if found:
            return found
        local_ffmpeg = os.path.join(os.path.dirname(__file__), "ffmpeg.exe")
        if os.path.exists(local_ffmpeg):
            return local_ffmpeg
        if os.path.exists("ffmpeg.exe"):
            return os.path.abspath("ffmpeg.exe")
        return ""

    def create_widgets(self):
        # Header Frame
        header = tk.Frame(self.root, bg="#1a1a24", height=60)
        header.pack(fill=tk.X)
        
        lbl_title = tk.Label(header, text="☀️ SolarOS Video & GIF Converter", font=("Segoe UI", 15, "bold"), fg="#ffffff", bg="#1a1a24")
        lbl_title.pack(anchor=tk.W, padx=16, pady=(10, 2))
        lbl_sub = tk.Label(header, text="Optimized 400x300 RLCD (ST7305) & ES8311 Audio Media Processor", font=("Segoe UI", 9), fg="#a0a0b8", bg="#1a1a24")
        lbl_sub.pack(anchor=tk.W, padx=16, pady=(0, 10))

        # Main Frame
        main_frame = ttk.Frame(self.root, padding="14")
        main_frame.pack(fill=tk.BOTH, expand=True)

        # 1. File Selection Card
        file_group = ttk.LabelFrame(main_frame, text=" 1. Input & Output Files ", padding="10")
        file_group.pack(fill=tk.X, pady=(0, 10))

        # Input row
        ttk.Label(file_group, text="Input Video/Media:").grid(row=0, column=0, sticky=tk.W, pady=4)
        ttk.Entry(file_group, textvariable=self.input_summary, width=54, state="readonly").grid(row=0, column=1, sticky=tk.EW, padx=6, pady=4)
        input_buttons = ttk.Frame(file_group)
        input_buttons.grid(row=0, column=2, padx=2, pady=4)
        ttk.Button(input_buttons, text="Files...", command=self.browse_input_files).pack(side=tk.LEFT)
        ttk.Button(input_buttons, text="Folder...", command=self.browse_input_folder).pack(side=tk.LEFT, padx=(3, 0))

        # Output dir row
        ttk.Label(file_group, text="Output Directory:").grid(row=1, column=0, sticky=tk.W, pady=4)
        ttk.Entry(file_group, textvariable=self.output_dir, width=54).grid(row=1, column=1, sticky=tk.EW, padx=6, pady=4)
        ttk.Button(file_group, text="Browse...", command=self.browse_output_dir).grid(row=1, column=2, padx=2, pady=4)

        ttk.Label(file_group, text="Output names:").grid(row=2, column=0, sticky=tk.W, pady=4)
        ttk.Label(file_group, text="Source name; non-English letters/symbols are removed.", foreground="#666").grid(row=2, column=1, sticky=tk.W, padx=6, pady=4)

        file_group.columnconfigure(1, weight=1)

        # 2. Conversion Mode Card
        mode_group = ttk.LabelFrame(main_frame, text=" 2. Target Output Formats (select one or more) ", padding="10")
        mode_group.pack(fill=tk.X, pady=(0, 10))

        ttk.Checkbutton(mode_group, text="Cinema MJPEG (.mjpeg)", variable=self.output_mjpeg, command=self.on_output_changed).pack(anchor=tk.W, pady=2)
        ttk.Checkbutton(mode_group, text="Include WAV audio with MJPEG (.wav)", variable=self.output_mjpeg_audio).pack(anchor=tk.W, pady=2)
        ttk.Checkbutton(mode_group, text="⚡ SolarOS Ultra-Fast 2-bit Raw Stream (.slv)", variable=self.output_slv, command=self.on_output_changed).pack(anchor=tk.W, pady=2)
        ttk.Checkbutton(mode_group, text="🎞️ Animated GIF (.gif)", variable=self.output_gif, command=self.on_output_changed).pack(anchor=tk.W, pady=2)

        # 3. Settings Card
        settings_group = ttk.LabelFrame(main_frame, text=" 3. Video & Audio Settings ", padding="10")
        settings_group.pack(fill=tk.X, pady=(0, 10))

        # Resolution & Aspect
        ttk.Label(settings_group, text="Resolution:").grid(row=0, column=0, sticky=tk.W, pady=4)
        cb_res = ttk.Combobox(settings_group, textvariable=self.resolution, values=["400x300 (Full RLCD)", "200x150 (Half Res)"], width=20, state="readonly")
        cb_res.grid(row=0, column=1, sticky=tk.W, padx=6, pady=4)

        ttk.Label(settings_group, text="Scaling:").grid(row=0, column=2, sticky=tk.W, padx=(16, 4), pady=4)
        cb_aspect = ttk.Combobox(settings_group, textvariable=self.aspect_mode, values=["Fit (Letterbox)", "Crop (Fill)", "Stretch"], width=16, state="readonly")
        cb_aspect.grid(row=0, column=3, sticky=tk.W, padx=6, pady=4)

        # FPS & Dithering
        ttk.Label(settings_group, text="Frame Rate:").grid(row=1, column=0, sticky=tk.W, pady=4)
        cb_fps = ttk.Combobox(settings_group, textvariable=self.fps, values=["10", "12", "15", "20", "24", "30"], width=10, state="readonly")
        cb_fps.grid(row=1, column=1, sticky=tk.W, padx=6, pady=4)

        ttk.Label(settings_group, text="Dithering:").grid(row=1, column=2, sticky=tk.W, padx=(16, 4), pady=4)
        cb_dither = ttk.Combobox(settings_group, textvariable=self.dither, values=["bayer (Crisp Pattern)", "floyd_steinberg (Smooth)", "none (Threshold)"], width=22, state="readonly")
        cb_dither.grid(row=1, column=3, sticky=tk.W, padx=6, pady=4)

        # Trimming Row
        self.chk_trim = ttk.Checkbutton(settings_group, text="Trim Clip:", variable=self.enable_trim, command=self.on_trim_toggle)
        self.chk_trim.grid(row=2, column=0, sticky=tk.W, pady=4)

        trim_frame = ttk.Frame(settings_group)
        trim_frame.grid(row=2, column=1, columnspan=3, sticky=tk.W, padx=6, pady=4)
        ttk.Label(trim_frame, text="Start (s):").pack(side=tk.LEFT)
        self.ent_start = ttk.Entry(trim_frame, textvariable=self.trim_start, width=6)
        self.ent_start.pack(side=tk.LEFT, padx=4)
        ttk.Label(trim_frame, text="Duration (s):").pack(side=tk.LEFT, padx=(12, 0))
        self.ent_dur = ttk.Entry(trim_frame, textvariable=self.trim_duration, width=6)
        self.ent_dur.pack(side=tk.LEFT, padx=4)
        self.lbl_trim_hint = ttk.Label(trim_frame, text="(Recommended ≤ 5s for GIF)", foreground="#777")
        self.lbl_trim_hint.pack(side=tk.LEFT, padx=8)

        # 4. Action & Log Card
        action_frame = ttk.Frame(main_frame)
        action_frame.pack(fill=tk.X, pady=(4, 6))

        self.btn_convert = tk.Button(action_frame, text="🚀 CONVERT FOR SOLAROS", font=("Segoe UI", 11, "bold"),
                                     bg="#2e7d32", fg="#ffffff", activebackground="#1b5e20", activeforeground="#ffffff",
                                     padx=20, pady=8, relief=tk.RAISED, command=self.start_conversion)
        self.btn_convert.pack(side=tk.LEFT)

        self.btn_ffmpeg = ttk.Button(action_frame, text="Check / Download FFmpeg", command=self.check_ffmpeg_tool)
        self.btn_ffmpeg.pack(side=tk.RIGHT, padx=4)

        self.progress_bar = ttk.Progressbar(main_frame, mode="indeterminate")
        self.progress_bar.pack(fill=tk.X, pady=6)

        # Console Log
        log_group = ttk.LabelFrame(main_frame, text=" Console Output ", padding="6")
        log_group.pack(fill=tk.BOTH, expand=True)

        self.log_text = scrolledtext.ScrolledText(log_group, height=6, bg="#0d1117", fg="#58a6ff", font=("Consolas", 9))
        self.log_text.pack(fill=tk.BOTH, expand=True)

        self.log("SolarOS Media Converter initialized.")
        if self.ffmpeg_path:
            self.log(f"Found FFmpeg at: {self.ffmpeg_path}")
        else:
            self.log("WARNING: FFmpeg not detected in PATH. Click 'Check / Download FFmpeg' to install automatically.")

    def log(self, text):
        self.log_text.insert(tk.END, text + "\n")
        self.log_text.see(tk.END)

    def on_output_changed(self):
        if self.output_gif.get():
            self.lbl_trim_hint.config(text="(GIF: recommended ≤ 5s to fit ESP32 RAM)")
        else:
            self.lbl_trim_hint.config(text="(Streams from SD card, unlimited duration)")

    def on_trim_toggle(self):
        state = tk.NORMAL if self.enable_trim.get() else tk.DISABLED
        self.ent_start.config(state=state)
        self.ent_dur.config(state=state)

    def browse_input_files(self):
        files = filedialog.askopenfilenames(
            title="Select Video / Animation Files",
            filetypes=[("Video & Media Files", "*.mp4 *.mkv *.avi *.mov *.webm *.gif *.flv *.wmv *.mjpeg"), ("All Files", "*.*")]
        )
        if files:
            self.set_input_files(list(files))

    def browse_input_folder(self):
        folder = filedialog.askdirectory(title="Select Folder Containing Media Files")
        if folder:
            extensions = {".mp4", ".mkv", ".avi", ".mov", ".webm", ".gif", ".flv", ".wmv", ".mjpeg"}
            files = [os.path.join(root, name) for root, _, names in os.walk(folder)
                     for name in names if os.path.splitext(name)[1].lower() in extensions]
            if files:
                self.set_input_files(sorted(files))
            else:
                messagebox.showinfo("No Media Found", "No supported media files were found in this folder.")

    def set_input_files(self, files):
        self.input_files = files
        self.input_file.set(files[0])  # Compatibility with single-file workflows.
        self.input_summary.set(f"{len(files)} file(s) selected" if len(files) > 1 else files[0])
        if files:
            base_dir = os.path.dirname(files[0])
            base_name = self.clean_output_name(os.path.splitext(os.path.basename(files[0]))[0])
            if not self.output_dir.get():
                self.output_dir.set(base_dir)

    @staticmethod
    def clean_output_name(name):
        """Keep only English letters and digits, as required by SolarOS filenames."""
        cleaned = re.sub(r"[^A-Za-z0-9]+", "", name)
        return cleaned or "solarvideo"

    def browse_output_dir(self):
        d = filedialog.askdirectory(title="Select Output Directory (e.g. SD Card)")
        if d:
            self.output_dir.set(d)

    def check_ffmpeg_tool(self):
        self.ffmpeg_path = self.find_ffmpeg()
        if self.ffmpeg_path:
            messagebox.showinfo("FFmpeg Found", f"FFmpeg is available at:\n{self.ffmpeg_path}")
            return

        resp = messagebox.askyesno("FFmpeg Required", "FFmpeg was not found on your computer.\n\nWould you like this tool to automatically download a lightweight static FFmpeg build into the project tools folder?")
        if resp:
            threading.Thread(target=self.download_ffmpeg, daemon=True).start()

    def download_ffmpeg(self):
        self.log("Downloading standalone FFmpeg for Windows (approx 30 MB)...")
        self.progress_bar.start(10)
        try:
            url = "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-gpl.zip"
            zip_dest = os.path.join(os.path.dirname(__file__), "ffmpeg_dl.zip")
            
            urllib.request.urlretrieve(url, zip_dest)
            self.log("Extracting ffmpeg.exe...")
            
            with zipfile.ZipFile(zip_dest, 'r') as zip_ref:
                for item in zip_ref.namelist():
                    if item.endswith("ffmpeg.exe"):
                        with zip_ref.open(item) as src, open(os.path.join(os.path.dirname(__file__), "ffmpeg.exe"), 'wb') as dst:
                            dst.write(src.read())
                        break
            
            if os.path.exists(zip_dest):
                os.remove(zip_dest)
                
            self.ffmpeg_path = os.path.join(os.path.dirname(__file__), "ffmpeg.exe")
            self.log(f"SUCCESS: FFmpeg downloaded and ready at {self.ffmpeg_path}")
            messagebox.showinfo("Success", "FFmpeg downloaded successfully!")
        except Exception as e:
            self.log(f"Error downloading FFmpeg: {e}")
            self.log("Please install FFmpeg manually or add it to PATH.")
            messagebox.showerror("Download Failed", f"Could not download FFmpeg automatically:\n{e}\n\nPlease install FFmpeg on your system.")
        finally:
            self.progress_bar.stop()

    def start_conversion(self):
        inputs = self.input_files or ([self.input_file.get().strip()] if self.input_file.get().strip() else [])
        out_d = self.output_dir.get().strip()

        if not inputs or not all(os.path.isfile(path) for path in inputs):
            messagebox.showerror("Error", "Please select one or more valid input media files.")
            return

        if not out_d:
            messagebox.showerror("Error", "Please select an output directory.")
            return

        if not (self.output_mjpeg.get() or self.output_slv.get() or self.output_gif.get()):
            messagebox.showerror("Error", "Select at least one output format.")
            return

        self.ffmpeg_path = self.find_ffmpeg()
        if not self.ffmpeg_path:
            messagebox.showerror("FFmpeg Missing", "FFmpeg is required for conversion. Click 'Check / Download FFmpeg'.")
            return

        self.btn_convert.config(state=tk.DISABLED)
        self.progress_bar.start(10)

        threading.Thread(target=self.run_conversion_worker, daemon=True).start()

    def run_conversion_worker(self):
        inputs = list(self.input_files) or [self.input_file.get().strip()]
        out_d = self.output_dir.get().strip()
        fps_val = self.fps.get()
        dither_val = self.dither.get().split()[0]

        target_w = 400
        target_h = 300
        if "200x150" in self.resolution.get():
            target_w = 200
            target_h = 150

        # Build scale filter
        aspect = self.aspect_mode.get()
        if "Fit" in aspect:
            scale_filter = f"scale={target_w}:{target_h}:force_original_aspect_ratio=decrease,pad={target_w}:{target_h}:(ow-iw)/2:(oh-ih)/2:color=black"
        elif "Crop" in aspect:
            scale_filter = f"scale={target_w}:{target_h}:force_original_aspect_ratio=increase,crop={target_w}:{target_h}"
        else:
            scale_filter = f"scale={target_w}:{target_h}"

        # Trim args
        trim_args = []
        if self.enable_trim.get():
            try:
                st = float(self.trim_start.get())
                dur = float(self.trim_duration.get())
                trim_args = ["-ss", str(st), "-t", str(dur)]
            except ValueError:
                pass

        try:
            completed = 0
            used_names = set()
            for inp in inputs:
                base_name = self.clean_output_name(os.path.splitext(os.path.basename(inp))[0])
                out_n = base_name
                suffix = 2
                while out_n.lower() in used_names:
                    out_n = f"{base_name}{suffix}"
                    suffix += 1
                used_names.add(out_n.lower())
                self.log(f"--- [{completed + 1}/{len(inputs)}] {os.path.basename(inp)} -> {out_n} ---")
                self.convert_one(inp, out_d, out_n, scale_filter, trim_args, target_w, target_h, fps_val, dither_val)
                completed += 1
            messagebox.showinfo("Done", f"Converted {completed} file(s).\nSaved to: {out_d}")
        except Exception as ex:
            self.log(f"Unexpected Exception: {ex}")
            messagebox.showerror("Error", str(ex))
        finally:
            self.progress_bar.stop()
            self.btn_convert.config(state=tk.NORMAL)

    def convert_one(self, inp, out_d, out_n, scale_filter, trim_args, target_w, target_h, fps_val, dither_val):
        if self.output_gif.get():
                out_path = os.path.join(out_d, f"{out_n}.gif")
                palette_filter = f"[0:v] {scale_filter},format=gray,fps={fps_val},split [a][b]; [a] palettegen=max_colors=16 [p]; [b][p] paletteuse=dither={dither_val}"
                
                cmd = [self.ffmpeg_path, "-y"] + trim_args + ["-i", inp, "-filter_complex", palette_filter, out_path]
                self.log("Running: " + " ".join(cmd))
                res = subprocess.run(cmd, capture_output=True, text=True)
                if res.returncode != 0:
                    self.log(f"FFmpeg Error: {res.stderr}")
                    raise RuntimeError(f"GIF conversion failed for {os.path.basename(inp)}")
                
                size_mb = os.path.getsize(out_path) / (1024 * 1024)
                self.log(f"✅ GIF Created: {out_path} ({size_mb:.2f} MB)")
        if self.output_mjpeg.get():
                out_mjpeg = os.path.join(out_d, f"{out_n}.mjpeg")
                vf = f"{scale_filter},format=gray,fps={fps_val}"
                cmd = [self.ffmpeg_path, "-y"] + trim_args + ["-i", inp, "-vf", vf, "-vcodec", "mjpeg", "-q:v", "4", "-an", out_mjpeg]
                self.log("Extracting MJPEG video: " + " ".join(cmd))
                res = subprocess.run(cmd, capture_output=True, text=True)
                if res.returncode != 0:
                    self.log(f"FFmpeg Error: {res.stderr}")
                    raise RuntimeError(f"MJPEG conversion failed for {os.path.basename(inp)}")

                size_mb = os.path.getsize(out_mjpeg) / (1024 * 1024)
                self.log(f"✅ MJPEG Video Created: {out_mjpeg} ({size_mb:.2f} MB)")

                if self.output_mjpeg_audio.get():
                    out_wav = os.path.join(out_d, f"{out_n}.wav")
                    cmd_audio = [self.ffmpeg_path, "-y"] + trim_args + ["-i", inp, "-vn", "-acodec", "pcm_s16le", "-ac", "1", "-ar", "22050", out_wav]
                    self.log("Extracting WAV Audio: " + " ".join(cmd_audio))
                    res_a = subprocess.run(cmd_audio, capture_output=True, text=True)
                    if res_a.returncode == 0 and os.path.exists(out_wav):
                        self.log(f"✅ WAV Audio Created: {out_wav}")

        if self.output_slv.get():
                out_slv = os.path.join(out_d, f"{out_n}.slv")
                raw_gray = os.path.join(out_d, f"{out_n}_temp.gray")
                
                vf = f"{scale_filter},format=gray,fps={fps_val}"
                cmd = [self.ffmpeg_path, "-y"] + trim_args + ["-i", inp, "-vf", vf, "-f", "rawvideo", "-pix_fmt", "gray", raw_gray]
                self.log("Extracting raw frames...")
                res = subprocess.run(cmd, capture_output=True, text=True)
                if res.returncode != 0:
                    self.log(f"Error: {res.stderr}")
                    raise RuntimeError(f"SLV conversion failed for {os.path.basename(inp)}")

                self.log("Packing into 2-bit SolarOS RLCD stream (.slv)...")
                self.pack_slv_file(raw_gray, out_slv, target_w, target_h, int(fps_val))
                if os.path.exists(raw_gray):
                    os.remove(raw_gray)
                    
                size_mb = os.path.getsize(out_slv) / (1024 * 1024)
                self.log(f"✅ SolarOS Native Stream Created: {out_slv} ({size_mb:.2f} MB)")

    def pack_slv_file(self, raw_gray_path, out_slv_path, width, height, fps):
        """Packs 8-bit raw grayscale frames into 2-bit packed SolarOS video stream."""
        frame_bytes = width * height
        packed_frame_bytes = (width * height) // 4

        total_raw_size = os.path.getsize(raw_gray_path)
        frame_count = total_raw_size // frame_bytes

        with open(raw_gray_path, "rb") as fin, open(out_slv_path, "wb") as fout:
            import struct
            header = struct.pack("<4sHHBBI", b"SLV1", width, height, fps, 0, frame_count)
            fout.write(header)

            for _ in range(frame_count):
                raw = fin.read(frame_bytes)
                if len(raw) < frame_bytes:
                    break
                packed = bytearray(packed_frame_bytes)
                p_idx = 0
                for i in range(0, frame_bytes, 4):
                    b0 = raw[i] >> 6
                    b1 = raw[i+1] >> 6
                    b2 = raw[i+2] >> 6
                    b3 = raw[i+3] >> 6
                    packed[p_idx] = (b0 << 6) | (b1 << 4) | (b2 << 2) | b3
                    p_idx += 1
                fout.write(packed)

if __name__ == "__main__":
    root = tk.Tk()
    app = SolarVideoConverterApp(root)
    root.mainloop()
