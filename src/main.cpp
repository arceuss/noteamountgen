#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>

#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <memory>
#include <set>
#include <functional>

#include <sightread/chartparser.hpp>
#include <sightread/midiparser.hpp>
#include <nlohmann/json.hpp>

#include "chart_info.hpp"
#include "loop_generator.hpp"
#include "chart_writer.hpp"
#include "ini_parser.hpp"

using json = nlohmann::json;
namespace fs = std::filesystem;

// ffmpeg stuff

static std::string g_ffmpeg_path;

std::string find_ffmpeg() {
    // check next to exe first
    char exe_path[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, exe_path, MAX_PATH);
    
    if (len == 0) {
        return "";
    }
    
    // get exe dir
    std::string exe_dir = exe_path;
    size_t last_slash = exe_dir.find_last_of("\\/");
    if (last_slash != std::string::npos) {
        exe_dir = exe_dir.substr(0, last_slash);
    }
    
    std::string local_ffmpeg = exe_dir + "\\ffmpeg.exe";
    
    // windows api check works better with paths that have spaces
    DWORD attrib = GetFileAttributesA(local_ffmpeg.c_str());
    if (attrib != INVALID_FILE_ATTRIBUTES && !(attrib & FILE_ATTRIBUTE_DIRECTORY)) {
        return local_ffmpeg;
    }
    
    // try forward slashes too just in case
    std::string local_ffmpeg2 = exe_dir + "/ffmpeg.exe";
    attrib = GetFileAttributesA(local_ffmpeg2.c_str());
    if (attrib != INVALID_FILE_ATTRIBUTES && !(attrib & FILE_ATTRIBUTE_DIRECTORY)) {
        return local_ffmpeg2;
    }
    
    // check system path
    char buffer[MAX_PATH];
    FILE* pipe = _popen("where ffmpeg.exe 2>nul", "r");
    if (pipe) {
        if (fgets(buffer, MAX_PATH, pipe)) {
            _pclose(pipe);
            std::string path = buffer;
            // strip newline
            while (!path.empty() && (path.back() == '\n' || path.back() == '\r')) {
                path.pop_back();
            }
            attrib = GetFileAttributesA(path.c_str());
            if (attrib != INVALID_FILE_ATTRIBUTES && !(attrib & FILE_ATTRIBUTE_DIRECTORY)) {
                return path;
            }
        } else {
            _pclose(pipe);
        }
    }
    
    return "";
}

// progress callback
using ProgressCallback = std::function<void(int percent, const std::string& status)>;

