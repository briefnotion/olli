#ifndef tools_helper_cpp
#define tools_helper_cpp

#include "tools_helper.h"

void TIMER_SIMPLE::start() {
    m_startTime = std::chrono::steady_clock::now();
    m_running = true;
}

bool TIMER_SIMPLE::isFinished() const {
    if (!m_running) return false;
    auto now = std::chrono::steady_clock::now();
    return (now - m_startTime) >= m_duration;
}

double TIMER_SIMPLE::getRemainingTime() const {
    if (!m_running) return 0.0;
    auto now = std::chrono::steady_clock::now();
    auto elapsed = now - m_startTime;
    
    if (elapsed >= m_duration) return 0.0;
    
    std::chrono::duration<double> remaining = m_duration - elapsed;
    return remaining.count();
}

std::string TIMER_SIMPLE::getReminder() const 
{ 
    return m_reminder; 
}

// ----

void TASK_SIMPLE::clear()
{
    TASK_PHRASE = "";
    COMMANDS.clear();
}

// ----

TASK_SIMPLE_MANAGER::TASK_SIMPLE_MANAGER()
{
    load_all_task();
}

void TASK_SIMPLE_MANAGER::load_all_task()
{
    TASK_SIMPLE tmp_task;

    tmp_task.TASK_PHRASE = "I'm home";
    tmp_task.COMMANDS.push_back("Load the repose scene.");
    TASK_LIST.push_back(tmp_task);

    tmp_task.clear();
    tmp_task.TASK_PHRASE = "I'm leaving";
    tmp_task.COMMANDS.push_back("Load the labor scene.");
    TASK_LIST.push_back(tmp_task);

    tmp_task.clear();
    tmp_task.TASK_PHRASE = "I'm sleeping";
    tmp_task.COMMANDS.push_back("Load the slumber scene.");
    TASK_LIST.push_back(tmp_task);

    tmp_task.clear();
    tmp_task.TASK_PHRASE = "run system test";
    tmp_task.TASK_PURPOSE = "This is a series of a few simple questions to check responses.";
    tmp_task.TASK_DIRECTORY = "system_test";
    tmp_task.COMMANDS.push_back("Set a timer for 30 seconds and when the timer goes off, blink any light.");
    tmp_task.COMMANDS.push_back("turn off all the lights.");
    tmp_task.COMMANDS.push_back("show the numbers 1 through 10");
    tmp_task.COMMANDS.push_back("what time is it?");
    tmp_task.COMMANDS.push_back("what is the opposite of up?");
    tmp_task.COMMANDS.push_back("what is 2 + 2?");
    tmp_task.COMMANDS.push_back("Ask the user for the name of a dog.");
    tmp_task.COMMANDS.push_back("[[ASK]]What is a name for a dog?");
    tmp_task.COMMANDS.push_back("what was the previous word?");
    tmp_task.COMMANDS.push_back("what is the weather like in New York City right now?");
    tmp_task.COMMANDS.push_back("how many eggs are in a dozon eggs.");
    tmp_task.COMMANDS.push_back("turn all the lights back on.");
    tmp_task.COMMANDS.push_back("[[ENTER TO CONTINUE]]wait");
    tmp_task.COMMANDS.push_back("Announce the system test is complete.");
    TASK_LIST.push_back(tmp_task);

    tmp_task.clear();
    tmp_task.TASK_PHRASE = "run process resume";
    tmp_task.TASK_PURPOSE = "This is to arrange documents for a resume review.";
    tmp_task.TASK_DIRECTORY = "resume process";

    tmp_task.COMMANDS.push_back("Ask the user for the resume file.");
    tmp_task.COMMANDS.push_back("[[ASK]]");
    
    tmp_task.COMMANDS.push_back("Ask the user for the job description.");
    tmp_task.COMMANDS.push_back("[[ASK]]");

    tmp_task.COMMANDS.push_back("Generate a concise 1-paragraph evaluation explaining how well the applicant’s resume matches the job description. Highlight the strongest points of alignment, reference specific skills or achievements, and keep the tone professional and neutral.");
    tmp_task.COMMANDS.push_back("[[ENTER TO CONTINUE]]");
    
    tmp_task.COMMANDS.push_back("Using the applicant’s resume and the job description, generate a short professional cover letter (3–4 paragraphs) explaining why the applicant is a strong fit for the role. Keep the tone confident but not overly formal, and reference specific experience from the resume.");
    tmp_task.COMMANDS.push_back("[[ENTER TO CONTINUE]]");
    TASK_LIST.push_back(tmp_task);
}

// ----

// Logic for serializing HUE_SCENE to/from JSON
// Marked as inline to prevent "multiple definition" linker errors
inline void to_json(json& j, const HUE_SCENE& s) {
    j = json{{"name", s.name}, {"light_states", s.light_states}};
}
inline void from_json(const json& j, HUE_SCENE& s) {
    j.at("name").get_to(s.name);
    j.at("light_states").get_to(s.light_states);
}

// ----

std::string HUE_LIGHT_CLASS::to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { 
        return static_cast<char>(std::tolower(c)); 
    });
    return s;
}

void HUE_LIGHT_CLASS::save_scenes_to_disk() {
    if (scene_filepath.empty()) return;
    try {
        std::ofstream file(scene_filepath);
        if (file.is_open()) {
            // nlohmann::json can be piped directly to the stream with indentation
            file << json(local_scenes).dump(4);
        }
    } catch (...) {
        // Silently fail
    }
}

void HUE_LIGHT_CLASS::load_scenes_from_disk() {
    if (scene_filepath.empty()) return;
    try {
        std::ifstream file(scene_filepath);
        if (file.is_open()) {
            json j;
            file >> j;
            local_scenes = j.get<std::map<std::string, HUE_SCENE>>();
        }
    } catch (...) {
        // File might not exist yet, which is fine
    }
}

