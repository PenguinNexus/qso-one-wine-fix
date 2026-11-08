/*
 * MQ.dll - Media Foundation audio-capture shim for QSO One under Wine.
 *
 * Wine's mf.dll enumerates audio capture devices but cannot open them
 * (MFCreateDeviceSource returns E_NOTIMPL for MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_AUDCAP_GUID).
 * record_windows_plugin.dll is patched to import "MQ.dll" instead of "MF.dll".
 * Every export is forwarded to Wine's mf.dll (see mq.def) except MFCreateDeviceSource,
 * which tries the real implementation first and, for audio capture only, falls back to
 * a small live IMFMediaSource built on WASAPI (IAudioClient), which Wine does implement.
 *
 * Environment:
 *   QSO_MQ_FORCE=1  always use the WASAPI source for audio capture (skip Wine's mf.dll)
 *   QSO_MQ_DEBUG=1  log to stderr
 */
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <stdio.h>
#include <stdarg.h>

/* KSDATAFORMAT_SUBTYPE_IEEE_FLOAT; mingw has no import library for it. */
DEFINE_GUID(qso_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, 0x00000003, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);

/* Lets qso-one-wine-fix.sh recognise its own MQ.dll. */
__attribute__((used)) static const char marker[] = "QSO-WINE-FIX MQ capture shim";

#define BUFFER_DURATION_HNS  1000000   /* 100 ms WASAPI buffer */
#define MIN_CHUNK_MS         10
#define MAX_CHUNK_MS         40
#define MAX_BUFFERED_MS      1000
#define MAX_TOKENS           64

static int debug_enabled = -1;

static void trace(const char *fmt, ...)
{
    char buf[512];
    va_list args;

    if (debug_enabled < 0)
    {
        char v[8];
        debug_enabled = GetEnvironmentVariableA("QSO_MQ_DEBUG", v, sizeof(v)) && v[0] == '1';
    }
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    OutputDebugStringA(buf);
    if (debug_enabled)
    {
        fputs(buf, stderr);
        fflush(stderr);
    }
}

static const UINT32 native_rates[] = { 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 96000, 88200, 64000 };

enum source_state { STATE_STOPPED, STATE_STARTED, STATE_PAUSED, STATE_SHUTDOWN };

struct capture_format
{
    BOOL is_float;
    UINT32 bits, channels, rate, block_align;
};

struct source;

struct stream
{
    IMFMediaStream IMFMediaStream_iface;
    LONG refcount;
    IMFMediaEventQueue *queue;
    IMFStreamDescriptor *sd;
    struct source *source;
    BOOL active;
    IUnknown *tokens[MAX_TOKENS];
    unsigned int token_count;
};

struct source
{
    IMFMediaSource IMFMediaSource_iface;
    LONG refcount;
    IMFMediaEventQueue *queue;
    IMFPresentationDescriptor *pd;
    struct stream *stream;
    CRITICAL_SECTION op_cs;    /* serialises Start/Stop/Pause/Shutdown */
    CRITICAL_SECTION cs;       /* protects state, buffer and tokens */
    enum source_state state;

    IMMDevice *device;
    IAudioClient *client;
    IAudioCaptureClient *capture;
    HANDLE thread, stop_event, audio_event;

    struct capture_format target;   /* what we deliver */
    struct capture_format device_fmt; /* what WASAPI gives us */
    BOOL convert;
    double resample_pos;            /* fallback converter state */
    float last_frame[2];

    BYTE *buf;
    SIZE_T buf_len, buf_cap;
    LONGLONG frames_out;
};

static inline struct source *impl_from_IMFMediaSource(IMFMediaSource *iface)
{
    return CONTAINING_RECORD(iface, struct source, IMFMediaSource_iface);
}

static inline struct stream *impl_from_IMFMediaStream(IMFMediaStream *iface)
{
    return CONTAINING_RECORD(iface, struct stream, IMFMediaStream_iface);
}

/* --- buffering and delivery --------------------------------------------- */

static void buffer_append(struct source *source, const BYTE *data, SIZE_T len)
{
    SIZE_T max_len = (SIZE_T)source->target.block_align * source->target.rate * MAX_BUFFERED_MS / 1000;

    if (len > max_len)
    {
        data += len - max_len;
        len = max_len;
    }
    if (source->buf_len + len > max_len)
    {
        /* Nobody is reading: drop the oldest audio, whole frames only. */
        SIZE_T ba = source->target.block_align;
        SIZE_T drop = (source->buf_len + len - max_len + ba - 1) / ba * ba;
        if (drop > source->buf_len) drop = source->buf_len;
        memmove(source->buf, source->buf + drop, source->buf_len - drop);
        source->buf_len -= drop;
    }
    if (source->buf_len + len > source->buf_cap)
    {
        SIZE_T cap = max(source->buf_len + len, max_len + source->target.block_align);
        BYTE *nb = realloc(source->buf, cap);
        if (!nb) return;
        source->buf = nb;
        source->buf_cap = cap;
    }
    if (data)
        memcpy(source->buf + source->buf_len, data, len);
    else
        memset(source->buf + source->buf_len, 0, len);
    source->buf_len += len;
}

