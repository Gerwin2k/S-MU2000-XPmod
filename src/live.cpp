// license:BSD-3-Clause
//
// Windows XP live player for S-MU2000.\n// Quiet diagnostics: periodic summaries, error-only waveOut logging.
//
// This version deliberately has no dependency on ui/audio_out.h or
// ui/audio_out.cpp.  live.exe owns its WinMM waveOut path directly.
//
// The emulator/core is otherwise used unchanged.  MIDI input remains shared
// with the GUI through ui/midi_in.
//
// Usage:
//   live <rom directory>
//   live --list
//   live <rom directory> [--midi N] [--frames N] [--buffers N]
//                       [--seconds N] [--wav file.wav]
//                       [--nomidi] [--factory] [--fast-midi] [--single]
//                       [--native-fx] [--native-fx-full] [--native-engine]
//
// Windows XP uses WinMM waveOut.  The default XP buffer configuration is
// 2048 frames x 3 buffers, matching the XP-specific configuration used by
// the original ui/audio_out implementation.

// Modified by GB 2026 for Windows XP Compatibility of live.exe 
// AI disclosure: assisted by GPT-5.6 Luna (ChatGPT.com).

#include "mu2000.h"
#include "voicecache.h"
#include "nvram.h"
#include "ui/midi_in.h"
#include "compat/console.h"

#include <windows.h>
#include <mmsystem.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>


namespace {

bool diag;

constexpr u32 RATE = 44100;

ui::midi_in g_midi;

std::atomic<bool> g_quit{false};
std::atomic<bool> g_done{false};

BOOL WINAPI on_console_ctrl(DWORD type)
{
    g_quit.store(true);

    // For console close/logoff/shutdown, keep the process alive long enough
    // for the normal shutdown path to stop audio and save NVRAM.
    if (type == CTRL_CLOSE_EVENT ||
        type == CTRL_LOGOFF_EVENT ||
        type == CTRL_SHUTDOWN_EVENT) {
        for (int i = 0; i < 400 && !g_done.load(); ++i)
            Sleep(10);
    }

    return TRUE;
}

void list_midi_inputs()
{
    const UINT n = midiInGetNumDevs();

    if (!n) {
        std::printf("MIDI input not found.\n");
        return;
    }

    std::printf("MIDI input:\n");

    for (UINT i = 0; i < n; ++i) {
        MIDIINCAPSA caps{};

        if (midiInGetDevCapsA(i, &caps, sizeof(caps)) ==
            MMSYSERR_NOERROR) {
            std::printf("  %u: %s\n", i, caps.szPname);
        }
    }
}

int midi_input_count()
{
    return int(midiInGetNumDevs());
}

void list_audio_outputs()
{
    const UINT n = waveOutGetNumDevs();

    if (!n) {
        std::printf("Audio output not found.\n");
        return;
    }

    std::printf("Audio output:\n");

    for (UINT i = 0; i < n; ++i) {
        WAVEOUTCAPSA caps{};

        if (waveOutGetDevCapsA(
                i,
                &caps,
                sizeof(caps)) == MMSYSERR_NOERROR) {
            std::printf("  %u: %s\n", i, caps.szPname);
        }
    }
}

struct generator {
    mu2000 &mu;
    std::vector<s16> *rec;
    LARGE_INTEGER freq{};

    u64 busy_ticks = 0;
    u64 produced = 0;

    u64 fills = 0;
    u64 block_overruns = 0;
    u64 cushion_overruns = 0;

    u64 fill_ticks = 0;

    u64 midi_ticks = 0;
    u64 render_ticks = 0;
    u64 worst_midi_ticks = 0;
    u64 worst_render_ticks = 0;
    u64 max_midi_bytes = 0;

    u64 worst_ticks = 0;

    u32 cushion_frames = 0;

    generator(mu2000 &m, std::vector<s16> *r)
        : mu(m), rec(r)
    {
        QueryPerformanceFrequency(&freq);
    }

