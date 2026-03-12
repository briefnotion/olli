# /// script
# dependencies = [
#     "watchdog",
# ]
# ///

import os
import sys
import time
import subprocess
import shutil
import tempfile
import json
from pathlib import Path
from watchdog.observers import Observer
from watchdog.events import FileSystemEventHandler

# --- Configuration ---
BASE_DIR = Path(os.path.expanduser("~/olli_files"))
WATCH_DIR = BASE_DIR / "output"
CONTROL_FILE = BASE_DIR / "lira_control.json"

class LiraControl:
    """Manages the shared state between Lira and the Host."""
    def __init__(self):
        if not BASE_DIR.exists():
            BASE_DIR.mkdir(parents=True)
        self.ensure_config()

    def ensure_config(self):
        if not CONTROL_FILE.exists():
            self.write({"interrupt": False, "is_speaking": False})

    def read(self):
        try:
            with open(CONTROL_FILE, 'r') as f:
                return json.load(f)
        except (json.JSONDecodeError, FileNotFoundError):
            return {"interrupt": False, "is_speaking": False}

    def write(self, data):
        with open(CONTROL_FILE, 'w') as f:
            json.dump(data, f, indent=2)

    def set_status(self, is_speaking):
        data = self.read()
        data["is_speaking"] = is_speaking
        # Reset interrupt when we start/stop
        if is_speaking:
            data["interrupt"] = False 
        self.write(data)

    def check_interrupt(self):
        return self.read().get("interrupt", False)

def get_tts_command(preferred=None):
    engines = ["pico2wave", "festival", "espeak-ng", "espeak"]
    if preferred and shutil.which(preferred):
        return preferred
    for engine in engines:
        if shutil.which(engine):
            return engine
    return None

def get_player_command():
    if shutil.which("paplay"):
        return ["paplay"]
    if shutil.which("aplay"):
        return ["aplay", "-q"]
    return None

class SpeechHandler(FileSystemEventHandler):
    def __init__(self, preferred_engine=None):
        self.preferred_engine = preferred_engine
        self.control = LiraControl()
        super().__init__()

    def on_created(self, event):
        if not event.is_directory:
            self.process_file(event.src_path)

    def clear_pending_files(self):
        """Deletes all files in the watch directory to prevent queued speech."""
        print("Clearing all pending speech files...")
        for item in WATCH_DIR.iterdir():
            if item.is_file():
                try:
                    item.unlink()
                except Exception as e:
                    print(f"Error deleting {item}: {e}")

    def speak_with_interrupt(self, text, engine_name):
        """Plays audio but monitors the control file for an interrupt signal."""
        player_cmd = get_player_command()
        if not player_cmd: return

        temp_wav = None
        process = None
        
        try:
            # 1. Generate the WAV
            fd, temp_wav = tempfile.mkstemp(suffix=".wav")
            os.close(fd)
            
            if engine_name == "pico2wave":
                subprocess.run(["pico2wave", "-w", temp_wav, text], check=True)
            elif engine_name == "festival":
                subprocess.run(f'echo "{text}" | text2wave -o {temp_wav}', shell=True, check=True)
            else:
                # For espeak, we'll just pipe directly to player in the next step
                pass

            # 2. Update Status: Lira is now speaking
            self.control.set_status(True)

            # 3. Start Playback (Non-blocking)
            if engine_name in ["espeak", "espeak-ng"]:
                tts_cmd = [engine_name, "-s", "170", "--stdout", text]
                p1 = subprocess.Popen(tts_cmd, stdout=subprocess.PIPE)
                process = subprocess.Popen(player_cmd, stdin=p1.stdout)
            else:
                process = subprocess.Popen(player_cmd + [temp_wav])

            # 4. Monitor Loop: Wait for finish OR interrupt
            while process.poll() is None:
                if self.control.check_interrupt():
                    print("!!! Interrupt detected. Stopping speech.")
                    process.terminate()
                    # NEW: Wipe the directory so the observer doesn't process the next file
                    self.clear_pending_files()
                    break
                time.sleep(0.1)

        except Exception as e:
            print(f"Speech Error: {e}")
        finally:
            self.control.set_status(False)
            if temp_wav and os.path.exists(temp_wav):
                os.remove(temp_wav)

    def process_file(self, file_path):
        path = Path(file_path)
        # Check if file still exists (might have been wiped by an interrupt)
        if not path.exists():
            return

        time.sleep(0.1) # Small buffer for file write
        
        try:
            # Re-check existence after sleep
            if not path.exists(): return

            content = path.read_text(encoding='utf-8', errors='replace').strip()
            if path.exists(): path.unlink()
            
            if content:
                engine = get_tts_command(self.preferred_engine)
                print(f"Speaking: {content[:50]}...")
                self.speak_with_interrupt(content, engine)
                
        except Exception as e:
            print(f"Error: {e}")

if __name__ == "__main__":
    pref = sys.argv[1] if len(sys.argv) > 1 else None
    
    if not WATCH_DIR.exists(): WATCH_DIR.mkdir(parents=True)
    
    handler = SpeechHandler(preferred_engine=pref)
    observer = Observer()
    observer.schedule(handler, str(WATCH_DIR), recursive=False)
    
    print(f"Lira Active. Control file at: {CONTROL_FILE}")
    observer.start()
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        observer.stop()
    observer.join()