bool process_audio_with_ffmpeg(const std::string& source_path, 
                                const std::string& dest_path,
                                const std::vector<NoteGen::GenerationResult::AudioSegment>& segments,
                                bool is_full_song,
                                std::string& error_out,
                                ProgressCallback progress_cb = nullptr) {
    if (g_ffmpeg_path.empty() || segments.empty()) {
        return false;
    }
    
    // build the filter
    std::vector<std::string> filter_parts;
    int stream_index = 0;
    double total_duration = 0.0;
    
    for (const auto& seg : segments) {
        char start_str[32], dur_str[32];
        snprintf(start_str, sizeof(start_str), "%.3f", seg.start_seconds);
        snprintf(dur_str, sizeof(dur_str), "%.3f", seg.duration_seconds);
        
        for (int i = 0; i < seg.repeat_count; i++) {
            std::string part = "[0:a]atrim=start=" + std::string(start_str) + 
                              ":duration=" + std::string(dur_str) + 
                              ",asetpts=PTS-STARTPTS[s" + std::to_string(stream_index) + "]";
            filter_parts.push_back(part);
            stream_index++;
            total_duration += seg.duration_seconds;
        }
    }
    
    if (stream_index == 0) {
        return false;
    }
    
    // concat inputs
    std::string concat_inputs;
    for (int i = 0; i < stream_index; i++) {
        concat_inputs += "[s" + std::to_string(i) + "]";
    }
    
    // build filter complex
    std::string filter_complex;
    for (size_t i = 0; i < filter_parts.size(); i++) {
        if (i > 0) filter_complex += ";";
        filter_complex += filter_parts[i];
    }
    
    filter_complex += ";" + concat_inputs + "concat=n=" + std::to_string(stream_index) + ":v=0:a=1";
    
    if (is_full_song) {
        double fade_duration = 1.0;
        double fade_start = std::max(0.0, total_duration - fade_duration);
        char fade_start_str[32];
        snprintf(fade_start_str, sizeof(fade_start_str), "%.3f", fade_start);
        filter_complex += "[concat];[concat]afade=t=out:st=" + std::string(fade_start_str) + ":d=1.0[out]";
    } else {
        filter_complex += "[out]";
    }
    
    // write filter to temp file
    std::string filter_path = fs::path(dest_path).parent_path().string() + "\\ffmpeg_filter.txt";
    {
        std::ofstream filter_file(filter_path);
        filter_file << filter_complex;
    }
    
    // pipe for ffmpeg progress
    HANDLE hReadPipe, hWritePipe;
    SECURITY_ATTRIBUTES sa = {sizeof(SECURITY_ATTRIBUTES), NULL, TRUE};
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
        error_out = "Failed to create pipe";
        DeleteFileA(filter_path.c_str());
        return false;
    }
    
    // dont inherit read handle
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);
    
    // ffmpeg command with progress output
    std::string cmdline = "\"" + g_ffmpeg_path + "\" -y -progress pipe:1 -i \"" + source_path + 
                         "\" -filter_complex_script \"" + filter_path + 
                         "\" -map \"[out]\" \"" + dest_path + "\"";
    
    STARTUPINFOA si = {0};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    si.hStdInput = NULL;
    
    PROCESS_INFORMATION pi = {0};
    
    std::vector<char> cmdline_buf(cmdline.begin(), cmdline.end());
    cmdline_buf.push_back('\0');
    
    if (!CreateProcessA(
            g_ffmpeg_path.c_str(),
            cmdline_buf.data(),
            NULL, NULL, TRUE,  // Inherit handles = TRUE
            CREATE_NO_WINDOW,
            NULL, NULL, &si, &pi)) {
        error_out = "Failed to start FFmpeg (error " + std::to_string(GetLastError()) + ")";
        CloseHandle(hReadPipe);
        CloseHandle(hWritePipe);
        DeleteFileA(filter_path.c_str());
        return false;
    }
    
    // close write end in parent
    CloseHandle(hWritePipe);
    
    // read progress output
    char buffer[256];
    DWORD bytesRead;
    std::string accumulated;
    double current_time = 0.0;
    
    while (true) {
        // check if still running
        DWORD wait_result = WaitForSingleObject(pi.hProcess, 0);
        
        // try to read
        DWORD available = 0;
        PeekNamedPipe(hReadPipe, NULL, 0, NULL, &available, NULL);
        
        if (available > 0) {
            if (ReadFile(hReadPipe, buffer, std::min(available, (DWORD)sizeof(buffer)-1), &bytesRead, NULL) && bytesRead > 0) {
                buffer[bytesRead] = '\0';
                accumulated += buffer;
                
                // parse out_time_ms
                size_t pos;
                while ((pos = accumulated.find("out_time_ms=")) != std::string::npos) {
                    size_t end = accumulated.find('\n', pos);
                    if (end != std::string::npos) {
                        std::string time_str = accumulated.substr(pos + 12, end - pos - 12);
                        try {
                            double time_ms = std::stod(time_str);
                            current_time = time_ms / 1000000.0;  // microseconds to seconds
                            
                            if (progress_cb && total_duration > 0) {
                                int percent = std::min(100, (int)(current_time / total_duration * 100));
                                char status[64];
                                snprintf(status, sizeof(status), "%.1f / %.1f sec", current_time, total_duration);
                                progress_cb(percent, status);
                            }
                        } catch (...) {}
                        accumulated = accumulated.substr(end + 1);
                    } else {
                        break;
                    }
                }
                
                // keep only recent data
                if (accumulated.size() > 1024) {
                    accumulated = accumulated.substr(accumulated.size() - 512);
                }
            }
        }
        
        if (wait_result == WAIT_OBJECT_0) {
            // done, drain remaining output
            while (ReadFile(hReadPipe, buffer, sizeof(buffer)-1, &bytesRead, NULL) && bytesRead > 0) {
            }
            break;
        }
        
        Sleep(50);  // dont spin too hard
    }
    
    CloseHandle(hReadPipe);
    
    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    
    DeleteFileA(filter_path.c_str());
    
    if (progress_cb) {
        progress_cb(100, "Complete");
    }
    
    if (exit_code != 0) {
        error_out = "FFmpeg exited with code " + std::to_string(exit_code);
        return false;
    }
    
    return true;
}

#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// control ids

#define ID_BROWSE_BTN       101
#define ID_TARGET_EDIT      102
#define ID_PRESET_3999      103
#define ID_PRESET_5000      104
#define ID_PRESET_10000     105
#define ID_INSTRUMENT_COMBO 106
#define ID_DIFFICULTY_COMBO 107
#define ID_SECTIONS_LIST    108
#define ID_SELECT_ALL       109
#define ID_SELECT_NONE      110
#define ID_GENERATE_BTN     111
#define ID_OPEN_FOLDER_BTN  112
#define ID_NEW_BTN          113

// app state

struct AppState {
    // loaded chart data
    std::string chart_path;
    std::string chart_dir;
    std::unique_ptr<SightRead::Song> song;
    NoteGen::SongIniData ini_data;
    
