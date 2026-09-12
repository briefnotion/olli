#include "web_server.h"

#include <iostream>

#include "httplib.h"
#include <nlohmann/json.hpp>

namespace
{
    // SSE requires each event's "data:" to be one physical line - json::dump()
    // (no pretty-print) never emits a literal newline, so wrapping every
    // payload (a plain string for llm/thinking/system, {label,url} for a
    // link) in JSON sidesteps hand-rolled escaping entirely. Browser side
    // (the embedded page below) just JSON.parse()s event.data back.
    std::string format_sse_event(const std::string& event_type, const nlohmann::json& payload)
    {
        return "event: " + event_type + "\ndata: " + payload.dump() + "\n\n";
    }

    // Decodes an ncurses attribute (COLOR_PAIR(n) | A_BOLD/A_DIM, as
    // carried in COMMS::INPUT_FROM_LLM_COLOR/INPUT_FROM_USER_COLOR,
    // comms.h) into a CSS color, so the web page can show the same
    // per-instance coloring display_with_ncurses() already does
    // (task-runner cyan/yellow, delegator magenta/green - see tools.cpp).
    // Deliberately NOT calling ncurses' own pair_content() to look this
    // up at runtime - that needs ncurses actually initialized (start_
    // color()/init_pair() already run), which wouldn't hold running
    // headless/web-only. PAIR_NUMBER() below is a pure bit-math macro,
    // safe regardless of ncurses's init state; the pair-number -> color
    // mapping is a hardcoded duplicate of user_io.cpp's own init_pair()
    // calls instead - small and stable, but would need updating here too
    // if those ever change. Empty string means "no override" (attr 0,
    // comms.h's own "plain/undecorated, terminal's own default" case).
    std::string ncurses_attr_to_css_color(int attr)
    {
        if (attr == 0) return "";

        switch (PAIR_NUMBER(attr))
        {
            case 1: return "#9a9a9a"; // PAIR_USER_INPUT_GREY (COLOR_WHITE + A_DIM)
            case 2: return "#56b6c2"; // PAIR_TASK_RUNNER_LLM (COLOR_CYAN)
            case 3: return "#e5c07b"; // PAIR_TASK_RUNNER_USER (COLOR_YELLOW)
            case 4: return "#c678dd"; // PAIR_DELEGATOR_LLM (COLOR_MAGENTA)
            case 5: return "#98c379"; // PAIR_DELEGATOR_USER (COLOR_GREEN)
            default: return "";
        }
    }