/* Called with source->cs held. */
static void deliver_samples(struct source *source)
{
    struct stream *stream = source->stream;
    SIZE_T bytes_per_ms = (SIZE_T)source->target.block_align * source->target.rate / 1000;
    SIZE_T min_chunk = max(bytes_per_ms * MIN_CHUNK_MS, source->target.block_align);
    SIZE_T max_chunk = bytes_per_ms * MAX_CHUNK_MS;

    if (source->state != STATE_STARTED)
        return;

    while (stream->token_count && source->buf_len >= min_chunk)
    {
        SIZE_T len = min(source->buf_len, max_chunk);
        UINT32 frames;
        IUnknown *token = stream->tokens[0];
        IMFMediaBuffer *buffer = NULL;
        IMFSample *sample = NULL;
        BYTE *dst;
        HRESULT hr;

        len -= len % source->target.block_align;
        frames = len / source->target.block_align;

        hr = MFCreateMemoryBuffer(len, &buffer);
        if (SUCCEEDED(hr) && SUCCEEDED(hr = IMFMediaBuffer_Lock(buffer, &dst, NULL, NULL)))
        {
            memcpy(dst, source->buf, len);
            IMFMediaBuffer_Unlock(buffer);
            hr = IMFMediaBuffer_SetCurrentLength(buffer, len);
        }
        if (SUCCEEDED(hr)) hr = MFCreateSample(&sample);
        if (SUCCEEDED(hr)) hr = IMFSample_AddBuffer(sample, buffer);
        if (SUCCEEDED(hr)) hr = IMFSample_SetSampleTime(sample, source->frames_out * 10000000 / source->target.rate);
        if (SUCCEEDED(hr)) hr = IMFSample_SetSampleDuration(sample, (LONGLONG)frames * 10000000 / source->target.rate);
        if (SUCCEEDED(hr) && token) hr = IMFSample_SetUnknown(sample, &MFSampleExtension_Token, token);
        if (SUCCEEDED(hr))
            hr = IMFMediaEventQueue_QueueEventParamUnk(stream->queue, MEMediaSample, &GUID_NULL, S_OK, (IUnknown *)sample);

        if (buffer) IMFMediaBuffer_Release(buffer);
        if (sample) IMFSample_Release(sample);
        if (FAILED(hr))
        {
            trace("qso-mq: failed to deliver sample, hr %#lx\n", hr);
            return;
        }

        memmove(source->buf, source->buf + len, source->buf_len - len);
        source->buf_len -= len;
        source->frames_out += frames;

        if (token) IUnknown_Release(token);
        memmove(stream->tokens, stream->tokens + 1, (stream->token_count - 1) * sizeof(*stream->tokens));
        stream->token_count--;
    }
}

static void clear_tokens(struct stream *stream)
{
    while (stream->token_count)
    {
        IUnknown *token = stream->tokens[--stream->token_count];
        if (token) IUnknown_Release(token);
    }
}

/* --- format conversion (only used if WASAPI refuses the requested format) -- */

static float read_device_sample(const struct capture_format *fmt, const BYTE *p)
{
    if (fmt->is_float)
        return fmt->bits == 64 ? (float)*(const double *)p : *(const float *)p;
    switch (fmt->bits)
    {
        case 8:  return ((int)p[0] - 128) / 128.0f;
        case 16: return *(const INT16 *)p / 32768.0f;
        case 24: return (INT32)((p[0] << 8) | (p[1] << 16) | ((UINT32)p[2] << 24)) / 2147483648.0f;
        default: return *(const INT32 *)p / 2147483648.0f;
    }
}

static void read_device_frame(const struct source *source, const BYTE *frame, float out[2])
{
    const struct capture_format *fmt = &source->device_fmt;
    UINT32 bytes = fmt->bits / 8, c;
    float sum = 0.0f;

    if (source->target.channels == 1)
    {
        for (c = 0; c < fmt->channels; ++c)
            sum += read_device_sample(fmt, frame + c * bytes);
        out[0] = out[1] = sum / fmt->channels;
    }
    else
    {
        out[0] = read_device_sample(fmt, frame);
        out[1] = fmt->channels > 1 ? read_device_sample(fmt, frame + bytes) : out[0];
    }
}

