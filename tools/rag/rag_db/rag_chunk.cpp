#include "rag_chunk.hpp"

#include <sstream>

namespace {
    std::vector<std::string> split_paragraphs(const std::string& text)
    {
        std::vector<std::string> paragraphs;
        std::string current;
        std::istringstream stream(text);
        std::string line;

        while (std::getline(stream, line)) {
            bool blank = line.find_first_not_of(" \t\r") == std::string::npos;
            if (blank) {
                if (!current.empty()) {
                    paragraphs.push_back(current);
                    current.clear();
                }
            } else {
                if (!current.empty()) current += "\n";
                current += line;
            }
        }
        if (!current.empty()) paragraphs.push_back(current);
        return paragraphs;
    }

    int word_count(const std::string& s)
    {
        std::istringstream stream(s);
        int count = 0;
        std::string word;
        while (stream >> word) count++;
        return count;
    }

    std::string trailing_words(const std::string& s, int n)
    {
        std::istringstream stream(s);
        std::vector<std::string> words;
        std::string word;
        while (stream >> word) words.push_back(word);

        if (static_cast<int>(words.size()) <= n) return s;

        std::string result;
        for (size_t i = words.size() - static_cast<size_t>(n); i < words.size(); i++) {
            if (!result.empty()) result += " ";
            result += words[i];
        }
        return result;
    }
}

std::vector<std::string> chunk_text(const std::string& text, int chunk_words, int overlap_words)
{
    std::vector<std::string> paragraphs = split_paragraphs(text);
    std::vector<std::string> chunks;
    std::string current_chunk;

    for (const std::string& paragraph : paragraphs) {
        std::string candidate = current_chunk.empty() ? paragraph : current_chunk + "\n\n" + paragraph;

        if (!current_chunk.empty() && word_count(candidate) > chunk_words) {
            chunks.push_back(current_chunk);
            std::string overlap = trailing_words(current_chunk, overlap_words);
            current_chunk = overlap.empty() ? paragraph : overlap + "\n\n" + paragraph;
        } else {
            current_chunk = candidate;
        }
    }

    if (!current_chunk.empty()) chunks.push_back(current_chunk);
    return chunks;
}