    // The whole page: transcript + input box, no external assets (fonts,
    // scripts, styling all inline) - matches olli being fully local/offline
    // even when reached over the LAN. Listens for the same event types
    // push_output() below produces; POSTs a submitted line's raw text to
    // /input, same shape poll_input() reads back out.
    const char* PAGE_HTML = R"HTML(<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>olli</title>
<style>
  html, body { height: 100%; margin: 0; background: #1b1b1b; color: #e8e8e8;
               font-family: -apple-system, Segoe UI, Helvetica, Arial, sans-serif; }
  /* Fixed strip, always visible, own scrollback - mirrors
     display_with_ncurses()'s own win_system (user_io.cpp): system
     messages get their own dedicated 3-line-ish area, never mixed into
     the chat transcript below it. */
  #systemPanel { position: absolute; top: 0; left: 0; right: 180px; height: 60px;
                 box-sizing: border-box; overflow-y: auto; padding: 6px 16px;
                 font-size: 0.85em; color: #6fa8dc; border-bottom: 1px solid #333;
                 white-space: pre-wrap; word-wrap: break-word; }
  #transcript { position: absolute; top: 60px; left: 0; right: 180px; bottom: 56px;
                overflow-y: auto; padding: 12px 16px; white-space: pre-wrap;
                word-wrap: break-word; }
  #transcript .user { color: #9a9a9a; }
  #transcript .link { color: #6fa8dc; }
  #toolsPanel { position: absolute; top: 0; right: 0; bottom: 56px; width: 180px;
                overflow-y: auto; padding: 12px 10px; box-sizing: border-box;
                border-left: 1px solid #333; font-size: 0.85em; color: #b0b0b0; }
  #toolsPanel .title { color: #6fa8dc; margin-bottom: 6px; text-transform: uppercase;
                        font-size: 0.8em; letter-spacing: 0.05em; }
  #toolsPanel ul { list-style: none; margin: 0; padding: 0; }
  #toolsPanel li { padding: 2px 0; overflow-wrap: break-word; }
  /* Floats over the transcript's upper-right corner without displacing
     it, same as display_with_ncurses()'s own win_thinking (user_io.cpp) -
     hidden by default, shown/hidden by JS below. */
  #thinkingBox { display: none; position: absolute; top: 70px; right: 192px;
                 left: 30%; max-height: 160px; overflow-y: auto;
                 background: #232323; border: 1px solid #4a4a4a; border-radius: 4px;
                 padding: 6px 10px; box-shadow: 0 2px 8px rgba(0,0,0,0.4); z-index: 10; }
  #thinkingBox .title { color: #6fa8dc; text-transform: uppercase;
                         font-size: 0.75em; letter-spacing: 0.05em; margin-bottom: 4px; }
  #thinkingBoxContent { color: #9a9a9a; font-style: italic; font-size: 0.85em;
                        white-space: pre-wrap; word-wrap: break-word; }
  #inputRow { position: absolute; left: 0; right: 180px; bottom: 0; height: 56px;
              display: flex; border-top: 1px solid #333; }
  #inputBox { flex: 1; background: #111; color: #e8e8e8; border: 0;
              padding: 0 14px; font-size: 16px; }
  #sendBtn { width: 72px; background: #333; color: #e8e8e8; border: 0;
             font-size: 16px; }
  #inputBox:disabled, #sendBtn:disabled { opacity: 0.4; cursor: not-allowed; }
</style>
</head>
<body>
<div id="systemPanel"></div>
<div id="transcript"></div>
<div id="toolsPanel">
  <div class="title">Tools</div>
  <ul id="toolsList"></ul>
</div>
<div id="thinkingBox">
  <div class="title">thinking</div>
  <div id="thinkingBoxContent"></div>
</div>
<div id="inputRow">
  <input id="inputBox" type="text" placeholder="Message olli..." autofocus>
  <button id="sendBtn">Send</button>
