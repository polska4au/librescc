// GTK4 frontend for LibreSCC.
//
// This file is intentionally the ONLY place that talks to GTK, and the
// ONLY place with UI-specific logic. It communicates with the rest of
// the application exclusively through the pre-existing public APIs of
// Player and SynthEngine, plus the minimal setVoiceMuted()/isVoiceMuted()
// pair added to SynthEngine specifically to support the per-channel mute
// toggles below (see synth_engine.h for that addition's scope/rationale).
//
// Offline WAV/MP3 export re-implements the same small "which timbre does
// this channel use, and when do notes fire" dispatch Player's scheduler
// uses internally, via the same public helpers (parse_midi_file,
// gm_program_to_timbre, timbre_by_id, is_percussion_channel,
// SynthEngine::noteOn/noteOff/render), because Player doesn't expose an
// offline-render hook. See render_offline() below.
#include "ui.h"
#include "common.h"
#include "midi_file.h"
#include "instrument_map.h"
#include <gtk/gtk.h>
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <fstream>

#ifdef LIBRESCC_HAVE_LAME
#include <lame/lame.h>
#endif

namespace scc {

namespace {

constexpr int kNumChannelSlots = 32; // two rows of 16, matching GXSCC's layout
constexpr int kSlotsPerRow = 16;

struct RecentEntry {
    std::string path;
    std::string name;
    double duration = -1.0; // unknown until loaded at least once
};

struct AppCtx {
    Player *player = nullptr;
    SynthEngine *synth = nullptr;
    std::string initialStatus;
    std::string loadedPath; // full path of the currently loaded file, for export

    GtkWidget *window = nullptr;
    GtkWidget *play_btn = nullptr;
    GtkWidget *stop_btn = nullptr;
    GtkWidget *export_wav_btn = nullptr;
    GtkWidget *export_mp3_btn = nullptr;
    GtkWidget *progress = nullptr;
    GtkWidget *time_label = nullptr;
    GtkWidget *poly_scale = nullptr;
    GtkWidget *poly_label = nullptr;
    GtkWidget *style_dropdown = nullptr;   // now lives inside config_window
    GtkWidget *config_window = nullptr;
    GtkWidget *song_label = nullptr;
    GtkWidget *status_label = nullptr;

    // Fixed-size (kNumChannelSlots) per-slot widgets for the channel
    // matrix. Built once; slots beyond the current polyphony cap are
    // dimmed/disabled each tick rather than the grid being rebuilt.
    GtkWidget *cell_mute[kNumChannelSlots] = {};
    GtkWidget *cell_note[kNumChannelSlots] = {};
    GtkWidget *cell_wave[kNumChannelSlots] = {};
    GtkWidget *cell_level[kNumChannelSlots] = {};
    GtkWidget *cell_prio[kNumChannelSlots] = {};
    bool muteState[kNumChannelSlots] = {}; // local mirror driven by the toggle buttons

    std::vector<RecentEntry> recent;
    GMenu *recentMenu = nullptr; // "Open Recent" submenu contents, rebuilt on change