    void fill(s16 *out, u32 n)
    {
        LARGE_INTEGER t0, tm, t1;
        QueryPerformanceCounter(&t0);

        u8 b;
        u64 midi_bytes_this_fill = 0;

        while (g_midi.pop(b)) {
            mu.midi_in(b);
            ++midi_bytes_this_fill;
        }

        QueryPerformanceCounter(&tm);

        for (u32 i = 0; i < n; ++i) {
            s32 l = 0;
            s32 r = 0;

            mu.run_sample(l, r);

            l = l * 32768 / mu2000::DAC_FULL_SCALE;
            r = r * 32768 / mu2000::DAC_FULL_SCALE;

            out[i * 2 + 0] =
                s16(l < -32768 ? -32768 :
                    l > 32767  ?  32767 : l);

            out[i * 2 + 1] =
                s16(r < -32768 ? -32768 :
                    r > 32767  ?  32767 : r);
        }

        QueryPerformanceCounter(&t1);

        const u64 midi_one =
            u64(tm.QuadPart - t0.QuadPart);

        const u64 render_one =
            u64(t1.QuadPart - tm.QuadPart);

        const u64 one =
            u64(t1.QuadPart - t0.QuadPart);

        midi_ticks += midi_one;
        render_ticks += render_one;

        if (midi_one > worst_midi_ticks)
            worst_midi_ticks = midi_one;

        if (render_one > worst_render_ticks)
            worst_render_ticks = render_one;

        if (midi_bytes_this_fill > max_midi_bytes)
            max_midi_bytes = midi_bytes_this_fill;

        busy_ticks += one;
        fill_ticks += one;
        ++fills;

        if (one > worst_ticks)
            worst_ticks = one;

        const double fill_ms =
            1000.0 * double(one) / double(freq.QuadPart);

        const double block_ms =
            1000.0 * double(n) / double(RATE);

        if (fill_ms > block_ms)
            ++block_overruns;

        if (cushion_frames &&
            fill_ms >
                1000.0 *
                double(cushion_frames) /
                double(RATE)) {
            ++cushion_overruns;
        }

        if (rec) {
            rec->insert(
                rec->end(),
                out,
                out + size_t(n) * 2);
        }

        produced += n;
    }

