# Build & Install

Two ways to get olli built and running, manual or Claude-Code-assisted.
Pick whichever fits:

- **Option A** below if you'd rather describe what you want and let an
  agent adapt the build to your machine, fix what breaks, and check back
  with you on anything risky or anything it can't verify itself (sound,
  the on-screen display).
- **Option B** if you want to drive the build yourself.

Either way, olli targets **Linux** — audio playback/synthesis
(`espeak-ng`/`aplay`) and raw-terminal input are POSIX/ALSA-specific, not
portable as-is. On Windows, WSL is the realistic path, not native Windows.
On WSL specifically, audio only works with WSLg, and can still be flaky
even then.

Two more things worth knowing before you start:

- The ncurses display needs a real, recognized `$TERM`. If it isn't one,
  `initscr()` kills the whole process the instant olli starts — not a
  build problem, just run it in a normal interactive terminal (this is
  what to check first if it builds fine but dies immediately on launch).
- The default model, `qwen3:8b`, needs real RAM to run at a usable speed.
  If olli responds correctly but takes a very long time, that's likely
  the machine, not a broken install — try a smaller tool-capable model
  if it's unusably slow.

---

## Option A: Install via Claude Code

Prerequisite — Claude Code itself needs to be installed and logged in. If
it isn't yet:

```bash
npm install -g @anthropic-ai/claude-code
```

then run `claude` once and follow the login prompt.

Once that's done, copy the block below verbatim as your first message to
a fresh Claude Code session. It's written to be self-contained — it
doesn't assume you've read anything else on this page.