    // chart info
    std::string song_name;
    std::string artist;
    int total_notes = 0;
    
    struct SectionInfo {
        std::string name;
        std::string display_name;
        int note_count;
    };
    std::vector<SectionInfo> sections;
    
    // tracks
    std::vector<std::string> instruments;
    std::vector<std::string> difficulties = {"Expert", "Hard", "Medium", "Easy"};
    
    // result
    std::string output_folder;
    int generated_notes = 0;
    double generated_duration = 0.0;
    int generated_sections = 0;
};

static AppState g_state;
static HWND g_hwnd = NULL;
static HWND g_status_label = NULL;
static HWND g_song_label = NULL;
static HWND g_notes_label = NULL;
static HWND g_target_edit = NULL;
static HWND g_instrument_combo = NULL;
static HWND g_difficulty_combo = NULL;
static HWND g_sections_list = NULL;
static HWND g_generate_btn = NULL;
static HWND g_selected_label = NULL;
static HWND g_result_group = NULL;
static HWND g_result_label = NULL;
static HWND g_open_folder_btn = NULL;

// helpers

std::string read_file_content(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Cannot open file: " + path);
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();
    
    // strip utf8 bom if there
    const std::string utf8_bom = "\xEF\xBB\xBF";
    if (content.size() >= 3 && content.substr(0, 3) == utf8_bom) {
        content = content.substr(3);
    }
    
    return content;
}

SightRead::Instrument string_to_instrument(const std::string& str) {
    if (str == "Guitar") return SightRead::Instrument::Guitar;
    if (str == "Bass") return SightRead::Instrument::Bass;
    if (str == "Rhythm") return SightRead::Instrument::Rhythm;
    if (str == "Keys") return SightRead::Instrument::Keys;
    if (str == "Drums") return SightRead::Instrument::Drums;
    if (str == "GHLGuitar") return SightRead::Instrument::GHLGuitar;
    if (str == "GHLBass") return SightRead::Instrument::GHLBass;
    return SightRead::Instrument::Guitar;
}

SightRead::Difficulty string_to_difficulty(const std::string& str) {
    if (str == "Easy") return SightRead::Difficulty::Easy;
    if (str == "Medium") return SightRead::Difficulty::Medium;
    if (str == "Hard") return SightRead::Difficulty::Hard;
    if (str == "Expert") return SightRead::Difficulty::Expert;
    return SightRead::Difficulty::Expert;
}

std::string get_selected_instrument() {
    int idx = (int)SendMessage(g_instrument_combo, CB_GETCURSEL, 0, 0);
    if (idx >= 0 && idx < (int)g_state.instruments.size()) {
        return g_state.instruments[idx];
    }
    return "Guitar";
}

std::string get_selected_difficulty() {
    int idx = (int)SendMessage(g_difficulty_combo, CB_GETCURSEL, 0, 0);
    if (idx >= 0 && idx < (int)g_state.difficulties.size()) {
        return g_state.difficulties[idx];
    }
    return "Expert";
}

void update_selected_notes() {
    int count = (int)SendMessage(g_sections_list, LB_GETCOUNT, 0, 0);
    int total = 0;
    int selected = 0;
    
    for (int i = 0; i < count && i < (int)g_state.sections.size(); i++) {
        if (SendMessage(g_sections_list, LB_GETSEL, i, 0) > 0) {
            total += g_state.sections[i].note_count;
            selected++;
        }
    }
    
    std::string text = "Selected: " + std::to_string(total) + " notes (" + 
                       std::to_string(selected) + "/" + std::to_string(g_state.sections.size()) + " sections)";
    SetWindowTextA(g_selected_label, text.c_str());
}

void update_track_info() {
    if (!g_state.song) return;
    
    auto instrument = string_to_instrument(get_selected_instrument());
    auto difficulty = string_to_difficulty(get_selected_difficulty());
    
    json info_json = NoteGen::song_to_json(*g_state.song, g_state.ini_data, instrument, difficulty);
    g_state.total_notes = info_json["total_notes"];
    
    std::string notes_text = "Total Notes: " + std::to_string(g_state.total_notes);
    SetWindowTextA(g_notes_label, notes_text.c_str());
    
    // Update section note counts
    SendMessage(g_sections_list, LB_RESETCONTENT, 0, 0);
    g_state.sections.clear();
    
    for (const auto& sec : info_json["sections"]) {
        AppState::SectionInfo section;
        section.name = sec["name"];
        section.display_name = section.name;
        std::replace(section.display_name.begin(), section.display_name.end(), '_', ' ');
        section.note_count = sec["note_count"];
        g_state.sections.push_back(section);
        
        std::string item = section.display_name + " (" + std::to_string(section.note_count) + " notes)";
        SendMessageA(g_sections_list, LB_ADDSTRING, 0, (LPARAM)item.c_str());
    }
    
    // select all by default
    SendMessage(g_sections_list, LB_SELITEMRANGE, TRUE, MAKELPARAM(0, g_state.sections.size() - 1));
    update_selected_notes();
}