    guint timeout_id = 0;
};

const char *waveform_abbrev(Waveform w) {
    switch (w) {
        case Waveform::Square: return "SQ";
        case Waveform::Triangle: return "TRI";
        case Waveform::Sine: return "SIN";
        case Waveform::Noise: return "NS";
        default: return "?";
    }
}

const char *priority_abbrev(Priority p) {
    switch (p) {
        case Priority::Melody: return "M";
        case Priority::Harmony: return "H";
        case Priority::Percussion: return "P";
        default: return "F";
    }
}

const char *priority_css_class(Priority p) {
    switch (p) {
        case Priority::Melody: return "prio-melody";
        case Priority::Harmony: return "prio-harmony";
        case Priority::Percussion: return "prio-percussion";
        default: return "prio-filler";
    }
}

std::string format_time(double s) {
    if (s < 0 || !std::isfinite(s)) s = 0;
    int total = (int)(s + 0.5);
    int m = total / 60, sec = total % 60;
    char buf[32];
    snprintf(buf, sizeof(buf), "%d:%02d", m, sec);
    return buf;
}

std::string note_name(int note) {
    static const char *names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    if (note < 0) return "-";
    int octave = note / 12 - 1;
    return std::string(names[note % 12]) + std::to_string(octave);
}

// ---- forward declarations ----
void rebuild_recent_menu(AppCtx *ctx);
void set_status(AppCtx *ctx, const std::string &text);
void load_file_into_player(AppCtx *ctx, const std::string &path);

// ==================== offline render + export ====================

struct OfflineRender {
    std::vector<float> interleavedStereo;
    int sampleRate = 44100;
    bool ok = false;
    std::string error;
};

OfflineRender render_offline(const std::string &path, int maxVoices, StylePreset style) {
    OfflineRender r;
    MidiFile mf;
    if (!parse_midi_file(path, mf, r.error)) return r;

    SynthEngine synth(r.sampleRate, maxVoices);
    synth.setStylePreset(style);

    int channelProgram[16];
    std::fill(std::begin(channelProgram), std::end(channelProgram), 0);

    double tailSeconds = 1.0;
    long long totalFrames = (long long)((mf.durationSeconds + tailSeconds) * r.sampleRate);
    if (totalFrames <= 0) totalFrames = r.sampleRate;

    r.interleavedStereo.assign((size_t)totalFrames * 2, 0.0f);

    const int kBlock = 256;
    std::vector<float> buf((size_t)kBlock * 2);
    size_t evIdx = 0;
    long long frame = 0;

    while (frame < totalFrames) {
        double tSec = (double)frame / r.sampleRate;
        while (evIdx < mf.events.size() && mf.events[evIdx].timeSeconds <= tSec) {
            const MidiEvent &ev = mf.events[evIdx];
            switch (ev.type) {
                case MidiEventType::ProgramChange:
                    channelProgram[ev.channel] = ev.data1;
                    break;
                case MidiEventType::NoteOn: {
                    int timbreId = is_percussion_channel(ev.channel)
                                       ? TIMBRE_NOISE_PERC
                                       : gm_program_to_timbre((uint8_t)channelProgram[ev.channel]);
                    synth.noteOn(ev.channel, ev.data1, ev.data2 / 127.0f, timbre_by_id(timbreId));
                    break;
                }
                case MidiEventType::NoteOff:
                    synth.noteOff(ev.channel, ev.data1);
                    break;
            }
            evIdx++;
        }

        int n = (int)std::min<long long>(kBlock, totalFrames - frame);
        synth.render(buf.data(), n);
        std::copy(buf.begin(), buf.begin() + (size_t)n * 2, r.interleavedStereo.begin() + (size_t)frame * 2);
        frame += n;
    }

    r.ok = true;
    return r;
}

bool write_wav_file(const std::string &path, const std::vector<float> &samples, int sampleRate,
                     std::string &error) {
    std::ofstream f(path, std::ios::binary);
    if (!f) { error = "cannot open output file: " + path; return false; }

    const uint16_t numChannels = 2;
    const uint16_t bitsPerSample = 16;
    const uint32_t dataSize = (uint32_t)(samples.size() * sizeof(int16_t));
    const uint32_t byteRate = (uint32_t)sampleRate * numChannels * (bitsPerSample / 8);
    const uint16_t blockAlign = numChannels * (bitsPerSample / 8);
    const uint32_t chunkSize = 36 + dataSize;
    const uint16_t audioFormat = 1;
    const uint32_t subchunk1Size = 16;
    const uint32_t sr = (uint32_t)sampleRate;

    f.write("RIFF", 4);
    f.write(reinterpret_cast<const char *>(&chunkSize), 4);
    f.write("WAVE", 4);
    f.write("fmt ", 4);
    f.write(reinterpret_cast<const char *>(&subchunk1Size), 4);
    f.write(reinterpret_cast<const char *>(&audioFormat), 2);
    f.write(reinterpret_cast<const char *>(&numChannels), 2);
    f.write(reinterpret_cast<const char *>(&sr), 4);
    f.write(reinterpret_cast<const char *>(&byteRate), 4);
    f.write(reinterpret_cast<const char *>(&blockAlign), 2);
    f.write(reinterpret_cast<const char *>(&bitsPerSample), 2);
    f.write("data", 4);
    f.write(reinterpret_cast<const char *>(&dataSize), 4);

    for (float s : samples) {
        float clamped = std::clamp(s, -1.0f, 1.0f);
        int16_t v = (int16_t)std::lround(clamped * 32767.0f);
        f.write(reinterpret_cast<const char *>(&v), sizeof(v));
    }
    return f.good();
}

#ifdef LIBRESCC_HAVE_LAME
bool write_mp3_file(const std::string &path, const std::vector<float> &samples, int sampleRate,
                     std::string &error) {
    lame_t lame = lame_init();
    if (!lame) { error = "lame_init failed"; return false; }
    lame_set_in_samplerate(lame, sampleRate);
    lame_set_out_samplerate(lame, sampleRate);
    lame_set_num_channels(lame, 2);
    lame_set_quality(lame, 2);
    lame_set_brate(lame, 192);
    if (lame_init_params(lame) < 0) {
        error = "lame_init_params failed";
        lame_close(lame);
        return false;
    }

    size_t numFrames = samples.size() / 2;
    std::vector<short> pcmL(numFrames), pcmR(numFrames);
    for (size_t i = 0; i < numFrames; i++) {
        pcmL[i] = (short)std::lround(std::clamp(samples[i * 2 + 0], -1.0f, 1.0f) * 32767.0f);
        pcmR[i] = (short)std::lround(std::clamp(samples[i * 2 + 1], -1.0f, 1.0f) * 32767.0f);
    }

    std::vector<unsigned char> mp3buf(numFrames * 5 / 4 + 7200);
    int written = lame_encode_buffer(lame, pcmL.data(), pcmR.data(), (int)numFrames,
                                      mp3buf.data(), (int)mp3buf.size());
    if (written < 0) {
        error = "lame_encode_buffer failed (" + std::to_string(written) + ")";
        lame_close(lame);
        return false;
    }

    std::ofstream f(path, std::ios::binary);
    if (!f) {
        error = "cannot open output file: " + path;
        lame_close(lame);
        return false;
    }
    f.write(reinterpret_cast<const char *>(mp3buf.data()), written);

    int flushWritten = lame_encode_flush(lame, mp3buf.data(), (int)mp3buf.size());
    if (flushWritten > 0) f.write(reinterpret_cast<const char *>(mp3buf.data()), flushWritten);

    lame_close(lame);
    return f.good();
}
#endif

// ---- callbacks ----

void on_play_clicked(GtkButton *, gpointer user_data) {
    auto *ctx = static_cast<AppCtx *>(user_data);
    if (ctx->player->loadedFileName().empty()) {
        set_status(ctx, "No file loaded - open or drop a .mid file first");
        return;
    }
    ctx->player->play();
    set_status(ctx, "Playing: " + ctx->player->loadedFileName());
}

void on_stop_clicked(GtkButton *, gpointer user_data) {
    auto *ctx = static_cast<AppCtx *>(user_data);
    ctx->player->stop();
    set_status(ctx, "Stopped");
}

void on_poly_value_changed(GtkRange *range, gpointer user_data) {
    auto *ctx = static_cast<AppCtx *>(user_data);
    int v = (int)gtk_range_get_value(range);
    ctx->synth->setMaxVoices(v);
    gtk_label_set_text(GTK_LABEL(ctx->poly_label), (std::to_string(v) + " voices").c_str());
    // No grid rebuild needed: the channel matrix is fixed at 32 slots and
    // on_tick() dims/disables whichever slots fall outside the new cap.
}

void on_style_selected(GtkDropDown *dd, GParamSpec *, gpointer user_data) {
    auto *ctx = static_cast<AppCtx *>(user_data);
    guint idx = gtk_drop_down_get_selected(dd);
    ctx->synth->setStylePreset((StylePreset)idx);
}

void on_mute_toggled(GtkToggleButton *btn, gpointer user_data) {
    auto *ctx = static_cast<AppCtx *>(user_data);
    int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "slot-index"));
    if (idx < 0 || idx >= kNumChannelSlots) return;
    bool muted = gtk_toggle_button_get_active(btn);
    ctx->muteState[idx] = muted;
    ctx->synth->setVoiceMuted(idx, muted);
    gtk_button_set_icon_name(GTK_BUTTON(btn), muted ? "audio-volume-muted-symbolic"
                                                     : "audio-volume-high-symbolic");
}