static void write_target_frame(const struct capture_format *fmt, const float in[2], BYTE *out)
{
    UINT32 c;

    for (c = 0; c < fmt->channels; ++c)
    {
        float v = in[c];
        if (fmt->is_float)
            ((float *)out)[c] = v;
        else
        {
            if (v > 1.0f) v = 1.0f;
            if (v < -1.0f) v = -1.0f;
            ((INT16 *)out)[c] = (INT16)(v * 32767.0f);
        }
    }
}

/* Called with source->cs held. Linear resampling; good enough as a last resort. */
static void convert_and_append(struct source *source, const BYTE *data, UINT32 frames, BOOL silent)
{
    double step = (double)source->device_fmt.rate / source->target.rate;
    BYTE out[64 * 1024];
    SIZE_T out_len = 0;
    UINT32 i;

    for (i = 0; i < frames; ++i)
    {
        float cur[2] = { 0.0f, 0.0f };

        if (!silent)
            read_device_frame(source, data + (SIZE_T)i * source->device_fmt.block_align, cur);

        while (source->resample_pos <= 1.0)
        {
            float t = (float)source->resample_pos, mix[2];
            mix[0] = source->last_frame[0] + (cur[0] - source->last_frame[0]) * t;
            mix[1] = source->last_frame[1] + (cur[1] - source->last_frame[1]) * t;
            write_target_frame(&source->target, mix, out + out_len);
            out_len += source->target.block_align;
            if (out_len + source->target.block_align > sizeof(out))
            {
                buffer_append(source, out, out_len);
                out_len = 0;
            }
            source->resample_pos += step;
        }
        source->resample_pos -= 1.0;
        source->last_frame[0] = cur[0];
        source->last_frame[1] = cur[1];
    }
    if (out_len)
        buffer_append(source, out, out_len);
}

/* --- WASAPI capture ------------------------------------------------------- */

static void parse_waveformat(const WAVEFORMATEX *wfx, struct capture_format *fmt)
{
    fmt->is_float = wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wfx->cbSize >= 22)
        fmt->is_float = IsEqualGUID(&((const WAVEFORMATEXTENSIBLE *)wfx)->SubFormat, &qso_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    fmt->bits = wfx->wBitsPerSample;
    fmt->channels = wfx->nChannels;
    fmt->rate = wfx->nSamplesPerSec;
    fmt->block_align = wfx->nBlockAlign;
}

static DWORD WINAPI capture_thread(void *arg)
{
    struct source *source = arg;
    HANDLE handles[2] = { source->stop_event, source->audio_event };

    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    for (;;)
    {
        DWORD ret = WaitForMultipleObjects(2, handles, FALSE, 20);
        UINT32 packet = 0;

        if (ret == WAIT_OBJECT_0)
            break;

        EnterCriticalSection(&source->cs);
        while (source->capture && SUCCEEDED(IAudioCaptureClient_GetNextPacketSize(source->capture, &packet)) && packet)
        {
            BYTE *data;
            UINT32 frames;
            DWORD flags;

            if (FAILED(IAudioCaptureClient_GetBuffer(source->capture, &data, &frames, &flags, NULL, NULL)))
                break;
            if (source->state == STATE_STARTED)
            {
                BOOL silent = !!(flags & AUDCLNT_BUFFERFLAGS_SILENT);
                if (source->convert)
                    convert_and_append(source, data, frames, silent);
                else
                    buffer_append(source, silent ? NULL : data, (SIZE_T)frames * source->target.block_align);
            }
            IAudioCaptureClient_ReleaseBuffer(source->capture, frames);
        }
        deliver_samples(source);
        LeaveCriticalSection(&source->cs);
    }

    CoUninitialize();
    return 0;
}

static void release_client(struct source *source)
{
    if (source->capture) IAudioCaptureClient_Release(source->capture);
    if (source->client)
    {
        IAudioClient_Stop(source->client);
        IAudioClient_Release(source->client);
    }
    source->capture = NULL;
    source->client = NULL;
}