    void report(u32 period_frames) const
    {
        const double audio =
            double(produced) / double(RATE);

        const double busy =
            double(busy_ticks) / double(freq.QuadPart);

        const double avg_fill_ms =
            fills
                ? 1000.0 *
                  double(fill_ticks) /
                  double(freq.QuadPart) /
                  double(fills)
                : 0.0;

        const double worst_fill_ms =
            1000.0 *
            double(worst_ticks) /
            double(freq.QuadPart);

        const double avg_midi_ms =
            fills
                ? 1000.0 *
                  double(midi_ticks) /
                  double(freq.QuadPart) /
                  double(fills)
                : 0.0;

        const double worst_midi_ms =
            1000.0 *
            double(worst_midi_ticks) /
            double(freq.QuadPart);

        const double avg_render_ms =
            fills
                ? 1000.0 *
                  double(render_ticks) /
                  double(freq.QuadPart) /
                  double(fills)
                : 0.0;

        const double worst_render_ms =
            1000.0 *
            double(worst_render_ticks) /
            double(freq.QuadPart);

        const double block_ms =
            1000.0 *
            double(period_frames) /
            double(RATE);

        const double cpu =
            audio > 0.0
                ? 100.0 * busy / audio
                : 0.0;
		

		if (diag==true)
		{

        std::printf(
            "  %.0f seconds elapsed  MIDI %llu bytes  CPU %.1f%%\n",
            audio,
            (unsigned long long)g_midi.bytes(),
            cpu);

        std::printf(
            "     fill:   avg %.2f  ms / worst %.2f  ms / block %.2f ms\n",
            avg_fill_ms,
            worst_fill_ms,
            block_ms);

        std::printf(
            "     MIDI:   avg %.3f ms / worst  %.3f ms / max %llu bytes per fill\n",
            avg_midi_ms,
            worst_midi_ms,
            (unsigned long long)max_midi_bytes);

        std::printf(
            "     render: avg %.2f  ms / worst %.2f  ms\n",
            avg_render_ms,
            worst_render_ms);

        std::printf(
            "     fill overruns: %llu  |  cushion overruns: %llu\n",
            (unsigned long long)block_overruns,
            (unsigned long long)cushion_overruns);

        std::printf(
            "     output cushion: %.1f ms (%u frames)\n",
            1000.0 *
                double(cushion_frames) /
                double(RATE),
            cushion_frames);
		}
    }
};

int run_waveout(
    generator &gen,
    double seconds,
    int frames,
    int buffers)
{
    u64 wait_ticks_total = 0;
    u64 wait_ticks_worst = 0;
    u64 waits = 0;

    // XP-specific stable default.  Smaller blocks were observed to crackle.
    if (frames < 1024)
        frames = 1024;

    if (buffers < 3)
        buffers = 3;

    WAVEFORMATEX fmt{};

    fmt.wFormatTag =
        WAVE_FORMAT_PCM;

    fmt.nChannels =
        2;

    fmt.nSamplesPerSec =
        RATE;

    fmt.wBitsPerSample =
        16;

    fmt.nBlockAlign =
        4;

    fmt.nAvgBytesPerSec =
        RATE * 4;

    HANDLE done =
        CreateEventA(
            nullptr,
            TRUE,
            FALSE,
            nullptr);

    if (!done) {
        std::fprintf(
            stderr,
            "CreateEvent failed: %lu\n",
            (unsigned long)GetLastError());

        return 1;
    }

    HWAVEOUT hwo = nullptr;

    MMRESULT mr =
        waveOutOpen(
            &hwo,
            WAVE_MAPPER,
            &fmt,
            DWORD_PTR(done),
            0,
            CALLBACK_EVENT);

    if (mr != MMSYSERR_NOERROR) {
        char text[256] = {};

        waveOutGetErrorTextA(
            mr,
            text,
            sizeof(text));

        std::fprintf(
            stderr,
            "waveOutOpen failed: MMRESULT=%u (0x%08X): %s\n",
            (unsigned)mr,
            (unsigned)mr,
            text);

        CloseHandle(done);
        return 1;
    }

    std::vector<std::vector<s16> > pcm(
        buffers,
        std::vector<s16>(
            size_t(frames) * 2));

    std::vector<WAVEHDR> hdr(buffers);

    int prepared_count = 0;

    for (int i = 0; i < buffers; ++i) {
        std::memset(
            &hdr[i],
            0,
            sizeof(WAVEHDR));

        hdr[i].lpData =
            reinterpret_cast<LPSTR>(
                pcm[i].data());

        hdr[i].dwBufferLength =
            DWORD(
                pcm[i].size() *
                sizeof(s16));

        mr =
            waveOutPrepareHeader(
                hwo,
                &hdr[i],
                sizeof(WAVEHDR));

        if (mr != MMSYSERR_NOERROR) {
            char text[256] = {};

            waveOutGetErrorTextA(
                mr,
                text,
                sizeof(text));

            std::fprintf(
                stderr,
                "waveOutPrepareHeader(%d) failed: MMRESULT=%u (0x%08X): %s\n",
                i,
                (unsigned)mr,
                (unsigned)mr,
                text);

            for (int j = 0; j < prepared_count; ++j) {
                waveOutUnprepareHeader(
                    hwo,
                    &hdr[j],
                    sizeof(WAVEHDR));
            }

            waveOutClose(hwo);
            CloseHandle(done);
            return 1;
        }

        ++prepared_count;
    }

    std::printf(
        "waveOut: 44.1 kHz / 16-bit / stereo\n");

    std::printf(
        "waveOut buffer: %.1f ms (%d samples x %d buffers)\n",
        1000.0 *
            double(frames * buffers) /
            double(RATE),
        frames,
        buffers);

    if (seconds > 0.0)
        std::printf(
            "%.1f seconds\n",
            seconds);
    else
        std::printf(
            "Ctrl+C to exit\n");

    SetThreadPriority(
        GetCurrentThread(),
        THREAD_PRIORITY_TIME_CRITICAL);

    gen.cushion_frames =
        u32(frames) *
        u32(buffers);

    auto report_waveout_error =
        [&](const char *what, MMRESULT r) {
            char text[256] = {};

            waveOutGetErrorTextA(
                r,
                text,
                sizeof(text));

            std::fprintf(
                stderr,
                "%s: MMRESULT=%u (0x%08X): %s\n",
                what,
                (unsigned)r,
                (unsigned)r,
                text);
        };

    // Keep diagnostics cheap: successful waveOut operations are silent.
    // Errors print the full WAVEHDR state; periodic summaries are emitted
    // below without printing once per audio buffer.
    unsigned long emit_count = 0;
    unsigned long successful_writes = 0;

    auto emit =
        [&](int i) -> bool {
            gen.fill(
                pcm[i].data(),
                u32(frames));

            ++emit_count;

            const MMRESULT r =
                waveOutWrite(
                    hwo,
                    &hdr[i],
                    sizeof(WAVEHDR));

            if (r != MMSYSERR_NOERROR) {
                std::fprintf(
                    stderr,
                    "waveOutWrite ERROR: emit=%lu buffer=%d "
                    "data=%p len=%lu bytesRecorded=%lu "
                    "flags=0x%08lX prepared=%s done=%s loops=%lu\n",
                    emit_count,
                    i,
                    hdr[i].lpData,
                    (unsigned long)hdr[i].dwBufferLength,
                    (unsigned long)hdr[i].dwBytesRecorded,
                    (unsigned long)hdr[i].dwFlags,
                    (hdr[i].dwFlags & WHDR_PREPARED) ? "yes" : "no",
                    (hdr[i].dwFlags & WHDR_DONE) ? "yes" : "no",
                    (unsigned long)hdr[i].dwLoops);

                report_waveout_error(
                    "waveOutWrite",
                    r);

                std::fprintf(
                    stderr,
                    "waveOutWrite failure totals: successful=%lu emitted=%lu\n",
                    successful_writes,
                    emit_count);

                return false;
            }

            ++successful_writes;
            return true;
        };

    // Fill the complete WinMM queue before entering the wait/reuse loop.
    for (int i = 0; i < buffers; ++i) {
        if (!emit(i)) {
            waveOutReset(hwo);

            for (int j = 0; j < prepared_count; ++j) {
                waveOutUnprepareHeader(
                    hwo,
                    &hdr[j],
                    sizeof(WAVEHDR));
            }

            waveOutClose(hwo);
            CloseHandle(done);
            return 1;
        }
    }

    int next = 0;
    int result = 0;
    const u64 report_interval_frames = u64(RATE) * 5;
    u64 next_report_frames = report_interval_frames;

    while (
        !g_quit.load() &&
        (seconds <= 0.0 ||
         gen.produced < u64(seconds * RATE))) {

        while (!(hdr[next].dwFlags & WHDR_DONE)) {
            LARGE_INTEGER wt0;
            LARGE_INTEGER wt1;

            QueryPerformanceCounter(&wt0);

            const DWORD wr =
                WaitForSingleObject(
                    done,
                    100);

            QueryPerformanceCounter(&wt1);

            const u64 waited =
                u64(wt1.QuadPart - wt0.QuadPart);

            wait_ticks_total += waited;

            if (waited > wait_ticks_worst)
                wait_ticks_worst = waited;

            ++waits;

            if (wr == WAIT_FAILED) {
                std::fprintf(
                    stderr,
                    "WaitForSingleObject failed: %lu\n",
                    (unsigned long)GetLastError());

                result = 1;
                g_quit.store(true);
                break;
            }
        }

        if (g_quit.load())
            break;

        ResetEvent(done);

        if (!emit(next)) {
            result = 1;
            break;
        }

        next = (next + 1) % buffers;

        if (gen.produced >= next_report_frames) {
            gen.report(u32(frames));
            next_report_frames += report_interval_frames;
        }
    }

    if (waits) {
        std::printf(
            "  waveOut wait: avg %.3f ms / worst %.3f ms (%llu waits)\n",
            1000.0 *
                double(wait_ticks_total) /
                double(gen.freq.QuadPart) /
                double(waits),
            1000.0 *
                double(wait_ticks_worst) /
                double(gen.freq.QuadPart),
            (unsigned long long)waits);
    }

    waveOutReset(hwo);

    for (int i = 0; i < prepared_count; ++i) {
        waveOutUnprepareHeader(
            hwo,
            &hdr[i],
            sizeof(WAVEHDR));
    }

    waveOutClose(hwo);
    CloseHandle(done);

    return result;
}

void write_wav(
    const char *path,
    const std::vector<s16> &pcm)
{
    std::FILE *f =
        std::fopen(path, "wb");

    if (!f)
        return;

    const u32 bytes =
        u32(pcm.size() * sizeof(s16));

    auto w32 =
        [&](u32 v) {
            u8 b[4] = {
                u8(v),
                u8(v >> 8),
                u8(v >> 16),
                u8(v >> 24)
            };

            std::fwrite(
                b,
                1,
                4,
                f);
        };

    auto w16 =
        [&](u16 v) {
            u8 b[2] = {
                u8(v),
                u8(v >> 8)
            };

            std::fwrite(
                b,
                1,
                2,
                f);
        };

    std::fwrite("RIFF", 1, 4, f);
    w32(36 + bytes);
    std::fwrite("WAVE", 1, 4, f);

    std::fwrite("fmt ", 1, 4, f);
    w32(16);
    w16(1);
    w16(2);
    w32(RATE);
    w32(RATE * 4);
    w16(4);
    w16(16);

    std::fwrite("data", 1, 4, f);
    w32(bytes);

    std::fwrite(
        pcm.data(),
        1,
        bytes,
        f);

    std::fclose(f);

    std::printf(
        "Recording exported: %s\n",
        path);
}

} // namespace