void on_config_clicked(GtkButton *, gpointer user_data) {
    auto *ctx = static_cast<AppCtx *>(user_data);
    gtk_window_present(GTK_WINDOW(ctx->config_window));
}

void on_open_response(GObject *source, GAsyncResult *result, gpointer user_data) {
    auto *ctx = static_cast<AppCtx *>(user_data);
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source);
    GError *error = nullptr;
    GFile *file = gtk_file_dialog_open_finish(dialog, result, &error);
    if (file) {
        char *path = g_file_get_path(file);
        if (path) {
            load_file_into_player(ctx, path);
            g_free(path);
        }
        g_object_unref(file);
    } else if (error) {
        g_error_free(error);
    }
}

void start_open_dialog(AppCtx *ctx) {
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Open MIDI File");

    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "MIDI files");
    gtk_file_filter_add_pattern(filter, "*.mid");
    gtk_file_filter_add_pattern(filter, "*.midi");
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, filter);
    gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
    g_object_unref(filter);
    g_object_unref(filters);

    gtk_file_dialog_open(dialog, GTK_WINDOW(ctx->window), nullptr, on_open_response, ctx);
    g_object_unref(dialog);
}

// GAction handlers for the Open button's menu (Open.../Open Recent submenu)
void on_action_open(GSimpleAction *, GVariant *, gpointer user_data) {
    start_open_dialog(static_cast<AppCtx *>(user_data));
}

void on_action_open_recent(GSimpleAction *, GVariant *parameter, gpointer user_data) {
    auto *ctx = static_cast<AppCtx *>(user_data);
    const char *path = g_variant_get_string(parameter, nullptr);
    if (path) load_file_into_player(ctx, path);
}

