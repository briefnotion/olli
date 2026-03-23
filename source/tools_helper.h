#ifndef tools_helper_h
#define tools_helper_h

#include <string>
#include <chrono>
#include <vector>
#include <fstream>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

class TIMER_SIMPLE 
{
    public:
        explicit TIMER_SIMPLE(double seconds = 0.0, std::string reminder = "") 
            : m_duration(seconds), m_reminder(reminder), m_running(false) {}
        void start();
        bool isFinished() const;
        double getRemainingTime() const;
        std::string getReminder() const;

    private:
        std::chrono::duration<double> m_duration;
        std::chrono::steady_clock::time_point m_startTime;
        std::string m_reminder; 
        bool m_running;
};

struct TASK_SIMPLE
{
    public:
        std::string TASK_PHRASE = "";
        std::string TASK_PURPOSE = "";
        std::string TASK_DIRECTORY = "";
        std::vector<std::string> COMMANDS;
        void clear();
};

class TASK_SIMPLE_MANAGER
{
    public:
        std::vector<TASK_SIMPLE> TASK_LIST;
        TASK_SIMPLE_MANAGER();
        void load_all_task();
};

/**
 * HUE_SCENE
 * Stores a snapshot of light states under a specific name.
 */
struct HUE_SCENE {
    std::string name;
    std::map<std::string, json> light_states; // ID -> State snapshot
};

// Logic for serializing HUE_SCENE to/from JSON
// Marked as inline to prevent "multiple definition" linker errors
inline void to_json(json& j, const HUE_SCENE& s);
inline void from_json(const json& j, HUE_SCENE& s);

struct LightState {
    std::string id;
    std::string name;
    bool on;
    int brightness;
    std::vector<double> xy;
    bool reachable;
};

/**
 * HUE_LIGHT_CLASS
 * Handles the direct communication, state tracking, and name mapping for the Bridge.
 */
class HUE_LIGHT_CLASS {
public:

private:
    std::string bridge_ip;
    std::string api_key;
    std::string scene_filepath;
    std::map<std::string, LightState> lights_cache; // Map of ID -> State
    std::map<std::string, std::string> name_to_id;  // Map of Lowercase Name -> ID
    std::map<std::string, HUE_SCENE> local_scenes;  // Map of Lowercase Scene Name -> Scene

    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    static_cast<std::string*>(userp)->append(static_cast<const char*>(contents), size * nmemb);
    return size * nmemb;
    }

    std::string to_lower(std::string s);
    
    void save_scenes_to_disk();
    void load_scenes_from_disk();

public:
    HUE_LIGHT_CLASS(const std::string& ip = "", const std::string& key = "", const std::string& path = "scenes.json") 
        : bridge_ip(ip), api_key(key), scene_filepath(path) {
        load_scenes_from_disk();
    }

    void set_credentials(const std::string& ip, const std::string& key, const std::string& path);
    std::string make_request(const std::string& method, const std::string& endpoint, const std::string& body);
    bool refresh_lights();
    std::string resolve_id(const std::string& input);
    std::string set_light(const std::string& id_or_name, const json& state_body);

    // --- Scene Logic ---
    std::string save_scene(const std::string& name);
    std::string load_scene(const std::string& name);
    std::string remove_scene(const std::string& name);
    const std::map<std::string, HUE_SCENE>& get_scenes() const ;
    const std::map<std::string, LightState>& get_cached_lights() const ;
    std::pair<double, double> rgbToXY(int r, int g, int b);
};




#endif