/* Called with op_cs held, capture thread not running. */
static HRESULT init_client(struct source *source)
{
    IMFMediaTypeHandler *handler = NULL;
    IMFMediaType *type = NULL;
    WAVEFORMATEX wfx = { 0 }, *mix = NULL;
    GUID subtype;
    HRESULT hr;

    hr = IMFStreamDescriptor_GetMediaTypeHandler(source->stream->sd, &handler);
    if (SUCCEEDED(hr) && FAILED(IMFMediaTypeHandler_GetCurrentMediaType(handler, &type)))
        hr = IMFMediaTypeHandler_GetMediaTypeByIndex(handler, 0, &type);
    if (handler) IMFMediaTypeHandler_Release(handler);
    if (FAILED(hr)) return hr;

    memset(&source->target, 0, sizeof(source->target));
    IMFMediaType_GetGUID(type, &MF_MT_SUBTYPE, &subtype);
    IMFMediaType_GetUINT32(type, &MF_MT_AUDIO_NUM_CHANNELS, &source->target.channels);
    IMFMediaType_GetUINT32(type, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &source->target.rate);
    IMFMediaType_GetUINT32(type, &MF_MT_AUDIO_BITS_PER_SAMPLE, &source->target.bits);
    IMFMediaType_Release(type);
    source->target.is_float = IsEqualGUID(&subtype, &MFAudioFormat_Float);
    source->target.block_align = source->target.channels * source->target.bits / 8;

    wfx.wFormatTag = source->target.is_float ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
    wfx.nChannels = source->target.channels;
    wfx.nSamplesPerSec = source->target.rate;
    wfx.wBitsPerSample = source->target.bits;
    wfx.nBlockAlign = source->target.block_align;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    release_client(source);
    if (FAILED(hr = IMMDevice_Activate(source->device, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&source->client)))
        return hr;

    source->convert = FALSE;
    source->device_fmt = source->target;
    hr = IAudioClient_Initialize(source->client, AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
            BUFFER_DURATION_HNS, 0, &wfx, NULL);
    if (FAILED(hr))
    {
        trace("qso-mq: WASAPI refused %u Hz/%u ch/%u bit (hr %#lx), converting from mix format\n",
                wfx.nSamplesPerSec, wfx.nChannels, wfx.wBitsPerSample, hr);
        IAudioClient_Release(source->client);
        if (FAILED(hr = IMMDevice_Activate(source->device, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&source->client)))
        {
            source->client = NULL;
            return hr;
        }
        if (SUCCEEDED(hr = IAudioClient_GetMixFormat(source->client, &mix)))
        {
            parse_waveformat(mix, &source->device_fmt);
            hr = IAudioClient_Initialize(source->client, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                    BUFFER_DURATION_HNS, 0, mix, NULL);
            CoTaskMemFree(mix);
            source->convert = TRUE;
            source->resample_pos = 0.0;
            source->last_frame[0] = source->last_frame[1] = 0.0f;
        }
    }
    if (SUCCEEDED(hr)) hr = IAudioClient_SetEventHandle(source->client, source->audio_event);
    if (SUCCEEDED(hr)) hr = IAudioClient_GetService(source->client, &IID_IAudioCaptureClient, (void **)&source->capture);
    if (FAILED(hr))
    {
        release_client(source);
        return hr;
    }

    trace("qso-mq: capture opened: deliver %u Hz/%u ch/%u bit%s, device %u Hz/%u ch/%u bit%s%s\n",
            source->target.rate, source->target.channels, source->target.bits, source->target.is_float ? " float" : "",
            source->device_fmt.rate, source->device_fmt.channels, source->device_fmt.bits,
            source->device_fmt.is_float ? " float" : "", source->convert ? " (converting)" : "");
    return S_OK;
}

static void stop_thread(struct source *source)
{
    if (!source->thread) return;
    SetEvent(source->stop_event);
    WaitForSingleObject(source->thread, INFINITE);
    CloseHandle(source->thread);
    source->thread = NULL;
    ResetEvent(source->stop_event);
}

/* --- IMFMediaStream ------------------------------------------------------- */