gboolean on_drop(GtkDropTarget *, const GValue *value, double, double, gpointer user_data) {
    auto *ctx = static_cast<AppCtx *>(user_data);
    if (G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST)) {
        GSList *files = (GSList *)g_value_get_boxed(value);
        if (files) {
            GFile *f = G_FILE(files->data);
            char *path = g_file_get_path(f);
            if (path) {
                load_file_into_player(ctx, path);
                g_free(path);
            }
        }
        return TRUE;
    } else if (G_VALUE_HOLDS(value, G_TYPE_FILE)) {
        GFile *f = G_FILE(g_value_get_object(value));
        char *path = g_file_get_path(f);
        if (path) {
            load_file_into_player(ctx, path);
            g_free(path);
        }
        return TRUE;
    }
    return FALSE;
}

struct ExportJob {
    AppCtx *ctx;
    bool asMp3;
};

void on_export_save_response(GObject *source, GAsyncResult *result, gpointer user_data) {
    auto *job = static_cast<ExportJob *>(user_data);
    AppCtx *ctx = job->ctx;
    bool asMp3 = job->asMp3;
    delete job;

    GtkFileDialog *dialog = GTK_FILE_DIALOG(source);
    GError *error = nullptr;
    GFile *file = gtk_file_dialog_save_finish(dialog, result, &error);
    if (!file) {
        if (error) g_error_free(error);
        return;
    }
    char *pathC = g_file_get_path(file);
    g_object_unref(file);
    if (!pathC) return;
    std::string outPath = pathC;
    g_free(pathC);

    set_status(ctx, "Rendering \"" + ctx->player->loadedFileName() + "\" for export...");
    OfflineRender rendered = render_offline(ctx->loadedPath, ctx->synth->maxVoices(), ctx->synth->stylePreset());
    if (!rendered.ok) {
        set_status(ctx, "Export failed: " + rendered.error);
        return;
    }

    std::string err;
    bool ok;
    if (asMp3) {
#ifdef LIBRESCC_HAVE_LAME
        ok = write_mp3_file(outPath, rendered.interleavedStereo, rendered.sampleRate, err);
#else
        ok = false;
        err = "MP3 export not available in this build (libmp3lame not found at build time)";
#endif
    } else {
        ok = write_wav_file(outPath, rendered.interleavedStereo, rendered.sampleRate, err);
    }

    if (ok) {
        set_status(ctx, "Exported: " + outPath);
    } else {
        set_status(ctx, "Export failed: " + err);
    }
}

void start_export(AppCtx *ctx, bool asMp3) {
    if (ctx->loadedPath.empty()) {
        set_status(ctx, "No file loaded - open or drop a .mid file first");
        return;
    }
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, asMp3 ? "Export as MP3" : "Export as WAV");

    std::string baseName = ctx->player->loadedFileName();
    size_t dot = baseName.find_last_of('.');
    if (dot != std::string::npos) baseName = baseName.substr(0, dot);
    baseName += asMp3 ? ".mp3" : ".wav";
    gtk_file_dialog_set_initial_name(dialog, baseName.c_str());

    auto *job = new ExportJob{ctx, asMp3};
    gtk_file_dialog_save(dialog, GTK_WINDOW(ctx->window), nullptr, on_export_save_response, job);
    g_object_unref(dialog);
}

void on_export_wav_clicked(GtkButton *, gpointer user_data) {
    start_export(static_cast<AppCtx *>(user_data), false);
}

#ifdef LIBRESCC_HAVE_LAME
void on_export_mp3_clicked(GtkButton *, gpointer user_data) {
    start_export(static_cast<AppCtx *>(user_data), true);
}
#endif

