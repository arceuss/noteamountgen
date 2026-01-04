#ifndef CHART_INFO_HPP
#define CHART_INFO_HPP

#include <sightread/song.hpp>
#include <nlohmann/json.hpp>
#include <string>
#include "ini_parser.hpp"

namespace NoteGen {

// Convert song info to JSON for the web interface
nlohmann::json song_to_json(const SightRead::Song& song, 
                            const SongIniData& ini_data = SongIniData{},
                            SightRead::Instrument instrument = SightRead::Instrument::Guitar,
                            SightRead::Difficulty difficulty = SightRead::Difficulty::Expert);

// Get available instruments and difficulties
nlohmann::json get_available_tracks(const SightRead::Song& song);

} // namespace NoteGen

#endif // CHART_INFO_HPP