</div>
<script>
  var transcript = document.getElementById('transcript');
  var inputBox = document.getElementById('inputBox');
  var sendBtn = document.getElementById('sendBtn');
  var toolsList = document.getElementById('toolsList');
  var thinkingBox = document.getElementById('thinkingBox');
  var thinkingBoxContent = document.getElementById('thinkingBoxContent');
  var systemPanel = document.getElementById('systemPanel');

  function append(text, cls, color) {
    var atBottom = transcript.scrollHeight - transcript.scrollTop - transcript.clientHeight < 32;
    var div = document.createElement('div');
    if (cls) div.className = cls;
    if (color) div.style.color = color;
    div.textContent = text;
    transcript.appendChild(div);
    if (atBottom) transcript.scrollTop = transcript.scrollHeight;
  }

  // The assistant's reply streams in as many small chunks, one SSE event
  // each (same as ncurses' own chat_response += chunk, user_io.cpp) - all
  // of them belong on one running line, not one <div> per chunk (a <div>
  // is block-level, so a fresh one per chunk forced a line break per
  // chunk). Kept open across chunks, closed (set back to null) whenever a
  // new user turn starts, so the next reply gets its own fresh element.
  // Each chunk gets its own inline <span> (not appended as plain text)
  // so a color change mid-reply (a background task instance starting/
  // stopping - see COMMS::INPUT_FROM_LLM_COLOR's own comment, comms.h)
  // renders correctly without needing a new line - spans are inline,
  // so consecutive ones still flow together visually.
  var currentAssistantDiv = null;

  function appendLLM(payload) {
    var atBottom = transcript.scrollHeight - transcript.scrollTop - transcript.clientHeight < 32;
    if (!currentAssistantDiv) {
      currentAssistantDiv = document.createElement('div');
      transcript.appendChild(currentAssistantDiv);
    }
    var span = document.createElement('span');
    if (payload.color) span.style.color = payload.color;
    span.textContent = payload.text;
    currentAssistantDiv.appendChild(span);
    if (atBottom) transcript.scrollTop = transcript.scrollHeight;
  }

  // Mirrors display_with_ncurses()'s own in_thinking_block/
  // ncurses_thinking_visible/THINKING_BOX_LINGER_MS state machine
  // (user_io.cpp) as closely as a browser allows: thinking text goes in
  // its own floating box, not the transcript; the box closes the moment
  // the real reply starts (not on a thinking-side timeout), lingering 2s
  // first so it doesn't just vanish mid-read.
  var inThinkingBlock = false;
  var thinkingCloseTimer = null;

  function showThinking(text) {
    if (!inThinkingBlock) {
      inThinkingBlock = true;
      if (thinkingCloseTimer) { clearTimeout(thinkingCloseTimer); thinkingCloseTimer = null; }
      thinkingBoxContent.textContent = '';
      thinkingBox.style.display = 'block';
    }
    thinkingBoxContent.textContent += text;
    thinkingBox.scrollTop = thinkingBox.scrollHeight; // #thinkingBox is the scrollable one, not the inner content div
  }

  function closeThinkingSoon() {
    if (!inThinkingBlock) return;
    inThinkingBlock = false;
    thinkingCloseTimer = setTimeout(function() {
      thinkingBox.style.display = 'none';
      thinkingCloseTimer = null;
    }, 2000);
  }

  var src = new EventSource('/events');
  src.addEventListener('llm', function(e) {
    closeThinkingSoon(); // real reply starting - same signal ncurses uses to end thinking
    appendLLM(JSON.parse(e.data));
  });
  src.addEventListener('thinking', function(e) { showThinking(JSON.parse(e.data)); });
  src.addEventListener('system', function(e) {
    // Own dedicated strip, not the transcript - see #systemPanel's own
    // comment (CSS above) for why.
    systemPanel.textContent += JSON.parse(e.data);
    systemPanel.scrollTop = systemPanel.scrollHeight;
  });
  // A submitted line - whichever channel it came from - starts a new
  // turn: shared by send() (this page's own submission) and the 'user'
  // listener below (a keyboard/STT submission relayed in from
  // thread_main(), io_worker.cpp - never this page's own, which already
  // called this via send() the instant it was typed).
  function startNewTurn(text, color) {
    append(text, 'user', color);
    currentAssistantDiv = null;
    if (thinkingCloseTimer) { clearTimeout(thinkingCloseTimer); thinkingCloseTimer = null; }
    inThinkingBlock = false;
    thinkingBox.style.display = 'none';
  }

  src.addEventListener('user', function(e) {
    var payload = JSON.parse(e.data);
    startNewTurn(payload.text, payload.color);
  });
  src.addEventListener('link', function(e) {
    var link = JSON.parse(e.data);
    var div = document.createElement('div');
    div.className = 'link';
    var a = document.createElement('a');
    a.href = link.url; a.textContent = link.label || link.url;
    a.target = '_blank'; a.rel = 'noopener noreferrer';
    div.appendChild(a);
    transcript.appendChild(div);
  });
  src.addEventListener('tools', function(e) {
    var names = JSON.parse(e.data);
    toolsList.innerHTML = '';
    names.forEach(function(name) {
      var li = document.createElement('li');
      li.textContent = name;
      toolsList.appendChild(li);
    });
  });
  // Mirrors ncurses_update_input_box() dimming its own input box when
  // COMMS::ENABLE_KEYBOARD_INPUT is false (comms.h/user_io.cpp) - the
  // native disabled attribute both greys it out and stops it receiving
  // focus/keystrokes at all, which also means send()/the interrupt-ping
  // listener below can't fire while disabled, no extra guard needed.
  src.addEventListener('keyboard_enabled', function(e) {
    var enabled = JSON.parse(e.data);
    inputBox.disabled = !enabled;
    sendBtn.disabled = !enabled;
  });

  function send() {
    // Bare Enter (empty box) submits "\n", same as keyboard_input()'s
    // own behavior (user_io.cpp) rather than being blocked - a running
    // .task script's "press enter to continue" (command_wait_enter(),
    // tools_task_script.cpp) only checks ENTER_PRESSED, never the
    // submitted text's content.
    var text = inputBox.value || '\n';
    startNewTurn(text);
    inputBox.value = '';
    interruptSentForLine = false;
    fetch('/input', { method: 'POST', body: text });
  }
  sendBtn.addEventListener('click', send);
  inputBox.addEventListener('keydown', function(e) {
    if (e.key === 'Enter') send();
  });

  // Closest web equivalent to keyboard_input()'s own "any keystroke
  // interrupts" behavior (user_io.cpp) - one lightweight ping the moment
  // a fresh line starts, not a live per-keystroke sync. Resets once the
  // box empties again (backspaced out without sending, or a send just
  // went through above), so the next fresh line can ping again.
  var interruptSentForLine = false;
  inputBox.addEventListener('input', function() {
    if (inputBox.value.length === 0) {
      interruptSentForLine = false;
    } else if (!interruptSentForLine) {
      interruptSentForLine = true;
      fetch('/interrupt', { method: 'POST' });
    }
  });