bool load_chart(const std::string& path) {
    try {
        std::string content = read_file_content(path);
        std::string extension = fs::path(path).extension().string();
        
        SightRead::Metadata metadata;
        
        if (extension == ".chart") {
            SightRead::ChartParser parser(metadata);
            g_state.song = std::make_unique<SightRead::Song>(parser.parse(content));
        } else if (extension == ".mid" || extension == ".midi") {
            SightRead::MidiParser parser(metadata);
            g_state.song = std::make_unique<SightRead::Song>(
                parser.parse(std::vector<std::uint8_t>(content.begin(), content.end()))
            );
        } else {
            MessageBoxA(g_hwnd, ("Unknown file format: " + extension).c_str(), "Error", MB_OK | MB_ICONERROR);
            return false;
        }
        
        g_state.chart_path = path;
        g_state.chart_dir = fs::path(path).parent_path().string();
        
        // try to load song.ini
        std::string ini_path = g_state.chart_dir + "\\song.ini";
        if (fs::exists(ini_path)) {
            g_state.ini_data = NoteGen::parse_song_ini(ini_path);
        } else {
            g_state.ini_data = NoteGen::SongIniData{};
        }
        
        // song info
        g_state.song_name = !g_state.ini_data.name.empty() ? g_state.ini_data.name 
                          : g_state.song->global_data().name();
        g_state.artist = !g_state.ini_data.artist.empty() ? g_state.ini_data.artist 
                       : g_state.song->global_data().artist();
        
        // update ui
        std::string song_text = g_state.song_name + " by " + g_state.artist;
        SetWindowTextA(g_song_label, song_text.c_str());
        
        // get instruments
        json tracks_json = NoteGen::get_available_tracks(*g_state.song);
        g_state.instruments.clear();
        std::set<std::string> inst_set;
        for (const auto& track : tracks_json) {
            std::string inst = track["instrument"];
            if (inst_set.find(inst) == inst_set.end()) {
                inst_set.insert(inst);
                g_state.instruments.push_back(inst);
            }
        }
        
        if (g_state.instruments.empty()) {
            MessageBoxA(g_hwnd, "No playable tracks found in chart", "Error", MB_OK | MB_ICONERROR);
            return false;
        }
        
        // fill instrument combo
        SendMessage(g_instrument_combo, CB_RESETCONTENT, 0, 0);
        for (const auto& inst : g_state.instruments) {
            SendMessageA(g_instrument_combo, CB_ADDSTRING, 0, (LPARAM)inst.c_str());
        }
        SendMessage(g_instrument_combo, CB_SETCURSEL, 0, 0);
        
        // reset difficulty combo
        SendMessage(g_difficulty_combo, CB_SETCURSEL, 0, 0);
        
        // update track info
        update_track_info();
        
        // enable stuff
        EnableWindow(g_generate_btn, TRUE);
        
        SetWindowTextA(g_status_label, "Chart loaded successfully!");
        
        return true;
        
    } catch (const std::exception& e) {
        MessageBoxA(g_hwnd, e.what(), "Error Loading Chart", MB_OK | MB_ICONERROR);
        return false;
    }
}

