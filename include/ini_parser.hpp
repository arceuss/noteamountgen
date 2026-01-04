#ifndef INI_PARSER_HPP
#define INI_PARSER_HPP

#include <string>
#include <map>
#include <fstream>
#include <algorithm>

namespace NoteGen {

struct SongIniData {
    std::string name;
    std::string artist;
    std::string charter;
    std::string album;
    std::string genre;
    std::string year;
    std::string loading_phrase;
    int song_length = 0; // milliseconds
    int preview_start_time = 0;
    int delay = 0;
    
    // Difficulty ratings (-1 = not present)
    int diff_guitar = -1;
    int diff_bass = -1;
    int diff_rhythm = -1;
    int diff_drums = -1;
    int diff_keys = -1;
};

inline std::string trim(const std::string& str) {
    auto start = str.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = str.find_last_not_of(" \t\r\n");
    return str.substr(start, end - start + 1);
}

inline SongIniData parse_song_ini(const std::string& filepath) {
    SongIniData data;
    std::ifstream file(filepath);
    
    if (!file.is_open()) {
        return data;
    }
    
    std::string line;
    bool in_song_section = false;
    
    while (std::getline(file, line)) {
        line = trim(line);
        
        if (line.empty() || line[0] == ';' || line[0] == '#') {
            continue;
        }
        
        // Check for section header
        if (line[0] == '[') {
            auto end = line.find(']');
            if (end != std::string::npos) {
                std::string section = line.substr(1, end - 1);
                std::transform(section.begin(), section.end(), section.begin(), ::tolower);
                in_song_section = (section == "song");
            }
            continue;
        }
        
        if (!in_song_section) continue;
        
        // Parse key = value
        auto eq_pos = line.find('=');
        if (eq_pos == std::string::npos) continue;
        
        std::string key = trim(line.substr(0, eq_pos));
        std::string value = trim(line.substr(eq_pos + 1));
        
        // Convert key to lowercase for comparison
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        
        // Parse known keys
        if (key == "name") data.name = value;
        else if (key == "artist") data.artist = value;
        else if (key == "charter" || key == "frets") data.charter = value;
        else if (key == "album") data.album = value;
        else if (key == "genre") data.genre = value;
        else if (key == "year") data.year = value;
        else if (key == "loading_phrase") data.loading_phrase = value;
        else if (key == "song_length") data.song_length = std::stoi(value);
        else if (key == "preview_start_time") data.preview_start_time = std::stoi(value);
        else if (key == "delay") data.delay = std::stoi(value);
        else if (key == "diff_guitar") data.diff_guitar = std::stoi(value);
        else if (key == "diff_bass") data.diff_bass = std::stoi(value);
        else if (key == "diff_rhythm") data.diff_rhythm = std::stoi(value);
        else if (key == "diff_drums") data.diff_drums = std::stoi(value);
        else if (key == "diff_keys") data.diff_keys = std::stoi(value);
    }
    
    return data;
}

} // namespace NoteGen

#endif // INI_PARSER_HPP