gboolean on_tick(gpointer user_data) {
    auto *ctx = static_cast<AppCtx *>(user_data);

    double dur = ctx->player->durationSeconds();
    double pos = ctx->player->positionSeconds();
    double frac = dur > 0 ? std::clamp(pos / dur, 0.0, 1.0) : 0.0;
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(ctx->progress), frac);
    gtk_label_set_text(GTK_LABEL(ctx->time_label), (format_time(pos) + " / " + format_time(dur)).c_str());
    gtk_widget_set_sensitive(ctx->play_btn, !ctx->player->isPlaying());

    auto snap = ctx->synth->snapshotVoices();
    int maxV = ctx->synth->maxVoices();

    for (int i = 0; i < kNumChannelSlots; i++) {
        bool inRange = (i < maxV);
        bool haveData = inRange && (i < (int)snap.size());
        bool voiceActive = haveData && snap[i].active;
        bool dim = !inRange || ctx->muteState[i];

        gtk_widget_set_sensitive(ctx->cell_mute[i], inRange);

        if (haveData && voiceActive) {
            gtk_level_bar_set_value(GTK_LEVEL_BAR(ctx->cell_level[i]), snap[i].envLevel);
            gtk_label_set_text(GTK_LABEL(ctx->cell_note[i]), note_name(snap[i].note).c_str());
            gtk_label_set_text(GTK_LABEL(ctx->cell_wave[i]), waveform_abbrev(snap[i].waveform));
            gtk_label_set_text(GTK_LABEL(ctx->cell_prio[i]), priority_abbrev(snap[i].priority));
        } else {
            gtk_level_bar_set_value(GTK_LEVEL_BAR(ctx->cell_level[i]), 0.0);
            gtk_label_set_text(GTK_LABEL(ctx->cell_note[i]), "-");
            gtk_label_set_text(GTK_LABEL(ctx->cell_wave[i]), "-");
            gtk_label_set_text(GTK_LABEL(ctx->cell_prio[i]), "-");
        }

        gtk_widget_remove_css_class(ctx->cell_prio[i], "prio-melody");
        gtk_widget_remove_css_class(ctx->cell_prio[i], "prio-harmony");
        gtk_widget_remove_css_class(ctx->cell_prio[i], "prio-percussion");
        gtk_widget_remove_css_class(ctx->cell_prio[i], "prio-filler");
        if (haveData && voiceActive) gtk_widget_add_css_class(ctx->cell_prio[i], priority_css_class(snap[i].priority));

        GtkWidget *dimTargets[4] = {ctx->cell_note[i], ctx->cell_wave[i], ctx->cell_level[i], ctx->cell_prio[i]};
        for (GtkWidget *w : dimTargets) {
            if (dim) gtk_widget_add_css_class(w, "channel-dim");
            else gtk_widget_remove_css_class(w, "channel-dim");
        }
    }

    return G_SOURCE_CONTINUE;
}

// ---- helpers ----

void set_status(AppCtx *ctx, const std::string &text) {
    gtk_label_set_text(GTK_LABEL(ctx->status_label), text.c_str());
}

void load_file_into_player(AppCtx *ctx, const std::string &path) {
    std::string err;
    if (ctx->player->loadFile(path, err)) {
        ctx->loadedPath = path;
        set_status(ctx, "Loaded: " + ctx->player->loadedFileName());
        gtk_label_set_text(GTK_LABEL(ctx->song_label), ctx->player->loadedFileName().c_str());

        ctx->recent.erase(std::remove_if(ctx->recent.begin(), ctx->recent.end(),
                                          [&](const RecentEntry &e) { return e.path == path; }),
                           ctx->recent.end());
        RecentEntry re;
        re.path = path;
        re.name = ctx->player->loadedFileName();
        re.duration = ctx->player->durationSeconds();
        ctx->recent.insert(ctx->recent.begin(), re);
        if (ctx->recent.size() > 10) ctx->recent.resize(10);
        rebuild_recent_menu(ctx);
    } else {
        set_status(ctx, "Failed to load: " + err);
    }
}

void rebuild_recent_menu(AppCtx *ctx) {
    g_menu_remove_all(ctx->recentMenu);
    for (auto &entry : ctx->recent) {
        GMenuItem *item = g_menu_item_new(entry.name.c_str(), nullptr);
        g_menu_item_set_action_and_target_value(item, "win.open-recent", g_variant_new_string(entry.path.c_str()));
        g_menu_append_item(ctx->recentMenu, item);
        g_object_unref(item);
    }
}

// Builds one channel-matrix row (16 columns) into `grid`, starting at
// slot index `startSlot`. Column layout top-to-bottom: mute toggle,
// slot number, note, waveform, level meter, priority tag -- echoing
// GXSCC's per-channel column organization while using plain themed
// GTK4 widgets (toggle button, labels, level bar) rather than any custom
// skin.
void build_channel_row(AppCtx *ctx, GtkWidget *grid, int startSlot) {
    for (int col = 0; col < kSlotsPerRow; col++) {
        int slot = startSlot + col;

        GtkWidget *mute = gtk_toggle_button_new();
        gtk_button_set_icon_name(GTK_BUTTON(mute), "audio-volume-high-symbolic");
        gtk_widget_add_css_class(mute, "flat");
        gtk_widget_set_tooltip_text(mute, "Mute this channel");
        gtk_widget_set_size_request(mute, 30, 30);
        g_object_set_data(G_OBJECT(mute), "slot-index", GINT_TO_POINTER(slot));
        g_signal_connect(mute, "toggled", G_CALLBACK(on_mute_toggled), ctx);
        gtk_grid_attach(GTK_GRID(grid), mute, col, 0, 1, 1);

        GtkWidget *idx_lbl = gtk_label_new(std::to_string(slot + 1).c_str());
        gtk_widget_add_css_class(idx_lbl, "dim-label");
        gtk_grid_attach(GTK_GRID(grid), idx_lbl, col, 1, 1, 1);

        GtkWidget *note_lbl = gtk_label_new("-");
        gtk_widget_set_size_request(note_lbl, 34, -1);
        gtk_grid_attach(GTK_GRID(grid), note_lbl, col, 2, 1, 1);

        GtkWidget *wave_lbl = gtk_label_new("-");
        gtk_widget_add_css_class(wave_lbl, "dim-label");
        gtk_grid_attach(GTK_GRID(grid), wave_lbl, col, 3, 1, 1);

        GtkWidget *level = gtk_level_bar_new();
        gtk_level_bar_set_min_value(GTK_LEVEL_BAR(level), 0.0);
        gtk_level_bar_set_max_value(GTK_LEVEL_BAR(level), 1.0);
        gtk_widget_set_size_request(level, 34, 12);
        gtk_widget_add_css_class(level, "voice-level");
        gtk_grid_attach(GTK_GRID(grid), level, col, 4, 1, 1);

        GtkWidget *prio_lbl = gtk_label_new("-");
        gtk_widget_add_css_class(prio_lbl, "voice-prio-cell");
        gtk_grid_attach(GTK_GRID(grid), prio_lbl, col, 5, 1, 1);

        ctx->cell_mute[slot] = mute;
        ctx->cell_note[slot] = note_lbl;
        ctx->cell_wave[slot] = wave_lbl;
        ctx->cell_level[slot] = level;
        ctx->cell_prio[slot] = prio_lbl;
    }
}