static HRESULT WINAPI stream_QueryInterface(IMFMediaStream *iface, REFIID riid, void **out)
{
    if (IsEqualIID(riid, &IID_IMFMediaStream) || IsEqualIID(riid, &IID_IMFMediaEventGenerator) ||
            IsEqualIID(riid, &IID_IUnknown))
    {
        *out = iface;
        IMFMediaStream_AddRef(iface);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI stream_AddRef(IMFMediaStream *iface)
{
    return InterlockedIncrement(&impl_from_IMFMediaStream(iface)->refcount);
}

static ULONG WINAPI stream_Release(IMFMediaStream *iface)
{
    struct stream *stream = impl_from_IMFMediaStream(iface);
    ULONG refcount = InterlockedDecrement(&stream->refcount);

    if (!refcount)
    {
        clear_tokens(stream);
        if (stream->queue) IMFMediaEventQueue_Release(stream->queue);
        if (stream->sd) IMFStreamDescriptor_Release(stream->sd);
        free(stream);
    }
    return refcount;
}

static HRESULT WINAPI stream_GetEvent(IMFMediaStream *iface, DWORD flags, IMFMediaEvent **event)
{
    return IMFMediaEventQueue_GetEvent(impl_from_IMFMediaStream(iface)->queue, flags, event);
}

static HRESULT WINAPI stream_BeginGetEvent(IMFMediaStream *iface, IMFAsyncCallback *callback, IUnknown *state)
{
    return IMFMediaEventQueue_BeginGetEvent(impl_from_IMFMediaStream(iface)->queue, callback, state);
}

static HRESULT WINAPI stream_EndGetEvent(IMFMediaStream *iface, IMFAsyncResult *result, IMFMediaEvent **event)
{
    return IMFMediaEventQueue_EndGetEvent(impl_from_IMFMediaStream(iface)->queue, result, event);
}

static HRESULT WINAPI stream_QueueEvent(IMFMediaStream *iface, MediaEventType type, REFGUID ext_type,
        HRESULT hr, const PROPVARIANT *value)
{
    return IMFMediaEventQueue_QueueEventParamVar(impl_from_IMFMediaStream(iface)->queue, type, ext_type, hr, value);
}

static HRESULT WINAPI stream_GetMediaSource(IMFMediaStream *iface, IMFMediaSource **out)
{
    struct stream *stream = impl_from_IMFMediaStream(iface);
    struct source *source = stream->source;
    HRESULT hr = S_OK;

    if (!source) return MF_E_SHUTDOWN;
    EnterCriticalSection(&source->cs);
    if (source->state == STATE_SHUTDOWN)
        hr = MF_E_SHUTDOWN;
    else
    {
        *out = &source->IMFMediaSource_iface;
        IMFMediaSource_AddRef(*out);
    }
    LeaveCriticalSection(&source->cs);
    return hr;
}

static HRESULT WINAPI stream_GetStreamDescriptor(IMFMediaStream *iface, IMFStreamDescriptor **sd)
{
    struct stream *stream = impl_from_IMFMediaStream(iface);

    *sd = stream->sd;
    IMFStreamDescriptor_AddRef(*sd);
    return S_OK;
}

static HRESULT WINAPI stream_RequestSample(IMFMediaStream *iface, IUnknown *token)
{
    struct stream *stream = impl_from_IMFMediaStream(iface);
    struct source *source = stream->source;
    HRESULT hr = S_OK;

    if (!source) return MF_E_SHUTDOWN;
    EnterCriticalSection(&source->cs);
    if (source->state == STATE_SHUTDOWN)
        hr = MF_E_SHUTDOWN;
    else if (source->state == STATE_STOPPED)
        hr = MF_E_MEDIA_SOURCE_WRONGSTATE;
    else if (stream->token_count == MAX_TOKENS)
        hr = MF_E_SAMPLEALLOCATOR_EMPTY;
    else
    {
        if (token) IUnknown_AddRef(token);
        stream->tokens[stream->token_count++] = token;
        deliver_samples(source);
    }
    LeaveCriticalSection(&source->cs);
    return hr;
}

static IMFMediaStreamVtbl stream_vtbl =
{
    .QueryInterface = stream_QueryInterface,
    .AddRef = stream_AddRef,
    .Release = stream_Release,
    .GetEvent = stream_GetEvent,
    .BeginGetEvent = stream_BeginGetEvent,
    .EndGetEvent = stream_EndGetEvent,
    .QueueEvent = stream_QueueEvent,
    .GetMediaSource = stream_GetMediaSource,
    .GetStreamDescriptor = stream_GetStreamDescriptor,
    .RequestSample = stream_RequestSample,
};

/* --- IMFMediaSource ------------------------------------------------------- */

static HRESULT WINAPI source_QueryInterface(IMFMediaSource *iface, REFIID riid, void **out)
{
    if (IsEqualIID(riid, &IID_IMFMediaSource) || IsEqualIID(riid, &IID_IMFMediaEventGenerator) ||
            IsEqualIID(riid, &IID_IUnknown))
    {
        *out = iface;
        IMFMediaSource_AddRef(iface);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI source_AddRef(IMFMediaSource *iface)
{
    return InterlockedIncrement(&impl_from_IMFMediaSource(iface)->refcount);
}

static ULONG WINAPI source_Release(IMFMediaSource *iface)
{
    struct source *source = impl_from_IMFMediaSource(iface);
    ULONG refcount = InterlockedDecrement(&source->refcount);

    if (!refcount)
    {
        stop_thread(source);
        release_client(source);
        if (source->stream)
        {
            source->stream->source = NULL;
            IMFMediaStream_Release(&source->stream->IMFMediaStream_iface);
        }
        if (source->queue) IMFMediaEventQueue_Release(source->queue);
        if (source->pd) IMFPresentationDescriptor_Release(source->pd);
        if (source->device) IMMDevice_Release(source->device);
        if (source->stop_event) CloseHandle(source->stop_event);
        if (source->audio_event) CloseHandle(source->audio_event);
        DeleteCriticalSection(&source->cs);
        DeleteCriticalSection(&source->op_cs);
        free(source->buf);
        free(source);
    }
    return refcount;
}

static HRESULT WINAPI source_GetEvent(IMFMediaSource *iface, DWORD flags, IMFMediaEvent **event)
{
    return IMFMediaEventQueue_GetEvent(impl_from_IMFMediaSource(iface)->queue, flags, event);
}

static HRESULT WINAPI source_BeginGetEvent(IMFMediaSource *iface, IMFAsyncCallback *callback, IUnknown *state)
{
    return IMFMediaEventQueue_BeginGetEvent(impl_from_IMFMediaSource(iface)->queue, callback, state);
}

static HRESULT WINAPI source_EndGetEvent(IMFMediaSource *iface, IMFAsyncResult *result, IMFMediaEvent **event)
{
    return IMFMediaEventQueue_EndGetEvent(impl_from_IMFMediaSource(iface)->queue, result, event);
}

static HRESULT WINAPI source_QueueEvent(IMFMediaSource *iface, MediaEventType type, REFGUID ext_type,
        HRESULT hr, const PROPVARIANT *value)
{
    return IMFMediaEventQueue_QueueEventParamVar(impl_from_IMFMediaSource(iface)->queue, type, ext_type, hr, value);
}

static HRESULT WINAPI source_GetCharacteristics(IMFMediaSource *iface, DWORD *flags)
{
    struct source *source = impl_from_IMFMediaSource(iface);

    if (source->state == STATE_SHUTDOWN) return MF_E_SHUTDOWN;
    *flags = MFMEDIASOURCE_IS_LIVE | MFMEDIASOURCE_CAN_PAUSE;
    return S_OK;
}

static HRESULT WINAPI source_CreatePresentationDescriptor(IMFMediaSource *iface, IMFPresentationDescriptor **pd)
{
    struct source *source = impl_from_IMFMediaSource(iface);

    if (source->state == STATE_SHUTDOWN) return MF_E_SHUTDOWN;
    return IMFPresentationDescriptor_Clone(source->pd, pd);
}

static HRESULT WINAPI source_Start(IMFMediaSource *iface, IMFPresentationDescriptor *pd, const GUID *time_format,
        const PROPVARIANT *position)
{
    struct source *source = impl_from_IMFMediaSource(iface);
    struct stream *stream = source->stream;
    PROPVARIANT empty;
    HRESULT hr = S_OK;

    if (time_format && !IsEqualGUID(time_format, &GUID_NULL))
        return MF_E_UNSUPPORTED_TIME_FORMAT;
    PropVariantInit(&empty);
    if (!position) position = &empty;

    EnterCriticalSection(&source->op_cs);

    if (source->state == STATE_SHUTDOWN)
        hr = MF_E_SHUTDOWN;
    else if (source->state == STATE_STOPPED)
    {
        if (SUCCEEDED(hr = init_client(source)))
        {
            source->buf_len = 0;
            source->frames_out = 0;
            ResetEvent(source->stop_event);
            if (!(source->thread = CreateThread(NULL, 0, capture_thread, source, 0, NULL)))
                hr = HRESULT_FROM_WIN32(GetLastError());
        }
    }
    if (SUCCEEDED(hr))
        hr = IAudioClient_Start(source->client);
    if (hr == AUDCLNT_E_NOT_STOPPED)
        hr = S_OK;

    if (SUCCEEDED(hr))
    {
        EnterCriticalSection(&source->cs);
        source->state = STATE_STARTED;
        IMFMediaEventQueue_QueueEventParamUnk(source->queue, stream->active ? MEUpdatedStream : MENewStream,
                &GUID_NULL, S_OK, (IUnknown *)&stream->IMFMediaStream_iface);
        stream->active = TRUE;
        IMFMediaEventQueue_QueueEventParamVar(source->queue, MESourceStarted, &GUID_NULL, S_OK, position);
        IMFMediaEventQueue_QueueEventParamVar(stream->queue, MEStreamStarted, &GUID_NULL, S_OK, position);
        LeaveCriticalSection(&source->cs);
    }
    else
    {
        trace("qso-mq: Start failed, hr %#lx\n", hr);
        if (source->state == STATE_STOPPED)
        {
            stop_thread(source);
            release_client(source);
        }
    }

    LeaveCriticalSection(&source->op_cs);
    return hr;
}

static HRESULT WINAPI source_Stop(IMFMediaSource *iface)
{
    struct source *source = impl_from_IMFMediaSource(iface);
    struct stream *stream = source->stream;
    HRESULT hr = S_OK;

    EnterCriticalSection(&source->op_cs);
    if (source->state == STATE_SHUTDOWN)
        hr = MF_E_SHUTDOWN;
    else
    {
        stop_thread(source);
        release_client(source);

        EnterCriticalSection(&source->cs);
        source->state = STATE_STOPPED;
        source->buf_len = 0;
        clear_tokens(stream);
        if (stream->active)
            IMFMediaEventQueue_QueueEventParamVar(stream->queue, MEStreamStopped, &GUID_NULL, S_OK, NULL);
        IMFMediaEventQueue_QueueEventParamVar(source->queue, MESourceStopped, &GUID_NULL, S_OK, NULL);
        LeaveCriticalSection(&source->cs);
    }
    LeaveCriticalSection(&source->op_cs);
    return hr;
}

static HRESULT WINAPI source_Pause(IMFMediaSource *iface)
{
    struct source *source = impl_from_IMFMediaSource(iface);
    struct stream *stream = source->stream;
    HRESULT hr = S_OK;

    EnterCriticalSection(&source->op_cs);
    if (source->state == STATE_SHUTDOWN)
        hr = MF_E_SHUTDOWN;
    else if (source->state != STATE_STARTED)
        hr = MF_E_INVALID_STATE_TRANSITION;
    else
    {
        IAudioClient_Stop(source->client);
        IAudioClient_Reset(source->client);

        EnterCriticalSection(&source->cs);
        source->state = STATE_PAUSED;
        source->buf_len = 0;
        IMFMediaEventQueue_QueueEventParamVar(stream->queue, MEStreamPaused, &GUID_NULL, S_OK, NULL);
        IMFMediaEventQueue_QueueEventParamVar(source->queue, MESourcePaused, &GUID_NULL, S_OK, NULL);
        LeaveCriticalSection(&source->cs);
    }
    LeaveCriticalSection(&source->op_cs);
    return hr;
}

static HRESULT WINAPI source_Shutdown(IMFMediaSource *iface)
{
    struct source *source = impl_from_IMFMediaSource(iface);
    struct stream *stream = source->stream;

    EnterCriticalSection(&source->op_cs);
    if (source->state == STATE_SHUTDOWN)
    {
        LeaveCriticalSection(&source->op_cs);
        return MF_E_SHUTDOWN;
    }
    stop_thread(source);
    release_client(source);

    EnterCriticalSection(&source->cs);
    source->state = STATE_SHUTDOWN;
    clear_tokens(stream);
    LeaveCriticalSection(&source->cs);

    IMFMediaEventQueue_Shutdown(stream->queue);
    IMFMediaEventQueue_Shutdown(source->queue);
    LeaveCriticalSection(&source->op_cs);
    trace("qso-mq: source %p shut down\n", source);
    return S_OK;
}

static IMFMediaSourceVtbl source_vtbl =
{
    .QueryInterface = source_QueryInterface,
    .AddRef = source_AddRef,
    .Release = source_Release,
    .GetEvent = source_GetEvent,
    .BeginGetEvent = source_BeginGetEvent,
    .EndGetEvent = source_EndGetEvent,
    .QueueEvent = source_QueueEvent,
    .GetCharacteristics = source_GetCharacteristics,
    .CreatePresentationDescriptor = source_CreatePresentationDescriptor,
    .Start = source_Start,
    .Stop = source_Stop,
    .Pause = source_Pause,
    .Shutdown = source_Shutdown,
};

/* --- construction --------------------------------------------------------- */

/* Exactly major/subtype/bits/rate/channels: Wine's IsEqual treats the smaller
 * attribute set as a subset match, so the plugin's partial PCM request
 * (which also carries MF_MT_AVG_BITRATE) matches one of these. */
static HRESULT create_native_type(BOOL is_float, UINT32 rate, UINT32 channels, IMFMediaType **out)
{
    IMFMediaType *type;
    HRESULT hr;

    if (FAILED(hr = MFCreateMediaType(&type))) return hr;
    IMFMediaType_SetGUID(type, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
    IMFMediaType_SetGUID(type, &MF_MT_SUBTYPE, is_float ? &MFAudioFormat_Float : &MFAudioFormat_PCM);
    IMFMediaType_SetUINT32(type, &MF_MT_AUDIO_BITS_PER_SAMPLE, is_float ? 32 : 16);
    IMFMediaType_SetUINT32(type, &MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
    IMFMediaType_SetUINT32(type, &MF_MT_AUDIO_NUM_CHANNELS, channels);
    *out = type;
    return S_OK;
}

static HRESULT get_device(IMFAttributes *attributes, IMMDevice **device)
{
    IMMDeviceEnumerator *enumerator;
    WCHAR *id = NULL;
    UINT32 len, role = eConsole;
    HRESULT hr;

    if (FAILED(hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_INPROC_SERVER,
            &IID_IMMDeviceEnumerator, (void **)&enumerator)))
        return hr;

    if (SUCCEEDED(IMFAttributes_GetAllocatedString(attributes, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_AUDCAP_ENDPOINT_ID, &id, &len)))
    {
        hr = IMMDeviceEnumerator_GetDevice(enumerator, id, device);
        trace("qso-mq: endpoint %ls -> hr %#lx\n", id, hr);
        CoTaskMemFree(id);
    }
    else
    {
        IMFAttributes_GetUINT32(attributes, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_AUDCAP_ROLE, &role);
        hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator, eCapture, role, device);
        trace("qso-mq: default capture endpoint (role %u) -> hr %#lx\n", role, hr);
    }
    IMMDeviceEnumerator_Release(enumerator);
    return hr;
}

static HRESULT create_wasapi_source(IMFAttributes *attributes, IMFMediaSource **out)
{
    IMFMediaType *types[2 * 2 * ARRAYSIZE(native_rates)];
    IMFStreamDescriptor *sd = NULL;
    IAudioClient *probe;
    struct source *source;
    struct stream *stream;
    unsigned int count = 0, i, f, c;
    HRESULT hr;

    if (!(source = calloc(1, sizeof(*source))) || !(stream = calloc(1, sizeof(*stream))))
    {
        free(source);
        return E_OUTOFMEMORY;
    }
    source->IMFMediaSource_iface.lpVtbl = &source_vtbl;
    source->refcount = 1;
    InitializeCriticalSection(&source->op_cs);
    InitializeCriticalSection(&source->cs);
    source->state = STATE_STOPPED;
    source->stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    source->audio_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    stream->IMFMediaStream_iface.lpVtbl = &stream_vtbl;
    stream->refcount = 1;
    stream->source = source;
    source->stream = stream;

    hr = get_device(attributes, &source->device);
    /* Fail early, like the native source, if the endpoint cannot be opened. */
    if (SUCCEEDED(hr) && SUCCEEDED(hr = IMMDevice_Activate(source->device, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&probe)))
        IAudioClient_Release(probe);

    for (f = 0; SUCCEEDED(hr) && f < 2; ++f)
        for (i = 0; SUCCEEDED(hr) && i < ARRAYSIZE(native_rates); ++i)
            for (c = 1; SUCCEEDED(hr) && c <= 2; ++c)
                if (SUCCEEDED(hr = create_native_type(f, native_rates[i], c, &types[count])))
                    count++;

    if (SUCCEEDED(hr)) hr = MFCreateStreamDescriptor(0, count, types, &sd);
    if (SUCCEEDED(hr)) hr = MFCreatePresentationDescriptor(1, &sd, &source->pd);
    if (SUCCEEDED(hr)) hr = IMFPresentationDescriptor_SelectStream(source->pd, 0);
    if (SUCCEEDED(hr)) hr = MFCreateEventQueue(&source->queue);
    if (SUCCEEDED(hr)) hr = MFCreateEventQueue(&stream->queue);

    for (i = 0; i < count; ++i)
        IMFMediaType_Release(types[i]);
    stream->sd = sd;

    if (FAILED(hr))
    {
        trace("qso-mq: failed to create WASAPI source, hr %#lx\n", hr);
        IMFMediaSource_Release(&source->IMFMediaSource_iface);
        return hr;
    }

    *out = &source->IMFMediaSource_iface;
    return S_OK;
}

typedef HRESULT (WINAPI *create_device_source_func)(IMFAttributes *, IMFMediaSource **);

HRESULT WINAPI MFCreateDeviceSource(IMFAttributes *attributes, IMFMediaSource **source)
{
    static create_device_source_func real;
    char force[8];
    GUID type;
    HRESULT hr = E_NOTIMPL;

    if (!source) return E_POINTER;
    *source = NULL;

    if (!real)
    {
        HMODULE mf = LoadLibraryW(L"mf.dll");
        if (mf) real = (create_device_source_func)GetProcAddress(mf, "MFCreateDeviceSource");
    }

    if (!attributes || FAILED(IMFAttributes_GetGUID(attributes, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, &type)) ||
            !IsEqualGUID(&type, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_AUDCAP_GUID))
        return real ? real(attributes, source) : E_NOTIMPL;

    if (!(GetEnvironmentVariableA("QSO_MQ_FORCE", force, sizeof(force)) && force[0] == '1') && real)
    {
        if (SUCCEEDED(hr = real(attributes, source)))
            return hr;
        trace("qso-mq: mf.dll MFCreateDeviceSource failed (hr %#lx), using WASAPI source\n", hr);
    }

    hr = create_wasapi_source(attributes, source);
    trace("qso-mq: WASAPI source %p, hr %#lx\n", *source, hr);
    return hr;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(instance);
    return TRUE;
}
