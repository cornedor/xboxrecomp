/**
 * XAudio2 Audio Output Backend
 *
 * Provides low-latency audio output via XAudio2 (Win7+).
 * Called from the APU monitor frame to submit mixed samples.
 * Falls back gracefully if XAudio2 is unavailable.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "apu_xaudio2.h"

/* XAudio2 on Windows; elsewhere the same xa2_* interface on SDL2's audio
 * queue (below). */
#if defined(_WIN32)

#define COBJMACROS
#include <windows.h>
#include <xaudio2.h>

#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "ole32.lib")

#define XA2_SAMPLE_RATE   48000
#define XA2_CHANNELS      2
#define XA2_BUF_SAMPLES   1024   /* ~21ms per submission */
#define XA2_NUM_BUFS      16     /* ring size; how many may queue depends on
                                    * the submission size, see xa2_submit_samples */

static IXAudio2               *g_xa2 = NULL;
static IXAudio2MasteringVoice *g_xa2_master = NULL;
static IXAudio2SourceVoice    *g_xa2_source = NULL;
static int16_t                 g_xa2_bufs[XA2_NUM_BUFS][XA2_BUF_SAMPLES][2];
static int                     g_xa2_next_buf = 0;
static int                     g_xa2_initialized = 0;
static int                     g_xa2_frames_written = 0;
static int                     g_xa2_underruns, g_xa2_drops, g_xa2_waits, g_xa2_refills;
static int                     g_xa2_min_queued = 1 << 30;

int xa2_init(void)
{
    HRESULT hr;
    int com_initialized;
    WAVEFORMATEX wfx = { 0 };

    if (g_xa2_initialized) return 1;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        fprintf(stderr, "[XA2] CoInitializeEx failed: 0x%08lX\n", hr);
        return 0;
    }
    com_initialized = SUCCEEDED(hr);

    hr = XAudio2Create(&g_xa2, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr) || !g_xa2) {
        fprintf(stderr, "[XA2] XAudio2Create failed: 0x%08lX\n", hr);
        goto fail;
    }

    hr = IXAudio2_CreateMasteringVoice(g_xa2, &g_xa2_master,
        XA2_CHANNELS, XA2_SAMPLE_RATE, 0, NULL, NULL, 0);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] CreateMasteringVoice failed: 0x%08lX\n", hr);
        goto fail;
    }

    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = XA2_CHANNELS;
    wfx.nSamplesPerSec  = XA2_SAMPLE_RATE;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = XA2_CHANNELS * 2;
    wfx.nAvgBytesPerSec = XA2_SAMPLE_RATE * wfx.nBlockAlign;

    hr = IXAudio2_CreateSourceVoice(g_xa2, &g_xa2_source,
        &wfx, 0, XAUDIO2_DEFAULT_FREQ_RATIO, NULL, NULL, NULL);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] CreateSourceVoice failed: 0x%08lX\n", hr);
        goto fail;
    }

    hr = IXAudio2SourceVoice_Start(g_xa2_source, 0, XAUDIO2_COMMIT_NOW);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] Start failed: 0x%08lX\n", hr);
        goto fail;
    }

    g_xa2_next_buf = 0;
    g_xa2_initialized = 1;
    g_xa2_frames_written = 0;

    fprintf(stderr, "[XA2] XAudio2 initialized (%d Hz stereo 16-bit, %d x %d-sample buffers)\n",
            XA2_SAMPLE_RATE, XA2_NUM_BUFS, XA2_BUF_SAMPLES);
    return 1;

fail:
    xa2_shutdown();
    /* Failed initialization still runs on the COM-initializing thread. */
    if (com_initialized) CoUninitialize();
    return 0;
}

void xa2_shutdown(void)
{
    if (g_xa2_source) {
        IXAudio2SourceVoice_Stop(g_xa2_source, 0, XAUDIO2_COMMIT_NOW);
        IXAudio2SourceVoice_FlushSourceBuffers(g_xa2_source);
        g_xa2_source->lpVtbl->DestroyVoice(g_xa2_source);
        g_xa2_source = NULL;
    }
    if (g_xa2_master) {
        g_xa2_master->lpVtbl->DestroyVoice(g_xa2_master);
        g_xa2_master = NULL;
    }
    if (g_xa2) {
        IXAudio2_Release(g_xa2);
        g_xa2 = NULL;
    }

    if (g_xa2_initialized)
        fprintf(stderr, "[XA2] Shut down (%d frames written)\n", g_xa2_frames_written);
    g_xa2_initialized = 0;
}