GtkWidget *make_settings_row(const char *label, GtkWidget *control) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_start(row, 12);
    gtk_widget_set_margin_end(row, 12);
    gtk_widget_set_margin_top(row, 8);
    gtk_widget_set_margin_bottom(row, 8);
    GtkWidget *lbl = gtk_label_new(label);
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_widget_set_hexpand(lbl, TRUE);
    gtk_box_append(GTK_BOX(row), lbl);
    gtk_box_append(GTK_BOX(row), control);
    return row;
}

const char *kCssData = R"CSS(
.voice-prio-cell {
    font-weight: bold;
    font-size: 10px;
    min-width: 18px;
    border-radius: 4px;
    padding: 1px 3px;
}
.voice-prio-cell.prio-melody     { background-color: #26a269; color: #fff; }
.voice-prio-cell.prio-harmony    { background-color: #3584e4; color: #fff; }
.voice-prio-cell.prio-percussion { background-color: #e66100; color: #fff; }
.voice-prio-cell.prio-filler     { background-color: alpha(currentColor, 0.15); }
.channel-dim { opacity: 0.35; }
)CSS";

// Builds the (currently hidden) Config window. Each setting lives in its
// own row inside a "boxed-list" GtkListBox, matching the GNOME
// preferences-window idiom, specifically so more rows can be appended
// later without restructuring anything.
GtkWidget *build_config_window(AppCtx *ctx) {
    GtkWidget *win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(win), "Configuration");
    gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(ctx->window));
    gtk_window_set_modal(GTK_WINDOW(win), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(win), 380, -1);
    gtk_window_set_hide_on_close(GTK_WINDOW(win), TRUE);

    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(content, 16);
    gtk_widget_set_margin_end(content, 16);
    gtk_widget_set_margin_top(content, 16);
    gtk_widget_set_margin_bottom(content, 16);
    gtk_window_set_child(GTK_WINDOW(win), content);

    GtkWidget *heading = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(heading), "<b>Synthesis Options</b>");
    gtk_widget_set_halign(heading, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(content), heading);

    GtkWidget *list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(list, "boxed-list");

    const char *presets[] = {"SCC Classic", "Pure Square", "Pure Triangle", "Bright Sine", nullptr};
    GtkWidget *style_dd = gtk_drop_down_new_from_strings(presets);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(style_dd), (guint)ctx->synth->stylePreset());
    ctx->style_dropdown = style_dd;
    g_signal_connect(style_dd, "notify::selected", G_CALLBACK(on_style_selected), ctx);
    gtk_list_box_append(GTK_LIST_BOX(list), make_settings_row("Synthesis Style", style_dd));

    // Additional settings rows can be appended here later, e.g.:
    //   gtk_list_box_append(GTK_LIST_BOX(list), make_settings_row("...", some_widget));

    gtk_box_append(GTK_BOX(content), list);
    return win;
}