void HUE_LIGHT_CLASS::set_credentials(const std::string& ip, const std::string& key, const std::string& path) {
    bridge_ip = ip;
    api_key = key;
    if (!path.empty()) {
        scene_filepath = path;
        load_scenes_from_disk();
    }
}

std::string HUE_LIGHT_CLASS::make_request(const std::string& method, const std::string& endpoint, const std::string& body = "") {
    CURL* curl = curl_easy_init();
    std::string readBuffer;
    if (curl) {
        std::string url = "http://" + bridge_ip + "/api/" + api_key + endpoint;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L); 

        if (!body.empty()) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        }

        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);

        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            readBuffer = "{\"error\": \"CURL failed: " + std::string(curl_easy_strerror(res)) + "\"}";
        }
        curl_easy_cleanup(curl);
    }
    return readBuffer;
}

bool HUE_LIGHT_CLASS::refresh_lights() {
    std::string response = make_request("GET", "/lights");
    try {
        json data = json::parse(response);
        if (data.is_array() && !data.empty() && data[0].contains("error")) return false;

        lights_cache.clear();
        name_to_id.clear();

        for (auto& [id, info] : data.items()) {
            std::string name = info.value("name", "Unknown");
            LightState state;
            state.id = id;
            state.name = name;
            
            if (info.contains("state")) {
                state.on = info["state"].value("on", false);
                state.brightness = info["state"].value("bri", 0);
                state.reachable = info["state"].value("reachable", false);
                if (info["state"].contains("xy")) {
                    state.xy = info["state"]["xy"].get<std::vector<double>>();
                }
            }

            lights_cache[id] = state;
            name_to_id[to_lower(name)] = id;
        }
        return true;
    } catch (...) {
        return false;
    }
}

std::string HUE_LIGHT_CLASS::resolve_id(const std::string& input) {
    if (input.empty()) return "";
    // Special case for "all" which is supported by Hue groups API usually, 
    // but here we can handle it by iterating or using group 0
    if (to_lower(input) == "all") return "all"; 
    if (std::all_of(input.begin(), input.end(), ::isdigit)) return input;

    std::string lower_input = to_lower(input);
    if (name_to_id.count(lower_input)) {
        return name_to_id[lower_input];
    }
    return "";
}

std::string HUE_LIGHT_CLASS::set_light(const std::string& id_or_name, const json& state_body) {
    std::string id = resolve_id(id_or_name);
    if (id == "all") {
        // Use Group 0 to control all lights on the bridge simultaneously
        return make_request("PUT", "/groups/0/action", state_body.dump());
    }
    if (id.empty()) return "{\"error\": \"Light '" + id_or_name + "' not found\"}";
    return make_request("PUT", "/lights/" + id + "/state", state_body.dump());
}

// --- Scene Logic ---

std::string HUE_LIGHT_CLASS::save_scene(const std::string& name) {
    std::string lower_name = to_lower(name);
    
    // Check if the scene already exists in the local map
    if (local_scenes.find(lower_name) != local_scenes.end()) {
        return "Scene '" + name + "' already exists. Please remove it first or use a different name.";
    }

    if (!refresh_lights()) return "Failed to refresh lights for scene capture.";
    
    HUE_SCENE new_scene;
    new_scene.name = name;

    for (auto const& [id, state] : lights_cache) {
        json state_json;
        state_json["on"] = state.on;
        state_json["bri"] = state.brightness;
        if (!state.xy.empty()) state_json["xy"] = state.xy;
        new_scene.light_states[id] = state_json;
    }

    local_scenes[lower_name] = new_scene;
    save_scenes_to_disk();
    return "Scene '" + name + "' saved to disk.";
}

std::string HUE_LIGHT_CLASS::load_scene(const std::string& name) {
    std::string lower_name = to_lower(name);
    if (local_scenes.find(lower_name) == local_scenes.end()) {
        return "Scene '" + name + "' not found.";
    }

    HUE_SCENE& scene = local_scenes[lower_name];
    
    for (auto const& [id, state_json] : scene.light_states) {
        make_request("PUT", "/lights/" + id + "/state", state_json.dump());
    }

    return "Scene '" + name + "' activated.";
}

std::string HUE_LIGHT_CLASS::remove_scene(const std::string& name) {
    std::string lower_name = to_lower(name);
    if (local_scenes.erase(lower_name)) {
        save_scenes_to_disk();
        return "Scene '" + name + "' removed.";
    }
    return "Scene '" + name + "' not found.";
}

const std::map<std::string, HUE_SCENE>& HUE_LIGHT_CLASS::get_scenes() const {
    return local_scenes;
}

const std::map<std::string, LightState>& HUE_LIGHT_CLASS::get_cached_lights() const {
    return lights_cache;
}

std::pair<double, double> HUE_LIGHT_CLASS::rgbToXY(int r, int g, int b) {
    auto adjust = [](double val) {
        val /= 255.0;
        return (val > 0.04045) ? std::pow((val + 0.055) / (1.055), 2.4) : (val / 12.92);
    };
    double R = adjust(static_cast<double>(r)), G = adjust(static_cast<double>(g)), B = adjust(static_cast<double>(b));
    double X = R * 0.664511 + G * 0.154324 + B * 0.162028;
    double Y = R * 0.283881 + G * 0.668433 + B * 0.047685;
    double Z = R * 0.000088 + G * 0.072310 + B * 0.986039;
    double sum = X + Y + Z;
    double cx = (sum == 0) ? 0.0 : X / sum;
    double cy = (sum == 0) ? 0.0 : Y / sum;
    return {cx, cy};
}


// ----

#endif