int xa2_is_active(void)
{
    return g_xa2_initialized;
}

/* Submit a buffer of mixed samples to XAudio2.
 * Called from APU frame thread. Returns 1 if buffer was submitted. */
int xa2_submit_samples(const int16_t *samples, int num_samples)
{
    XAUDIO2_VOICE_STATE state;
    XAUDIO2_BUFFER xbuf;
    int idx;
    int copy_samples;
    HRESULT hr;

    if (!g_xa2_initialized || !g_xa2_source) return 0;

    IXAudio2SourceVoice_GetState(g_xa2_source, &state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    if (state.BuffersQueued == 0 && g_xa2_frames_written)
        g_xa2_underruns++;
    /* Voice-processor frames (256 samples, 5.3 ms) arrive in step with the
     * card, so the queue keeps whatever depth it had -- in some runs 1-2
     * buffers, where every late frame-thread wake is an underrun (a click).
     * Keep a cushion: start, and restart after running (nearly) dry, with
     * silence up to 8 buffers (43 ms). The full side is handled below. */
    if (num_samples <= 256 && state.BuffersQueued <= 1) {
        static const int16_t silence[256][2];
        XAUDIO2_BUFFER sb;
        UINT32 k;
        memset(&sb, 0, sizeof(sb));
        sb.AudioBytes = (UINT32)(num_samples * XA2_CHANNELS * sizeof(int16_t));
        sb.pAudioData = (const BYTE *)silence;
        for (k = state.BuffersQueued; k < 7; k++)
            if (FAILED(IXAudio2SourceVoice_SubmitSourceBuffer(g_xa2_source, &sb, NULL)))
                break;
        if (g_xa2_frames_written)
            g_xa2_refills++;
        state.BuffersQueued = k;
    }
    if ((int)state.BuffersQueued < g_xa2_min_queued)
        g_xa2_min_queued = (int)state.BuffersQueued;
    /* ~64 ms of queue either way: 3 buffers of the HLE mixer's 1024 samples,
     * or 12 of the 256-sample frames the emulated APU's voice processor
     * delivers (8 VP frames of 32), which arrive in step with real time and
     * need the slack to ride out a frame thread that is a little late. */
    {
        /* A full queue means this side runs ahead of the sound card (the
         * frame thread is paced by the system clock, which drifts against
         * the card's). The pacing is done by the frame thread before it
         * mixes (apu_core.c throttle, lock released); waiting here held the
         * APU's locks, and the guest's DirectSound writes queued behind them
         * with the guest CPU held -- long sessions crawled at ~20 FPS.
         * Dropping is the last resort. */
        int max_queued = num_samples <= 256 ? 12 : 3;
        if ((int)state.BuffersQueued >= max_queued) { g_xa2_drops++; return 0; }
    }

    idx = g_xa2_next_buf;
    copy_samples = (num_samples > XA2_BUF_SAMPLES) ? XA2_BUF_SAMPLES : num_samples;
    memcpy(g_xa2_bufs[idx], samples, copy_samples * XA2_CHANNELS * sizeof(int16_t));

    memset(&xbuf, 0, sizeof(xbuf));
    xbuf.AudioBytes = copy_samples * XA2_CHANNELS * sizeof(int16_t);
    xbuf.pAudioData = (const BYTE *)g_xa2_bufs[idx];

    hr = IXAudio2SourceVoice_SubmitSourceBuffer(g_xa2_source, &xbuf, NULL);
    if (FAILED(hr)) return 0;

    g_xa2_next_buf = (idx + 1) % XA2_NUM_BUFS;
    g_xa2_frames_written++;
    return 1;
}

/* Output health since the last call: buffers that found the queue empty
 * (the device ran dry -- an audible gap), buffers dropped because the queue
 * was full, the lowest queue depth seen, submits that waited for room and
 * silence refills of a (nearly) empty queue. */
void xa2_take_stats(int *underruns, int *drops, int *min_queued, int *waits, int *refills)
{
    *waits = g_xa2_waits;
    *refills = g_xa2_refills;
    g_xa2_waits = g_xa2_refills = 0;
    *underruns = g_xa2_underruns;
    *drops = g_xa2_drops;
    *min_queued = g_xa2_min_queued == 1 << 30 ? -1 : g_xa2_min_queued;
    g_xa2_underruns = g_xa2_drops = 0;
    g_xa2_min_queued = 1 << 30;
}

/* Buffers queued for the device now (0 without XAudio2). */
int xa2_queued(void)
{
    XAUDIO2_VOICE_STATE state;
    if (!g_xa2_initialized || !g_xa2_source)
        return 0;
    IXAudio2SourceVoice_GetState(g_xa2_source, &state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    return (int)state.BuffersQueued;
}

int xa2_get_buffer_size(void)
{
    return XA2_BUF_SAMPLES;
}

#else /* !_WIN32 -- SDL2 audio queue */

#include <SDL.h>

#define XA2_SAMPLE_RATE   48000
#define XA2_CHANNELS      2
#define XA2_BUF_SAMPLES   1024

static SDL_AudioDeviceID g_sdl_dev;
static int g_sdl_frames_written;
static int g_sdl_buf_samples = 256;   /* size of the last submit: the unit of "buffers queued" */
static int g_sdl_underruns, g_sdl_drops, g_sdl_refills;
static int g_sdl_min_queued = 1 << 30;

int xa2_init(void)
{
    SDL_AudioSpec want, have;

    if (g_sdl_dev) return 1;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "[XA2] SDL audio init failed: %s\n", SDL_GetError());
        return 0;
    }
    SDL_zero(want);
    want.freq     = XA2_SAMPLE_RATE;
    want.format   = AUDIO_S16LSB;
    want.channels = XA2_CHANNELS;
    want.samples  = 512;              /* device period, ~11 ms */
    want.callback = NULL;             /* queue mode: SDL_QueueAudio */
    g_sdl_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!g_sdl_dev) {
        fprintf(stderr, "[XA2] SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return 0;
    }
    SDL_PauseAudioDevice(g_sdl_dev, 0);
    g_sdl_frames_written = 0;
    fprintf(stderr, "[XA2] SDL audio initialized (%s, %d Hz stereo 16-bit, period %d)\n",
            SDL_GetCurrentAudioDriver(), have.freq, have.samples);
    return 1;
}

