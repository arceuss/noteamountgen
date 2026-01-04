#include "loop_generator.hpp"
#include <algorithm>
#include <sstream>
#include <set>

namespace NoteGen {

LoopGenerator::LoopGenerator(const SightRead::Song& song,
                             SightRead::Instrument instrument,
                             SightRead::Difficulty difficulty,
                             const SongIniData& ini_data)
    : m_song(song)
    , m_instrument(instrument)
    , m_difficulty(difficulty)
    , m_track(nullptr)
    , m_ini_data(ini_data) {
    try {
        m_track = &song.track(instrument, difficulty);
    } catch (...) {
        // track doesnt exist
    }
}

std::vector<SectionInfo> LoopGenerator::get_sections() const {
    std::vector<SectionInfo> result;
    
    const auto& practice_sections = m_song.global_data().practice_sections();
    const auto& tempo_map = m_song.global_data().tempo_map();
    
    for (size_t i = 0; i < practice_sections.size(); ++i) {
        SectionInfo info;
        info.name = practice_sections[i].name;
        info.start = practice_sections[i].start;
        info.end = get_section_end(i);
        info.note_count = count_notes_in_range(info.start, info.end);
        
        // duration in seconds
        auto start_seconds = tempo_map.to_seconds(info.start);
        auto end_seconds = tempo_map.to_seconds(info.end);
        info.duration_seconds = end_seconds.value() - start_seconds.value();
        
        result.push_back(info);
    }
    
    return result;
}

int LoopGenerator::get_total_notes() const {
    if (!m_track) return 0;
    return static_cast<int>(m_track->notes().size());
}

SightRead::Tick LoopGenerator::get_section_end(size_t section_index) const {
    const auto& practice_sections = m_song.global_data().practice_sections();
    
    if (section_index + 1 < practice_sections.size()) {
        return practice_sections[section_index + 1].start;
    }
    
    // last section - find end of last note
    if (m_track && !m_track->notes().empty()) {
        const auto& last_note = m_track->notes().back();
        SightRead::Tick max_end = last_note.position;
        
        // account for sustains
        for (int i = 0; i < 7; ++i) {
            if (last_note.lengths[i].value() > 0) {
                auto note_end = last_note.position + last_note.lengths[i];
                if (note_end > max_end) {
                    max_end = note_end;
                }
            }
        }
        
        // add some padding
        return max_end + SightRead::Tick(m_song.global_data().resolution());
    }
    
    return SightRead::Tick(0);
}

int LoopGenerator::count_notes_in_range(SightRead::Tick start, SightRead::Tick end) const {
    if (!m_track) return 0;
    
    int count = 0;
    for (const auto& note : m_track->notes()) {
        if (note.position >= start && note.position < end) {
            ++count;
        }
    }
    return count;
}

GenerationResult LoopGenerator::generate(const GenerationConfig& config) {
    GenerationResult result;
    
    if (!m_track) {
        result.error_message = "Track not found for selected instrument/difficulty";
        return result;
    }
    
    // get sections to loop
    auto all_sections = get_sections();
    std::vector<SectionInfo> sections_to_loop;
    
    if (config.selected_sections.empty()) {
        // use all sections
        sections_to_loop = all_sections;
    } else {
        // filter to selected
        for (const auto& section : all_sections) {
            if (std::find(config.selected_sections.begin(), 
                         config.selected_sections.end(), 
                         section.name) != config.selected_sections.end()) {
                sections_to_loop.push_back(section);
            }
        }
    }
    
    if (sections_to_loop.empty()) {
        result.error_message = "No sections selected";
        return result;
    }
    
    // generate looped notes
    // full song if all sections selected
    bool is_full_song = config.selected_sections.empty() || 
                        (sections_to_loop.size() == all_sections.size());
    
    std::vector<SightRead::StarPower> sp_phrases;
    auto looped_notes = generate_looped_notes(
        sections_to_loop, 
        config.target_note_count,
        result.looped_sections,
        result.audio_segments,
        result.sync_events,
        sp_phrases,
        is_full_song
    );
    
    result.total_notes = static_cast<int>(looped_notes.size());
    
    // calc total duration
    for (const auto& seg : result.audio_segments) {
        result.total_duration_seconds += seg.duration_seconds * seg.repeat_count;
    }
    
    // build chart name (prefer song.ini over chart metadata)
    std::string song_name = !m_ini_data.name.empty() ? m_ini_data.name : m_song.global_data().name();
    std::string artist = !m_ini_data.artist.empty() ? m_ini_data.artist : m_song.global_data().artist();
    std::string charter = !m_ini_data.charter.empty() ? m_ini_data.charter : m_song.global_data().charter();
    
    std::string chart_name;
    std::string folder_name;  // For ZIP filename
    if (is_full_song) {
        // full song format
        chart_name = std::to_string(result.total_notes) + " - " + song_name;
        folder_name = std::to_string(result.total_notes) + "_" + song_name;
    } else {
        // selected sections format
        auto replace_underscores = [](std::string str) {
            std::replace(str.begin(), str.end(), '_', ' ');
            return str;
        };
        
        std::string section_names;
        std::string section_names_underscore;
        for (size_t i = 0; i < sections_to_loop.size(); ++i) {
            if (i > 0) {
                section_names += ", ";
                section_names_underscore += "_";
            }
            section_names += replace_underscores(sections_to_loop[i].name);
            section_names_underscore += sections_to_loop[i].name;
            if (section_names.length() > 50) {
                section_names += "...";
                section_names_underscore += "...";
                break;
            }
        }
        chart_name = std::to_string(result.total_notes) + " " + section_names + " - " + song_name;
        folder_name = std::to_string(result.total_notes) + "_" + section_names_underscore;
    }
    result.folder_name = folder_name;
    result.chart_name = chart_name;
    
    ChartMetadata metadata;
    metadata.name = chart_name;
    metadata.artist = artist;
    metadata.charter = charter;
    metadata.resolution = m_song.global_data().resolution();
    
    std::map<std::pair<SightRead::Instrument, SightRead::Difficulty>, 
             std::vector<SightRead::Note>> tracks;
    tracks[{m_instrument, m_difficulty}] = looped_notes;
    
    std::ostringstream chart_stream;
    ChartWriter writer;
    writer.write(chart_stream, metadata, result.sync_events, result.looped_sections, tracks, sp_phrases);
    
    result.chart_data = chart_stream.str();
    result.is_full_song = is_full_song;
    result.success = true;
    
    return result;
}

std::vector<SightRead::Note> LoopGenerator::generate_looped_notes(
    const std::vector<SectionInfo>& sections_to_loop,
    int target_notes,
    std::vector<LoopedSection>& out_looped_sections,
    std::vector<GenerationResult::AudioSegment>& out_audio_segments,
    std::vector<SyncTrackEvent>& out_sync_events,
    std::vector<SightRead::StarPower>& out_sp_phrases,
    bool is_full_song) {
    
    std::vector<SightRead::Note> result;
    const auto& tempo_map = m_song.global_data().tempo_map();
    const auto& original_sp = m_track->sp_phrases();
    
    SightRead::Tick current_tick(0);
    int current_notes = 0;
    int full_loop_count = 0;
    
    if (is_full_song) {
        // full song mode: loop entire song from tick 0
        SightRead::Tick song_start(0);
        SightRead::Tick song_end = sections_to_loop.back().end;
        SightRead::Tick full_pass_duration = song_end - song_start;
        
        // audio from 0 to end of last section
        double full_pass_audio_start = 0.0;
        double full_pass_audio_end = tempo_map.to_seconds(song_end).value();
        double full_pass_audio_duration = full_pass_audio_end;
        
        // get all notes
        std::vector<SightRead::Note> all_notes;
        for (const auto& note : m_track->notes()) {
            if (note.position < song_end) {
                all_notes.push_back(note);
            }
        }
        
        // get all sp phrases
        std::vector<SightRead::StarPower> all_sp;
        for (const auto& sp : original_sp) {
            if (sp.position < song_end) {
                all_sp.push_back(sp);
            }
        }
        
        while (current_notes < target_notes) {
            ++full_loop_count;
            
            SightRead::Tick loop_offset = current_tick;
            
            // sync track events for this loop
            for (const auto& ts : tempo_map.time_sigs()) {
                if (ts.position < song_end) {
                    SyncTrackEvent event;
                    event.position = SightRead::Tick(ts.position.value() + loop_offset.value());
                    event.is_bpm = false;
                    event.ts_num = ts.numerator;
                    event.ts_denom = ts.denominator;
                    out_sync_events.push_back(event);
                }
            }
            for (const auto& bpm : tempo_map.bpms()) {
                if (bpm.position < song_end) {
                    SyncTrackEvent event;
                    event.position = SightRead::Tick(bpm.position.value() + loop_offset.value());
                    event.is_bpm = true;
                    event.bpm = bpm.bpm;
                    out_sync_events.push_back(event);
                }
            }
            
            // sp phrases with offset
            for (const auto& sp : all_sp) {
                out_sp_phrases.push_back({
                    SightRead::Tick(sp.position.value() + loop_offset.value()),
                    sp.length
                });
            }
            
            // section markers
            for (const auto& section : sections_to_loop) {
                std::string display_name = section.name;
                std::replace(display_name.begin(), display_name.end(), '_', ' ');
                
                LoopedSection looped_sec;
                looped_sec.name = display_name + " " + std::to_string(full_loop_count);
                // add loop offset to section position
                looped_sec.start = SightRead::Tick(section.start.value() + loop_offset.value());
                looped_sec.end = SightRead::Tick(section.end.value() + loop_offset.value());
                looped_sec.loop_count = 1;
                looped_sec.note_count = section.note_count;
                out_looped_sections.push_back(looped_sec);
            }
            
            // add notes
            SightRead::Tick last_note_tick = loop_offset;
            int notes_this_loop = 0;
            
            for (const auto& note : all_notes) {
                if (current_notes >= target_notes) break;
                
                SightRead::Note new_note = note;
                // offset note position
                new_note.position = SightRead::Tick(note.position.value() + loop_offset.value());
                result.push_back(new_note);
                last_note_tick = new_note.position;
                ++current_notes;
                ++notes_this_loop;
            }
            
            // Audio segment
            double audio_duration;
            if (current_notes >= target_notes && notes_this_loop < static_cast<int>(all_notes.size())) {
                // partial loop - time of last note relative to loop start
                SightRead::Tick relative_last = SightRead::Tick(last_note_tick.value() - loop_offset.value());
                audio_duration = tempo_map.to_seconds(relative_last).value() + 0.5;
            } else {
                audio_duration = full_pass_audio_duration;
            }
            
            GenerationResult::AudioSegment audio_seg;
            audio_seg.start_seconds = full_pass_audio_start;  // Always 0 for full song
            audio_seg.duration_seconds = audio_duration;
            audio_seg.repeat_count = 1;
            out_audio_segments.push_back(audio_seg);
            
            current_tick = current_tick + full_pass_duration;
        }
    } else {
        // selected sections mode: loop each section individually
        // track processed sections for hopo->tap on first occurrence
        std::set<std::string> processed_sections;
        bool first_section_processed = false;
        
        while (current_notes < target_notes) {
            ++full_loop_count;
            
            for (const auto& section : sections_to_loop) {
                if (current_notes >= target_notes) break;
                
                // get notes in section
                std::vector<SightRead::Note> section_notes;
                for (const auto& note : m_track->notes()) {
                    if (note.position >= section.start && note.position < section.end) {
                        section_notes.push_back(note);
                    }
                }
                
                if (section_notes.empty()) continue;
                
                // get sp in section
                std::vector<SightRead::StarPower> section_sp;
                for (const auto& sp : original_sp) {
                    if (sp.position >= section.start && sp.position < section.end) {
                        section_sp.push_back(sp);
                    }
                }
                
                SightRead::Tick section_duration = section.end - section.start;
                SightRead::Tick loop_offset = current_tick;
                SightRead::Tick original_offset = section.start;
                
                // tempo/time sig events for this section
                // find initial time sig (last ts before or at section start)
                int init_ts_idx = 0;
                for (size_t i = 0; i < tempo_map.time_sigs().size(); ++i) {
                    if (tempo_map.time_sigs()[i].position <= section.start) {
                        init_ts_idx = static_cast<int>(i);
                    }
                }
                if (!tempo_map.time_sigs().empty()) {
                    SyncTrackEvent ts_event;
                    ts_event.position = loop_offset;
                    ts_event.is_bpm = false;
                    ts_event.ts_num = tempo_map.time_sigs()[init_ts_idx].numerator;
                    ts_event.ts_denom = tempo_map.time_sigs()[init_ts_idx].denominator;
                    // Only add if this is a new position or different value
                    bool should_add = out_sync_events.empty();
                    if (!should_add) {
                        // check if already have ts at this position
                        bool found = false;
                        for (const auto& ev : out_sync_events) {
                            if (!ev.is_bpm && ev.position == loop_offset) {
                                found = true;
                                break;
                            }
                        }
                        should_add = !found;
                    }
                    if (should_add) {
                        out_sync_events.push_back(ts_event);
                    }
                }
                
                // find initial bpm
                int init_bpm_idx = 0;
                for (size_t i = 0; i < tempo_map.bpms().size(); ++i) {
                    if (tempo_map.bpms()[i].position <= section.start) {
                        init_bpm_idx = static_cast<int>(i);
                    }
                }
                if (!tempo_map.bpms().empty()) {
                    SyncTrackEvent bpm_event;
                    bpm_event.position = loop_offset;
                    bpm_event.is_bpm = true;
                    bpm_event.bpm = tempo_map.bpms()[init_bpm_idx].bpm;
                    // Only add if this is a new position or different value
                    bool should_add = out_sync_events.empty();
                    if (!should_add) {
                        bool found = false;
                        for (const auto& ev : out_sync_events) {
                            if (ev.is_bpm && ev.position == loop_offset) {
                                found = true;
                                break;
                            }
                        }
                        should_add = !found;
                    }
                    if (should_add) {
                        out_sync_events.push_back(bpm_event);
                    }
                }
                
                // tempo/ts changes within section
                for (const auto& ts : tempo_map.time_sigs()) {
                    if (ts.position > section.start && ts.position < section.end) {
                        SyncTrackEvent ts_event;
                        ts_event.position = SightRead::Tick(ts.position.value() - original_offset.value() + loop_offset.value());
                        ts_event.is_bpm = false;
                        ts_event.ts_num = ts.numerator;
                        ts_event.ts_denom = ts.denominator;
                        out_sync_events.push_back(ts_event);
                    }
                }
                
                for (const auto& bpm : tempo_map.bpms()) {
                    if (bpm.position > section.start && bpm.position < section.end) {
                        SyncTrackEvent bpm_event;
                        bpm_event.position = SightRead::Tick(bpm.position.value() - original_offset.value() + loop_offset.value());
                        bpm_event.is_bpm = true;
                        bpm_event.bpm = bpm.bpm;
                        out_sync_events.push_back(bpm_event);
                    }
                }
                
                first_section_processed = true;
                
                // section marker
                std::string display_name = section.name;
                std::replace(display_name.begin(), display_name.end(), '_', ' ');
                
                LoopedSection looped_sec;
                looped_sec.name = display_name + " " + std::to_string(full_loop_count);
                looped_sec.start = loop_offset;
                looped_sec.end = loop_offset + section_duration;
                looped_sec.loop_count = 1;
                looped_sec.note_count = static_cast<int>(section_notes.size());
                out_looped_sections.push_back(looped_sec);
                
                // sp phrases with offset
                for (const auto& sp : section_sp) {
                    out_sp_phrases.push_back({
                        SightRead::Tick(sp.position.value() - original_offset.value() + loop_offset.value()),
                        sp.length
                    });
                }
                
                // check if first time processing this section
                bool is_first_occurrence = (processed_sections.find(section.name) == processed_sections.end());
                if (is_first_occurrence) {
                    processed_sections.insert(section.name);
                }
                
                // add notes
                SightRead::Tick last_note_tick = loop_offset;
                int notes_this_section = 0;
                bool is_first_note_of_section = true;
                
                for (const auto& note : section_notes) {
                    if (current_notes >= target_notes) break;
                    
                    SightRead::Note new_note = note;
                    new_note.position = SightRead::Tick(note.position.value() - original_offset.value() + loop_offset.value());
                    
                    // if first note of section and its a hopo, make it a tap
                    if (is_first_note_of_section && is_first_occurrence) {
                        if (new_note.flags & SightRead::FLAGS_HOPO) {
                            // remove hopo, add tap
                            new_note.flags = static_cast<SightRead::NoteFlags>(
                                (new_note.flags & ~SightRead::FLAGS_HOPO) | SightRead::FLAGS_TAP
                            );
                        }
                    }
                    is_first_note_of_section = false;
                    
                    result.push_back(new_note);
                    last_note_tick = new_note.position;
                    ++current_notes;
                    ++notes_this_section;
                }
                
                // audio segment
                double audio_start = tempo_map.to_seconds(section.start).value();
                double audio_duration;
                
                if (current_notes >= target_notes && notes_this_section < static_cast<int>(section_notes.size())) {
                    SightRead::Tick relative_last = SightRead::Tick(last_note_tick.value() - loop_offset.value() + original_offset.value());
                    audio_duration = tempo_map.to_seconds(relative_last).value() - audio_start + 0.5;
                } else {
                    audio_duration = section.duration_seconds;
                }
                
                GenerationResult::AudioSegment audio_seg;
                audio_seg.start_seconds = audio_start;
                audio_seg.duration_seconds = audio_duration;
                audio_seg.repeat_count = 1;
                out_audio_segments.push_back(audio_seg);
                
                current_tick = current_tick + section_duration;
            }
        }
    }
    
    return result;
}

} // namespace NoteGen