int main(int argc, char **argv)
{
    smu2000::init_console_utf8();

    int midi_dev = -1;

    int frames = 1024;
    int buffers = 5;

    double seconds = 0.0;

    bool nomidi = false;
    bool single = false;
    bool factory = false;
    bool fast_midi = false;
	diag = false;

    // Current upstream engine options. These are applied using the same
    // mu2000 setters as current upstream live.cpp.
    int native_fx = 0;
    int native_engine = 0;

    const char *wav = nullptr;

    std::string dir;

    for (int i = 1; i < argc; ++i) {
		if (!std::strcmp(argv[i], "--list")) {
            list_midi_inputs();
            std::printf("\n");
            list_audio_outputs();
            return 0;
        }
        if (!std::strcmp(argv[i], "--midi") &&
            i + 1 < argc) {
            midi_dev = std::atoi(argv[++i]);
        }
        else if (!std::strcmp(argv[i], "--diag")) {
            diag=true; // GB 2026
        }
        else if (!std::strcmp(argv[i], "--frames") &&
                 i + 1 < argc) {
            frames = std::atoi(argv[++i]);
        }
        else if (!std::strcmp(argv[i], "--buffers") &&
                 i + 1 < argc) {
            buffers = std::atoi(argv[++i]);
        }
        else if (!std::strcmp(argv[i], "--seconds") &&
                 i + 1 < argc) {
            seconds = std::atof(argv[++i]);
        }
        else if (!std::strcmp(argv[i], "--wav") &&
                 i + 1 < argc) {
            wav = argv[++i];
        }
        else if (!std::strcmp(argv[i], "--waveout")) {
            // Kept as a compatibility option. WinMM is the only XP backend.
        }
        else if (!std::strcmp(argv[i], "--nomidi")) {
            nomidi = true;
        }
        else if (!std::strcmp(argv[i], "--factory")) {
            factory = true;
        }
        else if (!std::strcmp(argv[i], "--fast-midi")) {
            fast_midi = true;
        }
        else if (!std::strcmp(argv[i], "--single")) {
            single = true;
        }
        else if (!std::strcmp(argv[i], "--native-engine")) {
            native_engine = 1;
        }
        else if (!std::strcmp(argv[i], "--native-fx")) {
            native_fx = 1;
        }
        else if (!std::strcmp(argv[i], "--native-fx-full")) {
            native_fx = 2;
        }
        else if (!std::strcmp(argv[i], "-v")) {
            smu2000::g_verbose = true;
        }
        else if (dir.empty()) {
            dir = argv[i];
        }
    }

    if (dir.empty()) {
        std::fprintf(
            stderr,
            "Usage: live <rom directory> "
            "[--midi N] [--frames N] [--buffers N]\n"
            "       [--seconds N] [--wav file.wav]\n"
            "       [--nomidi] [--factory] [--fast-midi] [--single]\n"
            "       [--native-fx] [--native-fx-full] [--native-engine]\n"
            "       live --list\n");

        return 1;
    }

    mu2000 mu;

    if (!mu.load_program(
            dir + "/mu2000_flash.bin")) {
        std::fprintf(
            stderr,
            "%s\n",
            mu.error().c_str());

        return 1;
    }

    if (!mu.load_wave(dir + "/dump")) {
        std::fprintf(
            stderr,
            "%s\n",
            mu.error().c_str());

        return 1;
    }

    if (!mu.load_sintab(
            dir + "/standin/sin-table.bin")) {
        std::fprintf(
            stderr,
            "Warning: %s\n",
            mu.error().c_str());
    }

    mu.set_threaded(!single);
    mu.set_fast_midi(fast_midi);

    if (native_fx)
        mu.set_native_fx(native_fx);

    if (factory) {
        std::printf(
            "Starts up in factory default settings "
            "(remembered settings will be overwritten at shutdown).\n");
    }
    else if (smu2000::nvram::load(mu)) {
        std::printf(
            "Settings: %s\n",
            smu2000::nvram::path(mu).c_str());
    }

    mu.reset();

    std::printf(
        "Starting up S-MU2000 XP-Mod...");
    std::fflush(stdout);

    {
        const size_t limit =
            size_t(30.0 * RATE);

        size_t i = 0;
        s32 l = 0;
        s32 r = 0;

        for (; i < limit && !mu.midi_ready(); ++i)
            mu.run_sample(l, r);

        if (i >= limit) {
            std::fprintf(
                stderr,
                "\nStartup failure.\n");

            return 1;
        }

        std::printf(
            " %.2f seconds\n",
            double(i) / RATE);
    }

    // Match current upstream live.cpp: switch to the native engine only
    // after firmware startup has completed.
    if (native_engine) {
        mu.set_native_engine(native_engine);
        if (std::getenv("SMU2000_VOICECACHE") &&
            smu2000::voicecache::load(mu, smu2000::voicecache::key(mu))) {
            std::printf(
                "Loaded %d voices from previous native-engine cache.\n",
                int(mu.native_cal_count()));
        }
        std::printf(
            "Native engine enabled: SH-2 is run only when needed.\n");
    }

    if (nomidi)
        midi_dev = -1;
    else if (midi_dev < 0 &&
             midi_input_count() > 0)
        midi_dev = 0;

    if (midi_dev >= 0) {
        std::string merr;

        if (!g_midi.open(midi_dev, merr)) {
            std::fprintf(
                stderr,
                "MIDI input %d: %s\n",
                midi_dev,
                merr.c_str());

            return 1;
        }

        std::printf(
            "MIDI input: %d: %s\n",
            midi_dev,
            g_midi.device_name().c_str());
    }
    else {
        std::printf(
            "No MIDI input "
            "(sound is produced but nothing actually plays)\n");
    }

    std::vector<s16> rec;

    generator gen(
        mu,
        wav ? &rec : nullptr);

    SetConsoleCtrlHandler(
        on_console_ctrl,
        TRUE);

    const int rc =
        run_waveout(
            gen,
            seconds,
            frames,
            buffers);

    g_midi.close();

    if (wav && !rec.empty())
        write_wav(wav, rec);

    if (!smu2000::nvram::save(mu)) {
        std::fprintf(
            stderr,
            "Settings could not be saved: %s\n",
            smu2000::nvram::path(mu).c_str());
    }

    const double audio =
        double(gen.produced) /
        double(RATE);

    const double busy =
        double(gen.busy_ticks) /
        double(gen.freq.QuadPart);

    std::printf(
        "Finished\n"
        "Generated %.1f seconds of data in %.2f sec. "
        "CPU usage %.1f%%. MIDI %llu bytes\n",
        audio,
        busy,
        audio > 0.0
            ? 100.0 * busy / audio
            : 0.0,
        (unsigned long long)g_midi.bytes());

    g_done.store(true);

    return rc;
}