void xa2_shutdown(void)
{
    if (!g_sdl_dev) return;
    SDL_CloseAudioDevice(g_sdl_dev);
    g_sdl_dev = 0;
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    fprintf(stderr, "[XA2] Shut down (%d frames written)\n", g_sdl_frames_written);
}

int xa2_is_active(void)
{
    return g_sdl_dev != 0;
}

static int sdl_queued(int buf_samples)
{
    return (int)(SDL_GetQueuedAudioSize(g_sdl_dev) / (buf_samples * XA2_CHANNELS * sizeof(int16_t)));
}

/* Same queue policy as the XAudio2 path above: a silence cushion of 8
 * buffers after running (nearly) dry, drop when 12 (3 large) are queued. */
int xa2_submit_samples(const int16_t *samples, int num_samples)
{
    int queued, max_queued;

    if (!g_sdl_dev) return 0;
    if (num_samples > XA2_BUF_SAMPLES) num_samples = XA2_BUF_SAMPLES;
    g_sdl_buf_samples = num_samples;
    queued = sdl_queued(num_samples);
    if (queued == 0 && g_sdl_frames_written)
        g_sdl_underruns++;
    if (num_samples <= 256 && queued <= 1) {
        static const int16_t silence[256 * 7][2];
        SDL_QueueAudio(g_sdl_dev, silence,
                       (Uint32)((7 - queued) * num_samples * XA2_CHANNELS * sizeof(int16_t)));
        if (g_sdl_frames_written)
            g_sdl_refills++;
        queued = 7;
    }
    if (queued < g_sdl_min_queued)
        g_sdl_min_queued = queued;
    max_queued = num_samples <= 256 ? 12 : 3;
    if (queued >= max_queued) { g_sdl_drops++; return 0; }
    if (SDL_QueueAudio(g_sdl_dev, samples,
                       (Uint32)(num_samples * XA2_CHANNELS * sizeof(int16_t))) != 0)
        return 0;
    g_sdl_frames_written++;
    return 1;
}

void xa2_take_stats(int *underruns, int *drops, int *min_queued, int *waits, int *refills)
{
    *waits = 0;
    *refills = g_sdl_refills;
    *underruns = g_sdl_underruns;
    *drops = g_sdl_drops;
    *min_queued = g_sdl_min_queued == 1 << 30 ? -1 : g_sdl_min_queued;
    g_sdl_underruns = g_sdl_drops = g_sdl_refills = 0;
    g_sdl_min_queued = 1 << 30;
}

int xa2_queued(void)
{
    return g_sdl_dev ? sdl_queued(g_sdl_buf_samples) : 0;
}

int xa2_get_buffer_size(void)
{
    return XA2_BUF_SAMPLES;
}

#endif /* _WIN32 */