</script>
</body>
</html>
)HTML";
}

WEB_SERVER_CLASS::WEB_SERVER_CLASS(int port) : port_(port) {}

WEB_SERVER_CLASS::~WEB_SERVER_CLASS()
{
    stop();
}

void WEB_SERVER_CLASS::register_routes()
{
    server_->Get("/", [](const httplib::Request&, httplib::Response& res)
    {
        res.set_content(PAGE_HTML, "text/html; charset=utf-8");
    });

    server_->Post("/input", [this](const httplib::Request& req, httplib::Response& res)
    {
        if (!req.body.empty())
        {
            std::lock_guard<std::mutex> lock(input_mutex_);
            pending_input_.push_back(req.body);
        }
        res.status = 204;
    });

    server_->Post("/interrupt", [this](const httplib::Request&, httplib::Response& res)
    {
        interrupt_requested_.store(true);
        res.status = 204;
    });

    server_->Get("/events", [this](const httplib::Request&, httplib::Response& res)
    {
        client_count_.fetch_add(1);

        // Force the tools panel and keyboard-enabled state to be
        // (re-)sent to this connection at least once, even if neither
        // has changed lately - see tool_names_dirty_'s own comment
        // (web_server.h).
        {
            std::lock_guard<std::mutex> lock(output_mutex_);
            tool_names_dirty_ = true;
            keyboard_enabled_dirty_ = true;
        }

        res.set_chunked_content_provider("text/event-stream",
            [this](size_t /*offset*/, httplib::DataSink& sink) -> bool
            {
                std::string llm, llm_color, thinking, system_text, user_text, user_color;
                std::vector<TOOL_ATTACHMENT> attachments;
                std::vector<std::string> tool_names;
                bool send_tools = false;
                bool keyboard_enabled = true;
                bool send_keyboard_enabled = false;
                {
                    std::lock_guard<std::mutex> lock(output_mutex_);
                    llm.swap(pending_llm_);
                    llm_color = pending_llm_color_; // current setting, not
                    thinking.swap(pending_thinking_); // a one-shot event -
                    system_text.swap(pending_system_); // copied, not swapped
                    user_text.swap(pending_user_);      // (see its own
                    user_color = pending_user_color_;   // comment, web_server.h)
                    attachments.swap(pending_attachments_);
                    if (tool_names_dirty_)
                    {
                        tool_names = current_tool_names_;
                        tool_names_dirty_ = false;
                        send_tools = true;
                    }
                    if (keyboard_enabled_dirty_)
                    {
                        keyboard_enabled = current_keyboard_enabled_;
                        keyboard_enabled_dirty_ = false;
                        send_keyboard_enabled = true;
                    }
                }

                std::string chunk;
                if (!user_text.empty())
                {
                    chunk += format_sse_event("user", nlohmann::json{{"text", user_text}, {"color", user_color}});
                }
                if (!llm.empty())
                {
                    chunk += format_sse_event("llm", nlohmann::json{{"text", llm}, {"color", llm_color}});
                }
                if (!thinking.empty()) chunk += format_sse_event("thinking", thinking);
                if (!system_text.empty()) chunk += format_sse_event("system", system_text);
                if (send_tools) chunk += format_sse_event("tools", tool_names);
                if (send_keyboard_enabled) chunk += format_sse_event("keyboard_enabled", keyboard_enabled);
                for (const auto& a : attachments)
                {
                    // Only "link" is rendered so far - same as
                    // display_with_ncurses() today (see COMMS::
                    // TOOL_ATTACHMENTS's own comment, comms.h).
                    if (a.type != "link") continue;
                    chunk += format_sse_event("link", nlohmann::json{{"label", a.label}, {"url", a.content}});
                }

                if (!chunk.empty())
                {
                    return sink.write(chunk.data(), chunk.size());
                }

                // Nothing to send yet - httplib calls this provider again
                // in a loop (see write_content_chunked(), httplib.h) as
                // long as it keeps returning true, so sleep briefly rather
                // than busy-spinning while idle.
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
                return running_.load();
            },
            [this](bool /*success*/) { client_count_.fetch_sub(1); });
    });
}