bool generate_chart() {
    // get output folder
    char folder_path[MAX_PATH] = {0};
    BROWSEINFOA bi = {0};
    bi.hwndOwner = g_hwnd;
    bi.pszDisplayName = folder_path;
    bi.lpszTitle = "Select Output Folder";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    
    LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
    if (!pidl) return false;
    
    SHGetPathFromIDListA(pidl, folder_path);
    CoTaskMemFree(pidl);
    
    std::string output_dir = folder_path;
    
    try {
        auto instrument = string_to_instrument(get_selected_instrument());
        auto difficulty = string_to_difficulty(get_selected_difficulty());
        
        // target notes
        char target_str[32];
        GetWindowTextA(g_target_edit, target_str, 32);
        int target_notes = std::atoi(target_str);
        if (target_notes < 100) target_notes = 100;
        if (target_notes > 99999) target_notes = 99999;
        
        NoteGen::GenerationConfig config;
        config.target_note_count = target_notes;
        
        // get selected sections
        int count = (int)SendMessage(g_sections_list, LB_GETCOUNT, 0, 0);
        for (int i = 0; i < count && i < (int)g_state.sections.size(); i++) {
            if (SendMessage(g_sections_list, LB_GETSEL, i, 0) > 0) {
                config.selected_sections.push_back(g_state.sections[i].name);
            }
        }
        
        if (config.selected_sections.empty()) {
            MessageBoxA(g_hwnd, "Please select at least one section", "Error", MB_OK | MB_ICONWARNING);
            return false;
        }
        
        SetWindowTextA(g_status_label, "Generating chart...");
        UpdateWindow(g_hwnd);
        
        // generate
        NoteGen::LoopGenerator generator(*g_state.song, instrument, difficulty, g_state.ini_data);
        auto result = generator.generate(config);
        
        if (!result.success) {
            MessageBoxA(g_hwnd, result.error_message.c_str(), "Generation Error", MB_OK | MB_ICONERROR);
            return false;
        }
        
        // make output dir
        std::string final_output = output_dir + "\\" + result.folder_name;
        fs::create_directories(final_output);
        
        // write chart (with utf8 bom)
        std::string chart_path = final_output + "\\notes.chart";
        std::ofstream chart_file(chart_path, std::ios::binary);
        const unsigned char bom[] = {0xEF, 0xBB, 0xBF};
        chart_file.write(reinterpret_cast<const char*>(bom), 3);
        chart_file << result.chart_data;
        chart_file.close();
        
        // write song.ini
        std::string ini_path = final_output + "\\song.ini";
        std::ofstream ini_file(ini_path);
        ini_file << "[Song]\n";
        ini_file << "name = " << result.chart_name << "\n";
        ini_file << "artist = " << (g_state.ini_data.artist.empty() ? g_state.song->global_data().artist() : g_state.ini_data.artist) << "\n";
        ini_file << "charter = " << (g_state.ini_data.charter.empty() ? g_state.song->global_data().charter() : g_state.ini_data.charter) << "\n";
        ini_file << "album = " << g_state.ini_data.album << "\n";
        ini_file << "genre = Practice\n";
        ini_file << "year = " << g_state.ini_data.year << "\n";
        ini_file.close();
        
        // copy/process audio and images
        std::vector<std::string> audio_exts = {".ogg", ".opus", ".mp3", ".wav"};
        std::vector<std::string> image_exts = {".png", ".jpg", ".jpeg"};
        
        bool has_ffmpeg = !g_ffmpeg_path.empty();
        int audio_processed = 0;
        int audio_copied = 0;
        
        for (const auto& entry : fs::directory_iterator(g_state.chart_dir)) {
            std::string ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            
            bool is_audio = std::find(audio_exts.begin(), audio_exts.end(), ext) != audio_exts.end();
            bool is_image = std::find(image_exts.begin(), image_exts.end(), ext) != image_exts.end();
            
            if (is_image) {
                std::string dest = final_output + "\\" + entry.path().filename().string();
                try {
                    fs::copy_file(entry.path(), dest, fs::copy_options::overwrite_existing);
                } catch (...) {}
            }
            else if (is_audio) {
                std::string dest = final_output + "\\" + entry.path().filename().string();
                
                if (has_ffmpeg && !result.audio_segments.empty()) {
                    // process with ffmpeg
                    std::string filename = entry.path().filename().string();
                    
                    auto progress_callback = [&filename](int percent, const std::string& status) {
                        // ascii progress bar
                        int filled = percent / 5;  // 20 chars total
                        std::string bar = "[";
                        for (int i = 0; i < 20; i++) {
                            bar += (i < filled) ? '#' : ' ';
                        }
                        bar += "] " + std::to_string(percent) + "% - " + status;
                        
                        std::string msg = "Processing " + filename + "  " + bar;
                        SetWindowTextA(g_status_label, msg.c_str());
                        UpdateWindow(g_hwnd);
                        
                        // pump messages so ui doesnt freeze
                        MSG msg_struct;
                        while (PeekMessage(&msg_struct, NULL, 0, 0, PM_REMOVE)) {
                            TranslateMessage(&msg_struct);
                            DispatchMessage(&msg_struct);
                        }
                    };
                    
                    progress_callback(0, "Starting...");
                    
                    std::string ffmpeg_error;
                    if (process_audio_with_ffmpeg(entry.path().string(), dest, 
                                                   result.audio_segments, result.is_full_song,
                                                   ffmpeg_error, progress_callback)) {
                        audio_processed++;
                    } else {
                        // ffmpeg failed, just copy
                        try {
                            fs::copy_file(entry.path(), dest, fs::copy_options::overwrite_existing);
                            audio_copied++;
                        } catch (...) {}
                    }
                } else {
                    // no ffmpeg, just copy
                    try {
                        fs::copy_file(entry.path(), dest, fs::copy_options::overwrite_existing);
                        audio_copied++;
                    } catch (...) {}
                }
            }
        }
        
        // store results
        g_state.output_folder = final_output;
        g_state.generated_notes = result.total_notes;
        g_state.generated_duration = result.total_duration_seconds;
        g_state.generated_sections = (int)result.looped_sections.size();
        
        // update result display
        int mins = (int)(result.total_duration_seconds / 60);
        int secs = (int)result.total_duration_seconds % 60;
        
        std::string audio_status;
        if (audio_processed > 0) {
            audio_status = " (audio looped)";
        } else if (audio_copied > 0) {
            audio_status = " (audio copied, no FFmpeg)";
        }
        
        std::string result_text = "Generated " + std::to_string(result.total_notes) + " notes" + audio_status + "\n" +
                                  "Duration: " + std::to_string(mins) + ":" + 
                                  (secs < 10 ? "0" : "") + std::to_string(secs) + "\n" +
                                  "Saved to: " + final_output;
        
        SetWindowTextA(g_result_label, result_text.c_str());
        ShowWindow(g_result_group, SW_SHOW);
        ShowWindow(g_result_label, SW_SHOW);
        ShowWindow(g_open_folder_btn, SW_SHOW);
        
        SetWindowTextA(g_status_label, "Chart generated successfully!");
        
        return true;
        
    } catch (const std::exception& e) {
        MessageBoxA(g_hwnd, e.what(), "Error", MB_OK | MB_ICONERROR);
        SetWindowTextA(g_status_label, "Generation failed.");
        return false;
    }
}

