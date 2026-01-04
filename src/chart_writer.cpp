#include "chart_writer.hpp"
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <vector>

namespace NoteGen {

// clone hero/moonscraper format
const char* LINE_END = "\r\n";
const char* INDENT = "  ";  // two spaces not tabs

void ChartWriter::write(std::ostream& out,
                        const ChartMetadata& metadata,
                        const std::vector<SyncTrackEvent>& sync_events,
                        const std::vector<LoopedSection>& sections,
                        const std::map<std::pair<SightRead::Instrument, SightRead::Difficulty>,
                                      std::vector<SightRead::Note>>& tracks,
                        const std::vector<SightRead::StarPower>& sp_phrases) {
    write_song_section(out, metadata);
    write_sync_track(out, sync_events);
    write_events(out, sections);
    
    // write each track
    for (const auto& [key, notes] : tracks) {
        auto track_name = instrument_difficulty_to_track_name(key.first, key.second);
        write_note_track(out, track_name, notes, sp_phrases);
    }
}

void ChartWriter::write_song_section(std::ostream& out, const ChartMetadata& metadata) {
    out << "[Song]" << LINE_END << "{" << LINE_END;
    out << INDENT << "Name = \"" << metadata.name << "\"" << LINE_END;
    out << INDENT << "Artist = \"" << metadata.artist << "\"" << LINE_END;
    out << INDENT << "Charter = \"" << metadata.charter << "\"" << LINE_END;
    out << INDENT << "Offset = 0" << LINE_END;
    out << INDENT << "Resolution = " << metadata.resolution << LINE_END;
    out << INDENT << "Player2 = bass" << LINE_END;
    out << INDENT << "Difficulty = 0" << LINE_END;
    out << INDENT << "PreviewStart = 0" << LINE_END;
    out << INDENT << "PreviewEnd = 0" << LINE_END;
    out << INDENT << "Genre = \"Practice\"" << LINE_END;
    out << INDENT << "MediaType = \"cd\"" << LINE_END;
    out << INDENT << "MusicStream = \"song.ogg\"" << LINE_END;
    out << "}" << LINE_END;
}

void ChartWriter::write_sync_track(std::ostream& out, const std::vector<SyncTrackEvent>& sync_events) {
    out << "[SyncTrack]" << LINE_END << "{" << LINE_END;
    
    for (const auto& event : sync_events) {
        if (event.is_bpm) {
            out << INDENT << event.position.value() << " = B " << event.bpm << LINE_END;
        } else {
            out << INDENT << event.position.value() << " = TS " << event.ts_num;
            if (event.ts_denom != 4) {
                int denom_log = 0;
                int d = event.ts_denom;
                while (d > 1) { d /= 2; denom_log++; }
                out << " " << denom_log;
            }
            out << LINE_END;
        }
    }
    
    out << "}" << LINE_END;
}

void ChartWriter::write_events(std::ostream& out, const std::vector<LoopedSection>& sections) {
    out << "[Events]" << LINE_END << "{" << LINE_END;
    
    for (const auto& section : sections) {
        out << INDENT << section.start.value() << " = E \"section " << section.name << "\"" << LINE_END;
    }
    
    // end event after last section
    if (!sections.empty()) {
        out << INDENT << sections.back().end.value() << " = E \"end\"" << LINE_END;
    }
    
    out << "}" << LINE_END;
}

void ChartWriter::write_note_track(std::ostream& out,
                                    const std::string& track_name,
                                    const std::vector<SightRead::Note>& notes,
                                    const std::vector<SightRead::StarPower>& sp_phrases) {
    out << "[" << track_name << "]" << LINE_END << "{" << LINE_END;
    
    // build sorted event list (clone hero wants notes and sp interspersed)
    struct TrackEvent {
        int64_t tick;
        int order;  // 0=note, 1=sp (notes first at same tick)
        std::string line;
    };
    std::vector<TrackEvent> events;
    
    // notes
    for (const auto& note : notes) {
        // check each fret
        for (int fret = 0; fret < 7; ++fret) {
            if (note.lengths[fret].value() >= 0) {
                std::ostringstream ss;
                ss << INDENT << note.position.value() << " = N " << fret << " " 
                   << note.lengths[fret].value() << LINE_END;
                events.push_back({note.position.value(), 0, ss.str()});
            }
        }
        
        // note flags
        // n 5 = force (flip hopo/strum)
        if (note.flags & (SightRead::FLAGS_FORCE_FLIP | SightRead::FLAGS_FORCE_HOPO | SightRead::FLAGS_FORCE_STRUM)) {
            std::ostringstream ss;
            ss << INDENT << note.position.value() << " = N 5 0" << LINE_END;
            events.push_back({note.position.value(), 0, ss.str()});
        }
        // n 6 = tap
        if (note.flags & SightRead::FLAGS_TAP) {
            std::ostringstream ss;
            ss << INDENT << note.position.value() << " = N 6 0" << LINE_END;
            events.push_back({note.position.value(), 0, ss.str()});
        }
    }
    
    // sp phrases
    for (const auto& sp : sp_phrases) {
        std::ostringstream ss;
        ss << INDENT << sp.position.value() << " = S 2 " << sp.length.value() << LINE_END;
        events.push_back({sp.position.value(), 1, ss.str()});
    }
    
    // sort by tick then order
    std::sort(events.begin(), events.end(), [](const TrackEvent& a, const TrackEvent& b) {
        if (a.tick != b.tick) return a.tick < b.tick;
        return a.order < b.order;
    });
    
    // write em
    for (const auto& event : events) {
        out << event.line;
    }
    
    out << "}" << LINE_END;
}

std::string ChartWriter::instrument_difficulty_to_track_name(SightRead::Instrument inst,
                                                              SightRead::Difficulty diff) {
    std::string diff_str;
    switch (diff) {
        case SightRead::Difficulty::Easy: diff_str = "Easy"; break;
        case SightRead::Difficulty::Medium: diff_str = "Medium"; break;
        case SightRead::Difficulty::Hard: diff_str = "Hard"; break;
        case SightRead::Difficulty::Expert: diff_str = "Expert"; break;
    }
    
    std::string inst_str;
    switch (inst) {
        case SightRead::Instrument::Guitar: inst_str = "Single"; break;
        case SightRead::Instrument::Bass: inst_str = "DoubleBass"; break;
        case SightRead::Instrument::Rhythm: inst_str = "DoubleRhythm"; break;
        case SightRead::Instrument::Keys: inst_str = "Keyboard"; break;
        case SightRead::Instrument::Drums: inst_str = "Drums"; break;
        case SightRead::Instrument::GHLGuitar: inst_str = "GHLGuitar"; break;
        case SightRead::Instrument::GHLBass: inst_str = "GHLBass"; break;
        default: inst_str = "Single"; break;
    }
    
    return diff_str + inst_str;
}

} // namespace NoteGen