```
-------------------------------------------------------------------
I want you to install and get running a personal project of mine
called olli - a fully local, offline voice assistant written in
C++. Source: https://github.com/briefnotion/olli

Read README.md, BUILD_AND_INSTALL.md, and SETUP.txt in the repo fully
before doing anything - BUILD_AND_INSTALL.md and SETUP.txt describe
the normal manual build/deploy process. Treat this as what you're
adapting, not replacing.

Known constraints worth knowing up front:
- The project targets Linux. Audio playback/synthesis (aplay,
  espeak-ng) and raw-terminal input are POSIX/ALSA-specific, not
  portable as-is.
- The build depends on two header-only libraries (cpp-httplib,
  nlohmann/json) and a from-source static ncursesw+panel build,
  normally laid out as sibling directories next to the repo
  checkout.
- The persistent-session layer (tmux + tmux-resurrect/continuum,
  SETUP.txt steps 1-4 and 6-8) is optional, separate from just
  building and running the binary.
- It expects a local Ollama server with a tool-capable model
  pulled.
- If this is WSL: audio (aplay/PulseAudio) only works with WSLg,
  and can still be broken even then. Before building anything,
  check whether it's actually available (`/dev/snd` present?
  `pactl info` or `aplay -l` work?) and tell me up front if audio
  looks unavailable, rather than discovering it only after a full
  build - I may want to proceed text-only instead of debugging
  WSL audio.
- The ncurses display needs a real, recognized $TERM - if it's not
  one, olli crashes immediately on launch via initscr(). If a build
  succeeds but the binary dies the instant you run it, check $TERM
  and how you're launching it (a real interactive terminal, not a
  piped/non-interactive one) before assuming the build itself is
  broken.
- The default model, qwen3:8b, needs real RAM to run at a usable
  speed. If it responds correctly but very slowly, that's likely
  this machine's resources, not a bad install - mention it to me
  rather than treating it as a bug, and suggest a smaller
  tool-capable model if it's unusably slow.

Before doing anything else, ask me:
1. Where to install it (target directory/machine - assume this
   machine unless I say otherwise).
2. What OS/environment this actually is (Linux distro, macOS, or
   Windows - and if Windows, whether WSL is available/preferred,
   since native Windows isn't realistic given the POSIX audio and
   terminal code).
3. Whether I want the full persistent-session layer (tmux +
   resurrect/continuum) or just a working build I can run by hand.
4. Whether I want voice I/O working (espeak-ng/aplay or a platform
   equivalent) or text-only is fine for now.

Then work the plan, adapting each step to what you learn:
- Work out the full list of system packages you'll need for the
  detected OS/package manager, show me the whole list once, and
  get one yes/no before installing any of them - don't ask
  package by package. The same goes for anything else outside the
  checkout/install target (dotfiles, tmux config, etc).
- Set up cpp-httplib, nlohmann/json, and a built static
  ncursesw+panel per the README's "Building ncursesw" section.
- Run the CMake configure/build. When it fails, diagnose and fix
  it rather than giving up - that includes editing olli's own
  source (the .cpp/.h files, CMakeLists.txt) when the failure is a
  real portability gap (e.g. an aplay/ALSA call that doesn't exist
  on macOS, a POSIX header missing on the target, a warning-as-
  error tripped by a newer compiler than this was built with).
  Keep such fixes as small and targeted as possible, and tell me
  what you changed and why - don't rewrite working Linux behavior,
  only add or adapt what's needed to also work here.
- If you need to modify source to make this platform work, stop
  and ask first whether I want that done on a fork (rather than a
  plain local clone) so the changes are tracked, and separately
  whether I'd like a PR opened upstream with the portability fix
  once it's working - two different questions, don't assume yes
  to either. If I don't have or don't want to involve a GitHub
  account, default to a plain local clone with no fork - don't
  push on this.
- Get the binary actually running end-to-end against a local
  Ollama server (offer to install/start Ollama and pull a
  tool-capable model, e.g. qwen3:8b, if none is available).
- If I asked for the persistent-session layer, set up
  tmux/TPM/config and the pane layout, adapting the
  tmux-resurrect process whitelist and any paths for wherever
  this actually got installed.

Verification: a clean exit code or "it launched" isn't enough
proof for anything involving sound or the on-screen display - you
can't hear or see either one. Once it's running, tell me exactly
what I should be seeing or hearing and ask me to confirm before
calling that part done (e.g. "it should have just spoken a
greeting out loud - did you hear it?" / "you should see boxed
panels with a prompt at the bottom - does yours look like that?").
Iterate on what I report back rather than marking voice or
display as working on your own say-so.

Confirm with me before: installing system packages, forking or
pushing to a remote, or opening a PR. Everything else - reading
files, building, local edits inside the checkout - just do.

When you're done, tell me plainly what works, what doesn't, and
what (if anything) is Linux-only or unsupported on this platform.
-------------------------------------------------------------------
```

(This same block also ships alongside every build, as `SETUP.txt` —
`build/SETUP.txt` in the repo, copied to `~/olli/SETUP.txt` by
`install_to_home.sh` — for reference once you already have olli
installed somewhere.)

---

## Option B: Manual build

### Dependencies