void browse_for_chart() {
    char filename[MAX_PATH] = {0};
    OPENFILENAMEA ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = "Chart Files (*.chart;*.mid)\0*.chart;*.mid;*.midi\0All Files\0*.*\0";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = "Select Chart File";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    
    if (GetOpenFileNameA(&ofn)) {
        load_chart(filename);
    }
}

// window proc

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            // accept drag drop
            DragAcceptFiles(hwnd, TRUE);
            break;
            
        case WM_DROPFILES: {
            HDROP hDrop = (HDROP)wParam;
            char path[MAX_PATH];
            
            if (DragQueryFileA(hDrop, 0, path, MAX_PATH)) {
                std::string file_path = path;
                
                // check if folder
                if (fs::is_directory(file_path)) {
                    // find chart in folder
                    for (const auto& entry : fs::directory_iterator(file_path)) {
                        std::string ext = entry.path().extension().string();
                        if (ext == ".chart" || ext == ".mid" || ext == ".midi") {
                            load_chart(entry.path().string());
                            break;
                        }
                    }
                } else {
                    std::string ext = fs::path(file_path).extension().string();
                    if (ext == ".chart" || ext == ".mid" || ext == ".midi") {
                        load_chart(file_path);
                    }
                }
            }
            
            DragFinish(hDrop);
            break;
        }
        
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            int notify = HIWORD(wParam);
            
            switch (id) {
                case ID_BROWSE_BTN:
                    browse_for_chart();
                    break;
                    
                case ID_PRESET_3999:
                    SetWindowTextA(g_target_edit, "3999");
                    break;
                    
                case ID_PRESET_5000:
                    SetWindowTextA(g_target_edit, "5000");
                    break;
                    
                case ID_PRESET_10000:
                    SetWindowTextA(g_target_edit, "10000");
                    break;
                    
                case ID_INSTRUMENT_COMBO:
                case ID_DIFFICULTY_COMBO:
                    if (notify == CBN_SELCHANGE) {
                        update_track_info();
                    }
                    break;
                    
                case ID_SECTIONS_LIST:
                    if (notify == LBN_SELCHANGE) {
                        update_selected_notes();
                    }
                    break;
                    
                case ID_SELECT_ALL:
                    SendMessage(g_sections_list, LB_SELITEMRANGE, TRUE, 
                               MAKELPARAM(0, g_state.sections.size() - 1));
                    update_selected_notes();
                    break;
                    
                case ID_SELECT_NONE:
                    SendMessage(g_sections_list, LB_SELITEMRANGE, FALSE, 
                               MAKELPARAM(0, g_state.sections.size() - 1));
                    update_selected_notes();
                    break;
                    
                case ID_GENERATE_BTN:
                    generate_chart();
                    break;
                    
                case ID_OPEN_FOLDER_BTN:
                    if (!g_state.output_folder.empty()) {
                        ShellExecuteA(NULL, "explore", g_state.output_folder.c_str(), NULL, NULL, SW_SHOWDEFAULT);
                    }
                    break;
            }
            break;
        }
        
        case WM_DESTROY:
            PostQuitMessage(0);
            break;
            
        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

