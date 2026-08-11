#include "tts.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <vector>

namespace fs = std::filesystem;

namespace {

// Exit code we force in the child when execlp() itself fails (see synthesize()/
// playback() below), so the parent can tell "binary not found" apart from "binary
// ran and returned an error" (e.g. espeak-ng given an uninstalled voice).
constexpr int kExecFailedExitCode = 127;

void reportFailure(const char* tool, int status) {
    if (WIFEXITED(status) && WEXITSTATUS(status) == kExecFailedExitCode) {
        std::cerr << "tts: '" << tool << "' not found - is it installed? "
                     "(see README for dependencies)\n";
    } else if (WIFEXITED(status)) {
        std::cerr << "tts: '" << tool << "' failed (exit code " << WEXITSTATUS(status) << ")\n";
    } else {
        std::cerr << "tts: '" << tool << "' terminated abnormally\n";
    }
}

std::string makeTempWavPath() {
    std::string tmpl = "/tmp/tts_XXXXXX.wav";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    int fd = mkstemps(buf.data(), 4); // ".wav" suffix length
    if (fd < 0) return {};
    close(fd);
    return std::string(buf.data());
}

void redirectStdoutStderrToDevNull() {
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull < 0) return;
    dup2(devnull, STDOUT_FILENO);
    dup2(devnull, STDERR_FILENO);
    close(devnull);
}

} // namespace

TextToSpeech::TextToSpeech(std::string voice, int wordsPerMinute)
    : voice_(std::move(voice)), speed_(wordsPerMinute) {
    workerThread_ = std::thread([this] { workerLoop(); });
}

TextToSpeech::~TextToSpeech() {
    stop();
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        shuttingDown_ = true;
    }
    queueCv_.notify_one();
    if (workerThread_.joinable()) workerThread_.join();
}

void TextToSpeech::setVoice(const std::string& voice) { voice_ = voice; }

void TextToSpeech::setSpeed(int wordsPerMinute) { speed_ = wordsPerMinute; }

bool TextToSpeech::isSpeaking() const { return speaking_.load(); }

bool TextToSpeech::synthesize(const std::string& text, const std::string& wavPath) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return false;

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return false;
    }

    if (pid == 0) {
        dup2(pipefd[0], STDIN_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        redirectStdoutStderrToDevNull();
        execlp("espeak-ng", "espeak-ng",
               "-v", voice_.c_str(),
               "-s", std::to_string(speed_).c_str(),
               "-w", wavPath.c_str(),
               static_cast<char*>(nullptr));
        _exit(kExecFailedExitCode);
    }

    close(pipefd[0]);
    // Text goes over stdin (not argv or a shell string), so no command-injection risk.
    ssize_t written = write(pipefd[1], text.data(), text.size());
    (void)written;
    close(pipefd[1]);

    int status = 0;
    waitpid(pid, &status, 0);
    bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (!ok) reportFailure("espeak-ng", status);
    return ok;
}

bool TextToSpeech::playback(const std::string& wavPath) {
    pid_t pid = fork();
    if (pid < 0) return false;

    if (pid == 0) {
        redirectStdoutStderrToDevNull();
        execlp("aplay", "aplay", "-q", wavPath.c_str(), static_cast<char*>(nullptr));
        _exit(kExecFailedExitCode);
    }

    playerPid_ = pid;
    int status = 0;
    waitpid(pid, &status, 0);
    playerPid_ = -1;
    bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (!ok && !stopRequested_) reportFailure("aplay", status);
    return ok;
}

bool TextToSpeech::speak(const std::string& text) {
    if (text.empty()) return true;

    std::lock_guard<std::mutex> lock(speakMutex_);
    stopRequested_ = false;
    std::string wavPath = makeTempWavPath();
    if (wavPath.empty()) return false;

    speaking_ = true;
    bool ok = synthesize(text, wavPath);
    if (ok) {
        // stop() may have arrived while we were still synthesizing, before playerPid_
        // was set - catch that here instead of playing an utterance no one asked for.
        ok = stopRequested_ ? false : playback(wavPath);
    }
    speaking_ = false;

    std::error_code ec;
    fs::remove(wavPath, ec);
    return ok;
}

void TextToSpeech::speakAsync(const std::string& text) {
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        queue_.push_back(text);
    }
    queueCv_.notify_one();
}

void TextToSpeech::stop() {
    stopRequested_ = true;
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        queue_.clear();
    }
    int pid = playerPid_.load();
    if (pid > 0) kill(pid, SIGTERM);
}

void TextToSpeech::workerLoop() {
    while (true) {
        std::string text;
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            queueCv_.wait(lock, [this] { return shuttingDown_ || !queue_.empty(); });
            if (shuttingDown_ && queue_.empty()) return;
            text = std::move(queue_.front());
            queue_.pop_front();
        }
        speak(text);
    }
}
