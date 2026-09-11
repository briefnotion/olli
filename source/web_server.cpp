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
  #transcript { position: absolute; top: 0; left: 0; right: 180px; bottom: 56px;
                overflow-y: auto; padding: 12px 16px; white-space: pre-wrap;
                word-wrap: break-word; }
  #transcript .user { color: #9a9a9a; }
  #transcript .system { color: #6fa8dc; font-size: 0.9em; }
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
  #thinkingBox { display: none; position: absolute; top: 10px; right: 192px;
                 width: 220px; max-height: 160px; overflow-y: auto;
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
</style>
</head>
<body>
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
  var toolsList = document.getElementById('toolsList');
  var thinkingBox = document.getElementById('thinkingBox');
  var thinkingBoxContent = document.getElementById('thinkingBoxContent');

  function append(text, cls) {
    var atBottom = transcript.scrollHeight - transcript.scrollTop - transcript.clientHeight < 32;
    var span = document.createElement('div');
    if (cls) span.className = cls;
    span.textContent = text;
    transcript.appendChild(span);
    if (atBottom) transcript.scrollTop = transcript.scrollHeight;
  }

  // The assistant's reply streams in as many small chunks, one SSE event
  // each (same as ncurses' own chat_response += chunk, user_io.cpp) - all
  // of them belong on one running line, not one <div> per chunk (a <div>
  // is block-level, so a fresh one per chunk forced a line break per
  // chunk). Kept open across chunks, closed (set back to null) whenever a
  // new user turn starts, so the next reply gets its own fresh element.
  var currentAssistantDiv = null;

  function appendLLM(text) {
    var atBottom = transcript.scrollHeight - transcript.scrollTop - transcript.clientHeight < 32;
    if (!currentAssistantDiv) {
      currentAssistantDiv = document.createElement('div');
      transcript.appendChild(currentAssistantDiv);
    }
    currentAssistantDiv.textContent += text;
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
  src.addEventListener('system', function(e) { append(JSON.parse(e.data), 'system'); });
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

  function send() {
    var text = inputBox.value;
    if (!text) return;
    append(text, 'user');
    currentAssistantDiv = null; // next reply starts its own fresh element
    if (thinkingCloseTimer) { clearTimeout(thinkingCloseTimer); thinkingCloseTimer = null; }
    inThinkingBlock = false;
    thinkingBox.style.display = 'none';
    inputBox.value = '';
    fetch('/input', { method: 'POST', body: text });
  }
  document.getElementById('sendBtn').addEventListener('click', send);
  inputBox.addEventListener('keydown', function(e) {
    if (e.key === 'Enter') send();
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

    server_->Get("/events", [this](const httplib::Request&, httplib::Response& res)
    {
        client_count_.fetch_add(1);

        // Force the tools panel to be (re-)sent to this connection at
        // least once, even if it hasn't changed lately - see
        // tool_names_dirty_'s own comment (web_server.h).
        {
            std::lock_guard<std::mutex> lock(output_mutex_);
            tool_names_dirty_ = true;
        }

        res.set_chunked_content_provider("text/event-stream",
            [this](size_t /*offset*/, httplib::DataSink& sink) -> bool
            {
                std::string llm, thinking, system_text;
                std::vector<TOOL_ATTACHMENT> attachments;
                std::vector<std::string> tool_names;
                bool send_tools = false;
                {
                    std::lock_guard<std::mutex> lock(output_mutex_);
                    llm.swap(pending_llm_);
                    thinking.swap(pending_thinking_);
                    system_text.swap(pending_system_);
                    attachments.swap(pending_attachments_);
                    if (tool_names_dirty_)
                    {
                        tool_names = current_tool_names_;
                        tool_names_dirty_ = false;
                        send_tools = true;
                    }
                }

                std::string chunk;
                if (!llm.empty()) chunk += format_sse_event("llm", llm);
                if (!thinking.empty()) chunk += format_sse_event("thinking", thinking);
                if (!system_text.empty()) chunk += format_sse_event("system", system_text);
                if (send_tools) chunk += format_sse_event("tools", tool_names);
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

void WEB_SERVER_CLASS::push_output(const std::string& llm_text, const std::string& thinking_text,
                                    const std::string& system_text,
                                    const std::vector<TOOL_ATTACHMENT>& attachments)
{
    std::lock_guard<std::mutex> lock(output_mutex_);
    pending_llm_ += llm_text;
    pending_thinking_ += thinking_text;
    pending_system_ += system_text;
    pending_attachments_.insert(pending_attachments_.end(), attachments.begin(), attachments.end());
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

bool WEB_SERVER_CLASS::has_client() const
{
    return client_count_.load() > 0;
}