void activate(GtkApplication *app, gpointer user_data) {
    auto *ctx = static_cast<AppCtx *>(user_data);

    GtkWidget *window = gtk_application_window_new(app);
    ctx->window = window;
    gtk_window_set_title(GTK_WINDOW(window), "LibreSCC");
    gtk_window_set_default_size(GTK_WINDOW(window), 760, 520);

    // --- action group backing the Open button's menu (Open.../Open Recent) ---
    GSimpleActionGroup *actions = g_simple_action_group_new();
    GSimpleAction *openAction = g_simple_action_new("open", nullptr);
    g_signal_connect(openAction, "activate", G_CALLBACK(on_action_open), ctx);
    g_action_map_add_action(G_ACTION_MAP(actions), G_ACTION(openAction));

    GSimpleAction *openRecentAction = g_simple_action_new("open-recent", G_VARIANT_TYPE_STRING);
    g_signal_connect(openRecentAction, "activate", G_CALLBACK(on_action_open_recent), ctx);
    g_action_map_add_action(G_ACTION_MAP(actions), G_ACTION(openRecentAction));

    gtk_widget_insert_action_group(window, "win", G_ACTION_GROUP(actions));

    GMenu *rootMenu = g_menu_new();
    g_menu_append(rootMenu, "Open\xE2\x80\xA6", "win.open"); // "Open…"
    GMenu *recentMenu = g_menu_new();
    ctx->recentMenu = recentMenu;
    g_menu_append_submenu(rootMenu, "Open Recent", G_MENU_MODEL(recentMenu));

    // --- Nautilus-style flat header bar: just "LibreSCC", no subtitle ---
    GtkWidget *header = gtk_header_bar_new();
    GtkWidget *open_btn = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(open_btn), "document-open-symbolic");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(open_btn), G_MENU_MODEL(rootMenu));
    gtk_widget_set_tooltip_text(open_btn, "Open MIDI file");
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), open_btn);
    gtk_window_set_titlebar(GTK_WINDOW(window), header);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(root, 12);
    gtk_widget_set_margin_end(root, 12);
    gtk_widget_set_margin_top(root, 10);
    gtk_widget_set_margin_bottom(root, 10);
    gtk_window_set_child(GTK_WINDOW(window), root);

    // --- toolbar row: transport, export, polyphony, config ---
    GtkWidget *transport_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

    GtkWidget *play_btn = gtk_button_new_from_icon_name("media-playback-start-symbolic");
    GtkWidget *stop_btn = gtk_button_new_from_icon_name("media-playback-stop-symbolic");
    gtk_widget_add_css_class(play_btn, "circular");
    gtk_widget_add_css_class(stop_btn, "circular");
    gtk_widget_set_tooltip_text(play_btn, "Play");
    gtk_widget_set_tooltip_text(stop_btn, "Stop");
    ctx->play_btn = play_btn;
    ctx->stop_btn = stop_btn;
    g_signal_connect(play_btn, "clicked", G_CALLBACK(on_play_clicked), ctx);
    g_signal_connect(stop_btn, "clicked", G_CALLBACK(on_stop_clicked), ctx);

    GtkWidget *export_wav_btn = gtk_button_new_with_label("Export WAV");
    gtk_widget_set_tooltip_text(export_wav_btn, "Render the loaded file to a .wav file");
    g_signal_connect(export_wav_btn, "clicked", G_CALLBACK(on_export_wav_clicked), ctx);
    ctx->export_wav_btn = export_wav_btn;

    GtkWidget *export_mp3_btn = gtk_button_new_with_label("Export MP3");
#ifdef LIBRESCC_HAVE_LAME
    gtk_widget_set_tooltip_text(export_mp3_btn, "Render the loaded file to an .mp3 file");
    g_signal_connect(export_mp3_btn, "clicked", G_CALLBACK(on_export_mp3_clicked), ctx);
#else
    gtk_widget_set_sensitive(export_mp3_btn, FALSE);
    gtk_widget_set_tooltip_text(export_mp3_btn,
                                 "MP3 export unavailable: this build was compiled without libmp3lame");
