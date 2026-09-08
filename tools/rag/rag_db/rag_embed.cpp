#include "rag_embed.hpp"

#include <nlohmann/json.hpp>

#include <curl/curl.h>

using json = nlohmann::json;

namespace {
    size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp)
    {
        std::string* out = static_cast<std::string*>(userp);
        out->append(static_cast<char*>(contents), size * nmemb);
        return size * nmemb;
    }
}

RAG_EMBEDDER::RAG_EMBEDDER(std::string host_, int port_, std::string model_)
    : host(std::move(host_)), port(port_), model(std::move(model_))
{
}

std::vector<float> RAG_EMBEDDER::embed_document(const std::string& text)
{
    return embed("search_document: " + text);
}

std::vector<float> RAG_EMBEDDER::embed_query(const std::string& text)
{
    return embed("search_query: " + text);
}

std::vector<float> RAG_EMBEDDER::embed(const std::string& prefixed_text)
{
    error.clear();

    CURL* curl = curl_easy_init();
    if (!curl) {
        error = "curl_easy_init failed";
        return {};
    }

    std::string url = "http://" + host + ":" + std::to_string(port) + "/api/embeddings";
    std::string body = json{{"model", model}, {"prompt", prefixed_text}}.dump();
    std::string response_buffer;

    struct curl_slist* headers = curl_slist_append(nullptr, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_buffer);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        error = std::string("CURL failed: ") + curl_easy_strerror(res);
        return {};
    }

    try {
        json response = json::parse(response_buffer);
        if (response.contains("error")) {
            error = response["error"].get<std::string>();
            return {};
        }
        if (!response.contains("embedding") || !response["embedding"].is_array()) {
            error = "Response missing 'embedding' array: " + response_buffer;
            return {};
        }

        std::vector<float> result;
        result.reserve(response["embedding"].size());
        for (const auto& value : response["embedding"]) result.push_back(value.get<float>());
        return result;
    } catch (const std::exception& e) {
        error = std::string("Failed to parse response: ") + e.what();
        return {};
    }
}