- A C++17 compiler (GCC or Clang) and **CMake ≥ 3.10**
- **libcurl** development headers — `sudo apt install libcurl4-openssl-dev`
- **[cpp-httplib](https://github.com/yhirose/cpp-httplib)** (header-only)
- **[nlohmann/json](https://github.com/nlohmann/json)** (header-only)
- **ncursesw** (wide-char ncurses), including its **panel** library
  (`libpanelw`) — for the windowed display (see
  [Display](README.md#display) in the README). Not packaged system-wide
  here, so it's built from source into its own local install prefix,
  kept separate from olli. Panels come from the same build, no extra
  configure flags needed.

By default the build looks for the two header-only libraries as sibling
checkouts next to this repository:

```
code/
├── olli/            ← this repo
├── cpp-httplib/     ← git clone https://github.com/yhirose/cpp-httplib
├── json/            ← git clone https://github.com/nlohmann/json
└── ncurses-snapshots/  ← see below
```

If you keep them elsewhere (or installed system-wide), point CMake at them:

```bash
cmake -S source -B build \
      -DHTTPLIB_INCLUDE_DIR=/path/to/cpp-httplib \
      -DJSON_INCLUDE_DIR=/path/to/json/include
```

#### Building ncursesw

CMake looks for a *built* ncursesw next to this repo (`../../ncurses-snapshots`,
same sibling convention as above) — it doesn't build ncurses itself, since
ncurses uses autotools, not CMake. One-time setup:

```bash
git clone --depth 1 https://github.com/ThomasDickey/ncurses-snapshots.git
cd ncurses-snapshots
mkdir build && cd build
../configure --prefix="$(pwd)/../install" \
      --without-shared --with-normal --enable-widec \
      --without-debug --without-ada --without-tests \
      --without-manpages --without-progs
make -j
make install
```

Static (`--without-shared`) so the `olli` binary stays self-contained, and
wide-char (`--enable-widec`) so UTF-8 renders correctly. The install prefix
also ends up with its own bundled terminfo database, so nothing about it
depends on what's installed system-wide.

### Compile

```bash
# from the repo root
cmake -S source -B build
cmake --build build -j
```

The resulting `olli` binary lands in `build/`. (The `build/cmak.sh` and
`build/m` scripts are one-line shortcuts for these two steps.)

The project builds with strict warnings-as-errors (`-Wall -Wextra -Wpedantic
-Wconversion … -Werror`); third-party headers are included as `SYSTEM` so their
warnings don't break the build.

### Running

olli expects an Ollama server on `localhost:11434` with a **tool-capable**
model pulled. The default is `qwen3:8b`:

```bash
ollama serve            # if not already running
ollama pull qwen3:8b
```

Then start olli:

```bash
./build/olli          # shared settings, ~/olli_files/
./build/olli ron      # ron's own settings, ~/olli_files_ron/ (see [Profiles](README.md#profiles))
./build/olli --help   # usage, no model/audio/profile init - exits immediately
```

Voice input and output both run in-process, so there's nothing else to start.
You can also just type — speech input is optional; speech output is available
either way.

### Text-to-speech

Speech is synthesized and played by olli itself (`TextToSpeech`, declared and
defined in `io_worker.h`/`.cpp` - see [How it works](README.md#how-it-works)),
which shells out to `espeak-ng` for synthesis and `aplay` for playback — no
Python, no separate process:

```bash
sudo apt install espeak-ng   # synthesis
sudo apt install alsa-utils  # aplay
```

---

## Sanity check: is it actually working?

A build finishing with no errors doesn't mean olli is actually working —
check for these once it's running:

- **It starts and stays up.** If it builds fine but exits or crashes the
  instant you run it, check `$TERM` first (see the caveat above) before
  assuming the build is broken.
- **Text works.** Type something and olli should respond in the display
  (or plainly in the terminal if `USE_NCURSES` is off) within a
  reasonable time. A very slow first response can just be the model
  loading or the machine being underpowered for `qwen3:8b` — a wrong or
  garbled response, or no response at all, is the real red flag.
- **Voice works, if you set it up.** olli should speak a greeting out
  loud shortly after startup, and speak its replies back. If nothing is
  audible, that's most likely `espeak-ng`/`aplay` (or, on WSL, WSLg
  audio) rather than olli itself — worth double-checking those work
  standalone (e.g. `espeak-ng "test"`) before digging into olli's code.

If you're working through Option A, this is exactly the kind of thing to
report back when Claude Code asks you to confirm what you're seeing or
hearing.

---

## Optional: always-on via tmux

Either install path gets you a working `olli` binary you can run by
hand. If you want it to survive reboots and auto-restart in a persistent
tmux session instead, see `SETUP.txt` (ships in `build/`, and gets copied
to `~/olli/` once you deploy) for the full tmux + tmux-resurrect/continuum
setup.
