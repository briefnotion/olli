# /// script
# dependencies = [
#   "faster-whisper",
#   "SpeechRecognition",
#   "pyaudio",
#   "numpy",
# ]
# ///

"""
================================================================================
PROGRAM: Voca Pro (v3.2.12) - Smart Command Tracking
================================================================================
Updates:
- FUZZY MATCHING: Checks for variations of "start listening".
- ROBUST I/O: Added retry logic for JSON files to prevent race conditions.
- ASYNC AUDIO: Beeps are now non-blocking.
- AUTO-CLEANUP: Automatically deletes old interaction files (>24h).
- VIEWPORT: Improved noise gate settings for better accuracy in noisy rooms.
================================================================================
"""

import os
import sys
import time
import io
import threading
import math
import json
import subprocess
import numpy as np
import pyaudio
import speech_recognition as sr
from datetime import datetime, timedelta
from queue import Queue, Empty
from contextlib import contextmanager
from faster_whisper import WhisperModel

# Force suppress ONNX/Library logs
os.environ["ORT_LOGGING_LEVEL"] = "3"
os.environ["TF_CPP_MIN_LOG_LEVEL"] = "3"

# --- CONFIGURATION & PATHS ---
OLLI_DIR = os.path.expanduser("~/olli_files")
INTERACTIONS_DIR = os.path.join(OLLI_DIR, "input")

STATUS_FILE = os.path.join(OLLI_DIR, "voca_status.json")
COMMAND_FILE = os.path.join(OLLI_DIR, "voca_command.json")

WAKE_PHRASES = ["start listening", "started listening", "start listener", "star listening"] 
SLEEP_PHRASE = "stop listening"
INTERRUPT_PHRASE = "stop talking"
AUTO_SLEEP_TIMEOUT = 300  # 5 minutes
CLEANUP_INTERVAL = 3600   # Check for old files every hour

# UI Styling
C_GREEN = "\033[92m"
C_YELLOW = "\033[93m"
C_RED = "\033[91m"
C_CYAN = "\033[96m"
C_GRAY = "\033[90m"
C_MAGENTA = "\033[95m"
C_WHITE = "\033[97m"
C_RESET = "\033[0m"

def _play_beep_async(frequency, duration_sec, volume):
    """Worker function for the background beep thread."""
    try:
        sample_rate = 44100
        t = np.linspace(0, duration_sec, int(duration_sec * sample_rate), False)
        tone = np.sin(frequency * t * 2 * np.pi)
        audio = (tone * (volume * 32767)).astype(np.int16)
        audio_bytes = audio.tobytes()
        
        process = subprocess.Popen(
            ['aplay', '-r', str(sample_rate), '-f', 'S16_LE', '-t', 'raw', '-c', '1'],
            stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
        )
        process.communicate(input=audio_bytes)
    except Exception:
        pass

def make_beep(frequency=440, duration_sec=0.1, volume=0.5):
    """Triggers a beep without blocking the main thread."""
    threading.Thread(target=_play_beep_async, args=(frequency, duration_sec, volume), daemon=True).start()

@contextmanager
def silence_all():
    """Deep suppression of C-level stderr/stdout."""
    new_target = open(os.devnull, "w")
    old_stdout, old_stderr = sys.stdout, sys.stderr
    old_stdout_fd, old_stderr_fd = os.dup(1), os.dup(2)
    try:
        os.dup2(new_target.fileno(), 1)
        os.dup2(new_target.fileno(), 2)
        sys.stdout, sys.stderr = new_target, new_target
        yield
    finally:
        os.dup2(old_stdout_fd, 1)
        os.dup2(old_stderr_fd, 2)
        sys.stdout, sys.stderr = old_stdout, old_stderr
        os.close(old_stdout_fd)
        os.close(old_stderr_fd)
        new_target.close()

