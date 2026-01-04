#ifndef LOOP_GENERATOR_HPP
#define LOOP_GENERATOR_HPP

#include <sightread/song.hpp>
#include <sightread/songparts.hpp>
#include "chart_writer.hpp"
#include "ini_parser.hpp"
#include <vector>
#include <string>
#include <optional>

namespace NoteGen {

struct SectionInfo {
    std::string name;
    SightRead::Tick start{0};
    SightRead::Tick end{0};
    int note_count = 0;
    double duration_seconds = 0.0;
};

struct GenerationConfig {
    int target_note_count = 3999;
    std::vector<std::string> selected_sections; // empty = all sections
    bool loop_full_song = false; // if true, loop entire song instead of sections
};

struct GenerationResult {
    bool success = false;
    std::string error_message;
    std::string chart_data;
    std::vector<LoopedSection> looped_sections;
    std::vector<SyncTrackEvent> sync_events;
    int total_notes = 0;
    double total_duration_seconds = 0.0;
    bool is_full_song = false;  // True if all sections selected
    std::string folder_name;    // For ZIP filename
    std::string chart_name;     // For song.ini
    
    // Audio timing info for FFmpeg
    struct AudioSegment {
        double start_seconds;
        double duration_seconds;
        int repeat_count;
    };
    std::vector<AudioSegment> audio_segments;
};

class LoopGenerator {
public:
    explicit LoopGenerator(const SightRead::Song& song, 
                           SightRead::Instrument instrument = SightRead::Instrument::Guitar,
                           SightRead::Difficulty difficulty = SightRead::Difficulty::Expert,
                           const SongIniData& ini_data = SongIniData{});

    // Get info about available sections
    std::vector<SectionInfo> get_sections() const;
    
    // Get total note count for the track
    int get_total_notes() const;
    
    // Generate a looped chart
    GenerationResult generate(const GenerationConfig& config);

private:
    const SightRead::Song& m_song;
    SightRead::Instrument m_instrument;
    SightRead::Difficulty m_difficulty;
    const SightRead::NoteTrack* m_track;
    SongIniData m_ini_data;
    
    // Helper to count notes in a tick range
    int count_notes_in_range(SightRead::Tick start, SightRead::Tick end) const;
    
    // Get the end tick for a section (start of next section or end of song)
    SightRead::Tick get_section_end(size_t section_index) const;
    
    // Generate notes by looping sections
    std::vector<SightRead::Note> generate_looped_notes(
        const std::vector<SectionInfo>& sections_to_loop,
        int target_notes,
        std::vector<LoopedSection>& out_looped_sections,
        std::vector<GenerationResult::AudioSegment>& out_audio_segments,
        std::vector<SyncTrackEvent>& out_sync_events,
        std::vector<SightRead::StarPower>& out_sp_phrases,
        bool is_full_song);
};

} // namespace NoteGen

#endif // LOOP_GENERATOR_HPP

