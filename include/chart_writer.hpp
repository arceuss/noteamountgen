#ifndef CHART_WRITER_HPP
#define CHART_WRITER_HPP

#include <sightread/song.hpp>
#include <sightread/songparts.hpp>
#include <sightread/tempomap.hpp>
#include <string>
#include <vector>
#include <ostream>

namespace NoteGen {

struct ChartMetadata {
    std::string name;
    std::string artist;
    std::string charter;
    int resolution = 192;
    double offset = 0.0;
};

struct LoopedSection {
    std::string name;
    SightRead::Tick start{0};
    SightRead::Tick end{0};
    int loop_count = 0;
    int note_count = 0; // notes in this section
};

// Sync track events for the generated chart
struct SyncTrackEvent {
    SightRead::Tick position{0};
    bool is_bpm = true;  // true = BPM, false = time sig
    int64_t bpm = 120000;  // for BPM events
    int ts_num = 4;        // for time sig events
    int ts_denom = 4;
};

class ChartWriter {
public:
    ChartWriter() = default;

    // Write a complete chart file
    void write(std::ostream& out, 
               const ChartMetadata& metadata,
               const std::vector<SyncTrackEvent>& sync_events,
               const std::vector<LoopedSection>& sections,
               const std::map<std::pair<SightRead::Instrument, SightRead::Difficulty>, 
                             std::vector<SightRead::Note>>& tracks,
               const std::vector<SightRead::StarPower>& sp_phrases);

private:
    void write_song_section(std::ostream& out, const ChartMetadata& metadata);
    void write_sync_track(std::ostream& out, const std::vector<SyncTrackEvent>& sync_events);
    void write_events(std::ostream& out, const std::vector<LoopedSection>& sections);
    void write_note_track(std::ostream& out, 
                          const std::string& track_name,
                          const std::vector<SightRead::Note>& notes,
                          const std::vector<SightRead::StarPower>& sp_phrases);

    std::string instrument_difficulty_to_track_name(SightRead::Instrument inst, 
                                                     SightRead::Difficulty diff);
};

} // namespace NoteGen

#endif // CHART_WRITER_HPP