bool WEB_SERVER_CLASS::start()
{
    server_ = std::make_unique<httplib::Server>();
    register_routes();

    running_.store(true);
    server_thread_ = std::thread([this]()
    {
        if (!server_->listen("0.0.0.0", port_))
        {
            std::cerr << "WEB_SERVER_CLASS: failed to listen on port " << port_ << ".\n";
        }
    });

    // listen() binds the socket synchronously before accepting - is_running()
    // flips true once that's actually happened, so poll briefly rather than
    // assuming success the instant the thread's been launched.
    for (int i = 0; i < 100 && !server_->is_running(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (!server_->is_running())
    {
        std::cerr << "WEB_SERVER_CLASS: server did not start listening on port " << port_ << ".\n";
        stop();
        return false;
    }

    return true;
}

void WEB_SERVER_CLASS::stop()
{
    running_.store(false);
    if (server_) server_->stop();
    if (server_thread_.joinable()) server_thread_.join();
    server_.reset();
}

bool WEB_SERVER_CLASS::poll_input(std::string& out)
{
    std::lock_guard<std::mutex> lock(input_mutex_);
    if (pending_input_.empty()) return false;
    out = std::move(pending_input_.front());
    pending_input_.pop_front();
    return true;
}

bool WEB_SERVER_CLASS::poll_interrupt()
{
    return interrupt_requested_.exchange(false);
}

void WEB_SERVER_CLASS::push_output(const std::string& llm_text, const std::string& thinking_text,
                                    const std::string& system_text,
                                    const std::vector<TOOL_ATTACHMENT>& attachments,
                                    int llm_color_attr)
{
    std::lock_guard<std::mutex> lock(output_mutex_);
    pending_llm_ += llm_text;
    pending_llm_color_ = ncurses_attr_to_css_color(llm_color_attr);
    pending_thinking_ += thinking_text;
    pending_system_ += system_text;
    pending_attachments_.insert(pending_attachments_.end(), attachments.begin(), attachments.end());
}

void WEB_SERVER_CLASS::push_user_message(const std::string& text, int color_attr)
{
    std::lock_guard<std::mutex> lock(output_mutex_);
    pending_user_ += text;
    pending_user_color_ = ncurses_attr_to_css_color(color_attr);
}

void WEB_SERVER_CLASS::push_tool_names(const std::vector<std::string>& names)
{
    std::lock_guard<std::mutex> lock(output_mutex_);
    if (names != current_tool_names_)
    {
        current_tool_names_ = names;
        tool_names_dirty_ = true;
    }
}

void WEB_SERVER_CLASS::push_keyboard_enabled(bool enabled)
{
    std::lock_guard<std::mutex> lock(output_mutex_);
    if (enabled != current_keyboard_enabled_)
    {
        current_keyboard_enabled_ = enabled;
        keyboard_enabled_dirty_ = true;
    }
}

bool WEB_SERVER_CLASS::has_client() const
{
    return client_count_.load() > 0;
}
