#ifndef RKMEDIA_GATEWAY_SIMPLE_CONFIG_H
#define RKMEDIA_GATEWAY_SIMPLE_CONFIG_H

#include <cstdlib>
#include <fstream>
#include <map>
#include <string>

namespace simple_config {

inline std::string trim_copy(const std::string &s) {
    size_t begin = 0;
    size_t end = s.size();
    while (begin < end && (s[begin] == ' ' || s[begin] == '\t' || s[begin] == '\r' || s[begin] == '\n')) {
        ++begin;
    }
    while (end > begin && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r' || s[end - 1] == '\n')) {
        --end;
    }
    return s.substr(begin, end - begin);
}

class Reader {
public:
    Reader() : loaded_(false) {}

    bool load(const std::string &path) {
        std::ifstream file(path.c_str());
        if (!file.is_open()) {
            loaded_ = false;
            source_path_ = path;
            values_.clear();
            return false;
        }

        std::string line;
        values_.clear();
        while (std::getline(file, line)) {
            std::string trimmed = trim_copy(line);
            if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';') {
                continue;
            }

            std::string::size_type eq_pos = trimmed.find('=');
            if (eq_pos == std::string::npos) {
                continue;
            }

            std::string key = trim_copy(trimmed.substr(0, eq_pos));
            std::string value = trim_copy(trimmed.substr(eq_pos + 1));
            if (key.empty()) {
                continue;
            }

            if (value.size() >= 2) {
                char first = value[0];
                char last = value[value.size() - 1];
                if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
                    value = value.substr(1, value.size() - 2);
                }
            }
            values_[key] = value;
        }

        source_path_ = path;
        loaded_ = true;
        return true;
    }

    bool loaded() const {
        return loaded_;
    }

    const std::string &source_path() const {
        return source_path_;
    }

    std::string get_string(const char *key, const char *fallback) const {
        std::map<std::string, std::string>::const_iterator it = values_.find(key);
        if (it == values_.end() || it->second.empty()) {
            return fallback ? std::string(fallback) : std::string();
        }
        return it->second;
    }

    int get_int(const char *key, int fallback) const {
        std::map<std::string, std::string>::const_iterator it = values_.find(key);
        long parsed = 0;
        char *end_ptr = NULL;

        if (it == values_.end() || it->second.empty()) {
            return fallback;
        }

        parsed = std::strtol(it->second.c_str(), &end_ptr, 10);
        if (!end_ptr || *end_ptr != '\0') {
            return fallback;
        }
        return (int)parsed;
    }

private:
    bool loaded_;
    std::string source_path_;
    std::map<std::string, std::string> values_;
};

} // namespace simple_config

#endif