#endif
    ctx->export_mp3_btn = export_mp3_btn;

    GtkWidget *poly_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *poly_caption = gtk_label_new("Polyphony");
    gtk_widget_add_css_class(poly_caption, "dim-label");
    int initialPoly = ctx->synth->maxVoices();
    GtkWidget *poly_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 4, kNumChannelSlots, 1);
    gtk_range_set_value(GTK_RANGE(poly_scale), initialPoly);
    gtk_widget_set_size_request(poly_scale, 120, -1);
    gtk_scale_set_draw_value(GTK_SCALE(poly_scale), FALSE);
    GtkWidget *poly_val_lbl = gtk_label_new((std::to_string(initialPoly) + " voices").c_str());
    ctx->poly_scale = poly_scale;
    ctx->poly_label = poly_val_lbl;
    g_signal_connect(poly_scale, "value-changed", G_CALLBACK(on_poly_value_changed), ctx);
    gtk_box_append(GTK_BOX(poly_box), poly_caption);
    gtk_box_append(GTK_BOX(poly_box), poly_scale);
    gtk_box_append(GTK_BOX(poly_box), poly_val_lbl);

    GtkWidget *config_btn = gtk_button_new_from_icon_name("preferences-system-symbolic");
    gtk_widget_set_tooltip_text(config_btn, "Configuration");
    g_signal_connect(config_btn, "clicked", G_CALLBACK(on_config_clicked), ctx);

    gtk_box_append(GTK_BOX(transport_box), play_btn);
    gtk_box_append(GTK_BOX(transport_box), stop_btn);
    gtk_box_append(GTK_BOX(transport_box), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
    gtk_box_append(GTK_BOX(transport_box), export_wav_btn);
    gtk_box_append(GTK_BOX(transport_box), export_mp3_btn);
    gtk_box_append(GTK_BOX(transport_box), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
    gtk_box_append(GTK_BOX(transport_box), poly_box);
    gtk_box_append(GTK_BOX(transport_box), config_btn);

    gtk_box_append(GTK_BOX(root), transport_box);
    gtk_box_append(GTK_BOX(root), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    // --- GXSCC-inspired channel matrix: two rows of 16 ---
    GtkWidget *matrix_frame = gtk_frame_new(nullptr);
    GtkWidget *matrix_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(matrix_box, 10);
    gtk_widget_set_margin_end(matrix_box, 10);
    gtk_widget_set_margin_top(matrix_box, 10);
    gtk_widget_set_margin_bottom(matrix_box, 10);

    GtkWidget *grid_top = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid_top), 4);
    gtk_grid_set_column_spacing(GTK_GRID(grid_top), 6);
    build_channel_row(ctx, grid_top, 0);

    GtkWidget *grid_bottom = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid_bottom), 4);
    gtk_grid_set_column_spacing(GTK_GRID(grid_bottom), 6);
    build_channel_row(ctx, grid_bottom, kSlotsPerRow);

    GtkWidget *scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
    gtk_box_append(GTK_BOX(matrix_box), grid_top);
    gtk_box_append(GTK_BOX(matrix_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append(GTK_BOX(matrix_box), grid_bottom);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), matrix_box);
    gtk_frame_set_child(GTK_FRAME(matrix_frame), scroller);
    gtk_box_append(GTK_BOX(root), matrix_frame);

    gtk_box_append(GTK_BOX(root), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    // --- Song row (mirrors GXSCC's SONG field) ---
    GtkWidget *song_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *song_caption = gtk_label_new("Song:");
    gtk_widget_add_css_class(song_caption, "dim-label");
    GtkWidget *song_lbl = gtk_label_new("No file loaded");
    ctx->song_label = song_lbl;
    gtk_box_append(GTK_BOX(song_row), song_caption);
    gtk_box_append(GTK_BOX(song_row), song_lbl);
    gtk_box_append(GTK_BOX(root), song_row);

    // --- Position row (mirrors GXSCC's POSITION field) ---
    GtkWidget *pos_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *progress = gtk_progress_bar_new();
    gtk_widget_set_hexpand(progress, TRUE);
    gtk_widget_set_valign(progress, GTK_ALIGN_CENTER);
    ctx->progress = progress;
    GtkWidget *time_lbl = gtk_label_new("0:00 / 0:00");
    ctx->time_label = time_lbl;
    gtk_box_append(GTK_BOX(pos_row), progress);
    gtk_box_append(GTK_BOX(pos_row), time_lbl);
    gtk_box_append(GTK_BOX(root), pos_row);

    // --- status bar ---
    GtkWidget *status_lbl = gtk_label_new(ctx->initialStatus.c_str());
    gtk_widget_set_halign(status_lbl, GTK_ALIGN_START);
    gtk_widget_add_css_class(status_lbl, "dim-label");
    ctx->status_label = status_lbl;
    gtk_box_append(GTK_BOX(root), status_lbl);

    // --- Config window (hidden until requested) ---
    ctx->config_window = build_config_window(ctx);

    // --- drag & drop ---
    GtkDropTarget *drop = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
    g_signal_connect(drop, "drop", G_CALLBACK(on_drop), ctx);
    gtk_widget_add_controller(window, GTK_EVENT_CONTROLLER(drop));

    // --- minimal theme-friendly CSS (priority tags + dimming only) ---
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider, kCssData);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
                                                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);

    ctx->timeout_id = g_timeout_add(33, on_tick, ctx);

    gtk_window_present(GTK_WINDOW(window));
}

} // namespace

int run_ui(Player &player, SynthEngine &synth, const std::string &initialStatus) {
    AppCtx ctx;
    ctx.player = &player;
    ctx.synth = &synth;
    ctx.initialStatus = initialStatus;

    GtkApplication *app = gtk_application_new("org.librescc.LibreSCC", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), &ctx);
    int status = g_application_run(G_APPLICATION(app), 0, nullptr);
    if (ctx.timeout_id) g_source_remove(ctx.timeout_id);
    g_object_unref(app);
    return status;
}

} // namespace scc
