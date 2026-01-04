#include "chart_info.hpp"
#include "loop_generator.hpp"

namespace NoteGen {

nlohmann::json song_to_json(const SightRead::Song& song,
                            const SongIniData& ini_data,
                            SightRead::Instrument instrument,
                            SightRead::Difficulty difficulty) {
    nlohmann::json result;
    
    const auto& global_data = song.global_data();
    
    // prefer song.ini over chart metadata (chart often empty)
    result["name"] = !ini_data.name.empty() ? ini_data.name : global_data.name();
    result["artist"] = !ini_data.artist.empty() ? ini_data.artist : global_data.artist();
    result["charter"] = !ini_data.charter.empty() ? ini_data.charter : global_data.charter();
    result["resolution"] = global_data.resolution();
    
    // sections with note counts
    LoopGenerator gen(song, instrument, difficulty);
    auto sections = gen.get_sections();
    
    nlohmann::json sections_json = nlohmann::json::array();
    for (const auto& section : sections) {
        nlohmann::json sec;
        sec["name"] = section.name;
        sec["start_tick"] = section.start.value();
        sec["end_tick"] = section.end.value();
        sec["note_count"] = section.note_count;
        sec["duration_seconds"] = section.duration_seconds;
        sections_json.push_back(sec);
    }
    result["sections"] = sections_json;
    
    result["total_notes"] = gen.get_total_notes();
    
    // total duration
    const auto& tempo_map = global_data.tempo_map();
    if (!sections.empty()) {
        result["total_duration_seconds"] = tempo_map.to_seconds(sections.back().end).value();
    } else {
        result["total_duration_seconds"] = 0.0;
    }
    
    return result;
}

nlohmann::json get_available_tracks(const SightRead::Song& song) {
    nlohmann::json result = nlohmann::json::array();
    
    auto instruments = song.instruments();
    for (auto inst : instruments) {
        auto difficulties = song.difficulties(inst);
        for (auto diff : difficulties) {
            nlohmann::json track;
            
            // instrument to string
            switch (inst) {
                case SightRead::Instrument::Guitar: track["instrument"] = "Guitar"; break;
                case SightRead::Instrument::GuitarCoop: track["instrument"] = "GuitarCoop"; break;
                case SightRead::Instrument::Bass: track["instrument"] = "Bass"; break;
                case SightRead::Instrument::Rhythm: track["instrument"] = "Rhythm"; break;
                case SightRead::Instrument::Keys: track["instrument"] = "Keys"; break;
                case SightRead::Instrument::GHLGuitar: track["instrument"] = "GHLGuitar"; break;
                case SightRead::Instrument::GHLBass: track["instrument"] = "GHLBass"; break;
                case SightRead::Instrument::GHLRhythm: track["instrument"] = "GHLRhythm"; break;
                case SightRead::Instrument::GHLGuitarCoop: track["instrument"] = "GHLGuitarCoop"; break;
                case SightRead::Instrument::Drums: track["instrument"] = "Drums"; break;
                default: track["instrument"] = "Unknown"; break;
            }
            
            // difficulty to string
            switch (diff) {
                case SightRead::Difficulty::Easy: track["difficulty"] = "Easy"; break;
                case SightRead::Difficulty::Medium: track["difficulty"] = "Medium"; break;
                case SightRead::Difficulty::Hard: track["difficulty"] = "Hard"; break;
                case SightRead::Difficulty::Expert: track["difficulty"] = "Expert"; break;
            }
            
            // note count
            try {
                const auto& note_track = song.track(inst, diff);
                track["note_count"] = static_cast<int>(note_track.notes().size());
            } catch (...) {
                track["note_count"] = 0;
            }
            
            result.push_back(track);
        }
    }
    
    return result;
}

} // namespace NoteGen