// main

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    // init common controls
    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(icex);
    icex.dwICC = ICC_WIN95_CLASSES;
    InitCommonControlsEx(&icex);
    
    // check for ffmpeg
    g_ffmpeg_path = find_ffmpeg();
    
    // register window class
    WNDCLASSEXA wc = {0};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "NoteAmountGenGUI";
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    
    if (!RegisterClassExA(&wc)) {
        MessageBoxA(NULL, "Failed to register window class", "Error", MB_OK | MB_ICONERROR);
        return 1;
    }
    
    // create main window
    g_hwnd = CreateWindowExA(
        WS_EX_ACCEPTFILES,
        "NoteAmountGenGUI",
        "noteamountgen",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 600, 550,
        NULL, NULL, hInstance, NULL
    );
    
    if (!g_hwnd) {
        MessageBoxA(NULL, "Failed to create window", "Error", MB_OK | MB_ICONERROR);
        return 1;
    }
    
    // create font
    HFONT hFont = CreateFontA(
        -12, 0, 0, 0,
        FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, "MS Shell Dlg 2"
    );
    
    int y = 10;
    
    // browse section
    HWND label = CreateWindowExA(0, "STATIC", "Drag a chart folder here, or:", WS_CHILD | WS_VISIBLE,
                    10, y, 180, 20, g_hwnd, NULL, hInstance, NULL);
    SendMessage(label, WM_SETFONT, (WPARAM)hFont, TRUE);
    
    HWND browse_btn = CreateWindowExA(0, "BUTTON", "Browse...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                      170, y - 2, 70, 22, g_hwnd, (HMENU)ID_BROWSE_BTN, hInstance, NULL);
    SendMessage(browse_btn, WM_SETFONT, (WPARAM)hFont, TRUE);
    
    y += 30;
    
    // status
    std::string initial_status = "No chart loaded";
    if (g_ffmpeg_path.empty()) {
        initial_status += " | FFmpeg not found (audio won't be looped)";
    } else {
        initial_status += " | FFmpeg found";
    }
    g_status_label = CreateWindowExA(0, "STATIC", initial_status.c_str(), WS_CHILD | WS_VISIBLE,
                                     10, y, 570, 20, g_hwnd, NULL, hInstance, NULL);
    SendMessage(g_status_label, WM_SETFONT, (WPARAM)hFont, TRUE);
    
    y += 30;
    
    // song info
    HWND song_group = CreateWindowExA(0, "BUTTON", "Song Info", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                                      10, y, 565, 70, g_hwnd, NULL, hInstance, NULL);
    SendMessage(song_group, WM_SETFONT, (WPARAM)hFont, TRUE);
    
    g_song_label = CreateWindowExA(0, "STATIC", "(no chart loaded)", WS_CHILD | WS_VISIBLE,
                                   20, y + 20, 545, 20, g_hwnd, NULL, hInstance, NULL);
    SendMessage(g_song_label, WM_SETFONT, (WPARAM)hFont, TRUE);
    
    g_notes_label = CreateWindowExA(0, "STATIC", "Total Notes: -", WS_CHILD | WS_VISIBLE,
                                    20, y + 42, 200, 20, g_hwnd, NULL, hInstance, NULL);
    SendMessage(g_notes_label, WM_SETFONT, (WPARAM)hFont, TRUE);
    
    y += 80;
    
    // target notes
    HWND target_label = CreateWindowExA(0, "STATIC", "Target Notes:", WS_CHILD | WS_VISIBLE,
                    10, y + 3, 90, 20, g_hwnd, NULL, hInstance, NULL);
    SendMessage(target_label, WM_SETFONT, (WPARAM)hFont, TRUE);
    
    g_target_edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "3999", 
                                    WS_CHILD | WS_VISIBLE | ES_NUMBER,
                                    105, y, 80, 22, g_hwnd, (HMENU)ID_TARGET_EDIT, hInstance, NULL);
    SendMessage(g_target_edit, WM_SETFONT, (WPARAM)hFont, TRUE);
    
    HWND preset1 = CreateWindowExA(0, "BUTTON", "3999", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                   195, y, 50, 22, g_hwnd, (HMENU)ID_PRESET_3999, hInstance, NULL);
    HWND preset2 = CreateWindowExA(0, "BUTTON", "5000", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                   250, y, 50, 22, g_hwnd, (HMENU)ID_PRESET_5000, hInstance, NULL);
    HWND preset3 = CreateWindowExA(0, "BUTTON", "10000", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                   305, y, 55, 22, g_hwnd, (HMENU)ID_PRESET_10000, hInstance, NULL);
    SendMessage(preset1, WM_SETFONT, (WPARAM)hFont, TRUE);
    SendMessage(preset2, WM_SETFONT, (WPARAM)hFont, TRUE);
    SendMessage(preset3, WM_SETFONT, (WPARAM)hFont, TRUE);
    
    y += 30;
    
    // instrument and difficulty
    HWND inst_label = CreateWindowExA(0, "STATIC", "Instrument:", WS_CHILD | WS_VISIBLE,
                    10, y + 3, 70, 20, g_hwnd, NULL, hInstance, NULL);
    SendMessage(inst_label, WM_SETFONT, (WPARAM)hFont, TRUE);
    
    g_instrument_combo = CreateWindowExA(0, "COMBOBOX", "", 
                                         WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                                         85, y, 120, 200, g_hwnd, (HMENU)ID_INSTRUMENT_COMBO, hInstance, NULL);
    SendMessage(g_instrument_combo, WM_SETFONT, (WPARAM)hFont, TRUE);
    
    HWND diff_label = CreateWindowExA(0, "STATIC", "Difficulty:", WS_CHILD | WS_VISIBLE,
                    220, y + 3, 65, 20, g_hwnd, NULL, hInstance, NULL);
    SendMessage(diff_label, WM_SETFONT, (WPARAM)hFont, TRUE);
    
    g_difficulty_combo = CreateWindowExA(0, "COMBOBOX", "", 
                                         WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                                         290, y, 100, 200, g_hwnd, (HMENU)ID_DIFFICULTY_COMBO, hInstance, NULL);
    SendMessage(g_difficulty_combo, WM_SETFONT, (WPARAM)hFont, TRUE);
    
    // fill difficulty combo
    for (const auto& diff : g_state.difficulties) {
        SendMessageA(g_difficulty_combo, CB_ADDSTRING, 0, (LPARAM)diff.c_str());
    }
    SendMessage(g_difficulty_combo, CB_SETCURSEL, 0, 0);
    
    y += 30;
    
    // sections
    HWND sections_label = CreateWindowExA(0, "STATIC", "Sections:", WS_CHILD | WS_VISIBLE,
                    10, y + 3, 60, 20, g_hwnd, NULL, hInstance, NULL);
    SendMessage(sections_label, WM_SETFONT, (WPARAM)hFont, TRUE);
    
    HWND select_all = CreateWindowExA(0, "BUTTON", "Select All", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                      80, y, 75, 22, g_hwnd, (HMENU)ID_SELECT_ALL, hInstance, NULL);
    HWND select_none = CreateWindowExA(0, "BUTTON", "None", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                       160, y, 55, 22, g_hwnd, (HMENU)ID_SELECT_NONE, hInstance, NULL);
    SendMessage(select_all, WM_SETFONT, (WPARAM)hFont, TRUE);
    SendMessage(select_none, WM_SETFONT, (WPARAM)hFont, TRUE);
    
    y += 28;
    
    g_sections_list = CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", "",
                                      WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_EXTENDEDSEL | LBS_NOTIFY,
                                      10, y, 565, 120, g_hwnd, (HMENU)ID_SECTIONS_LIST, hInstance, NULL);
    SendMessage(g_sections_list, WM_SETFONT, (WPARAM)hFont, TRUE);
    
    y += 125;
    
    // selected notes label
    g_selected_label = CreateWindowExA(0, "STATIC", "Selected: 0 notes (0/0 sections)", WS_CHILD | WS_VISIBLE,
                                       10, y, 300, 20, g_hwnd, NULL, hInstance, NULL);
    SendMessage(g_selected_label, WM_SETFONT, (WPARAM)hFont, TRUE);
    
    y += 25;
    
    // generate button
    g_generate_btn = CreateWindowExA(0, "BUTTON", "Generate Chart", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                     10, y, 120, 28, g_hwnd, (HMENU)ID_GENERATE_BTN, hInstance, NULL);
    SendMessage(g_generate_btn, WM_SETFONT, (WPARAM)hFont, TRUE);
    EnableWindow(g_generate_btn, FALSE);
    
    y += 40;
    
    // result (hidden at start)
    g_result_group = CreateWindowExA(0, "BUTTON", "Result", WS_CHILD | BS_GROUPBOX,
                                     10, y, 565, 80, g_hwnd, NULL, hInstance, NULL);
    SendMessage(g_result_group, WM_SETFONT, (WPARAM)hFont, TRUE);
    
    g_result_label = CreateWindowExA(0, "STATIC", "", WS_CHILD,
                                     20, y + 18, 440, 55, g_hwnd, NULL, hInstance, NULL);
    SendMessage(g_result_label, WM_SETFONT, (WPARAM)hFont, TRUE);
    
    g_open_folder_btn = CreateWindowExA(0, "BUTTON", "Open Folder", WS_CHILD | BS_PUSHBUTTON,
                                        470, y + 30, 95, 28, g_hwnd, (HMENU)ID_OPEN_FOLDER_BTN, hInstance, NULL);
    SendMessage(g_open_folder_btn, WM_SETFONT, (WPARAM)hFont, TRUE);
    
    // show window
    ShowWindow(g_hwnd, nCmdShow);
    UpdateWindow(g_hwnd);
    
    // message loop
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    
    return (int)msg.wParam;
}