class VocaNode:
    def __init__(self, model_size="small.en"):
        self.is_running = True
        self.is_awake = False 
        self.is_busy = False   
        self.audio_queue = Queue()
        self.last_reported_status = None
        self.last_processed_cmd_time = 0
        self.current_rms = 0
        self.last_speech_time = time.time()
        self.last_cleanup_time = 0
        
        os.makedirs(INTERACTIONS_DIR, exist_ok=True)
        self.report_status("sleep")

        print(f"{C_CYAN}Initializing Whisper Engine ({model_size}) on CPU...{C_RESET}")
        with silence_all():
            try:
                self.model = WhisperModel(model_size, device="cpu", compute_type="int8", cpu_threads=8)
            except Exception:
                self.model = WhisperModel("tiny.en", device="cpu", compute_type="int8")
        
        self.recognizer = sr.Recognizer()
        # Tweaked for better phrase detection
        self.recognizer.pause_threshold = 1.2 
        self.recognizer.non_speaking_duration = 0.5
        self.recognizer.dynamic_energy_threshold = True
        
    def get_rms(self, audio_data):
        shortcut = np.frombuffer(audio_data.get_raw_data(), dtype=np.int16)
        if len(shortcut) == 0: return 0
        sum_squares = np.sum(shortcut.astype(float)**2)
        return math.sqrt(sum_squares / len(shortcut))

    def check_host_commands(self):
        """Checks for commands from Host with retry logic to avoid race conditions."""
        if not os.path.exists(COMMAND_FILE): return
        
        for _ in range(3): # Simple retry logic
            try:
                with open(COMMAND_FILE, "r") as f:
                    data = json.load(f)
                    command = data.get("command", "").lower()
                    cmd_time = data.get("last_update", 0)
                    if not command or cmd_time <= self.last_processed_cmd_time: return

                    self.last_processed_cmd_time = cmd_time
                    print(f"\n{C_CYAN}[HOST COMMAND] {command.upper()}{C_RESET}")

                    if command == "pause":
                        self.is_busy = True
                    elif command == "resume":
                        self.is_busy = False
                        self.last_speech_time = time.time()
                    elif command == "sleep":
                        if self.is_awake: make_beep(440, 0.1)
                        self.is_busy = False
                        self.is_awake = False
                    elif command == "listen":
                        if not self.is_awake: make_beep(880, 0.1)
                        self.is_busy = False
                        self.is_awake = True
                        self.last_speech_time = time.time()
                break # Success
            except (json.JSONDecodeError, IOError):
                time.sleep(0.05)

    def report_status(self, status):
        """Reports status to Host with file safety."""
        if self.last_reported_status == status: return
        try:
            temp_file = STATUS_FILE + ".tmp"
            with open(temp_file, "w") as f:
                json.dump({
                    "status": status, 
                    "timestamp": time.time(), 
                    "is_awake": self.is_awake, 
                    "is_busy": self.is_busy
                }, f)
            os.replace(temp_file, STATUS_FILE) # Atomic swap
            self.last_reported_status = status
        except Exception: pass

    def cleanup_old_files(self):
        """Deletes transcription files older than 24 hours."""
        now = time.time()
        if now - self.last_cleanup_time < CLEANUP_INTERVAL: return
        self.last_cleanup_time = now
        
        try:
            for filename in os.listdir(INTERACTIONS_DIR):
                file_path = os.path.join(INTERACTIONS_DIR, filename)
                if os.path.isfile(file_path):
                    file_age = now - os.path.getmtime(file_path)
                    if file_age > 86400: # 24 hours
                        os.remove(file_path)
        except Exception: pass

    def save_transcription(self, text, is_interrupt=False):
        ts = datetime.now().strftime("%Y%m%d_%H%M%S")
        prefix = "interrupt" if is_interrupt else "input"
        filepath = os.path.join(INTERACTIONS_DIR, f"{prefix}_{ts}_voca.txt")
        with open(filepath, "w", encoding="utf-8") as f:
            f.write(text)

    def process_audio_worker(self):
        while self.is_running:
            try:
                audio_data = self.audio_queue.get(timeout=0.5)
                wav_stream = io.BytesIO(audio_data.get_wav_data())
                segments, _ = self.model.transcribe(wav_stream, beam_size=1, language="en", vad_filter=True)
                full_text = " ".join([seg.text for seg in segments]).strip()
                if not full_text: continue
                clean_text = full_text.lower()

                # 1. INTERRUPT CHECK
                if self.is_busy:
                    if INTERRUPT_PHRASE in clean_text:
                        print(f"\n{C_RED}[INTERRUPT]{C_RESET} Signaling host...")
                        self.save_transcription(full_text, is_interrupt=True)
                    continue

                # 2. WAKE WORD CHECK (Enhanced with Fuzzy matching)
                if not self.is_awake:
                    found_phrase = next((p for p in WAKE_PHRASES if p in clean_text), None)
                    if found_phrase:
                        make_beep(880, 0.1)
                        self.is_awake = True
                        self.last_speech_time = time.time()
                        print(f"\n{C_MAGENTA}[WAKE]{C_RESET} Triggered by: '{found_phrase}'")
                        
                        start_idx = clean_text.find(found_phrase) + len(found_phrase)
                        remaining_text = full_text[start_idx:].strip().lstrip('.,!?; ')
                        
                        if remaining_text:
                            print(f"{C_GREEN}>> [COMBO]{C_RESET} {C_WHITE}{remaining_text}{C_RESET}")
                            self.save_transcription(remaining_text)
                        continue
                    else:
                        continue
                
                # 3. SLEEP PHRASE CHECK
                if SLEEP_PHRASE in clean_text:
                    make_beep(440, 0.1)
                    self.is_awake = False
                    print(f"\n{C_YELLOW}[SLEEP]{C_RESET} Manual sleep.")
                    continue

                # 4. NORMAL TRANSCRIPTION
                self.last_speech_time = time.time()
                print(f"\n{C_GREEN}>>{C_RESET} {C_WHITE}{full_text}{C_RESET}")
                self.save_transcription(full_text)
            except Exception: pass

    def run(self):
        print(f"{C_GREEN}--- Voca Pro (v3.2.12) Active ---{C_RESET}")
        with silence_all():
            mic = sr.Microphone(sample_rate=16000)
        try:
            with mic as source:
                self.recognizer.adjust_for_ambient_noise(source, duration=1.0)
        except Exception as e:
            print(f"{C_RED}Hardware Error: {e}{C_RESET}"); return

        threading.Thread(target=self.process_audio_worker, daemon=True).start()
        def callback(recognizer, audio):
            self.current_rms = self.get_rms(audio)
            self.audio_queue.put(audio)
        stop_fn = self.recognizer.listen_in_background(mic, callback, phrase_time_limit=30)

        try:
            spinner = ["⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"]
            idx = 0
            while self.is_running:
                self.check_host_commands()
                self.cleanup_old_files()
                
                if self.is_busy:
                    status_lbl = f"{C_RED}[PAUSED]"
                    self.report_status("pause")
                else:
                    now = time.time()
                    if self.is_awake and (now - self.last_speech_time > AUTO_SLEEP_TIMEOUT):
                        make_beep(440, 0.1)
                        self.is_awake = False
                        print(f"\n{C_GRAY}[SLEEP] Auto-timeout.{C_RESET}")

                    if self.is_awake:
                        status_lbl = f"{C_GREEN}[AWAKE]"
                        self.report_status("listen")
                    else:
                        status_lbl = f"{C_YELLOW}[ZzZzz]"
                        self.report_status("sleep")

                vol_bar = ("■" * min(int(self.current_rms / 50), 10)).ljust(10)
                sys.stdout.write(f"\r{status_lbl}{C_RESET} Mic: {C_CYAN}{vol_bar}{C_RESET} {spinner[idx % len(spinner)]}   ")
                sys.stdout.flush()
                idx += 1
                time.sleep(0.1)
        except KeyboardInterrupt: pass
        finally:
            self.is_running = False
            stop_fn(wait_for_stop=False)
            print(f"\n{C_YELLOW}Voca Offline.{C_RESET}")

if __name__ == "__main__":
    VocaNode().run()