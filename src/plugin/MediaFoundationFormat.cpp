#include "MediaFoundationFormat.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#if JUCE_WINDOWS
 #include <mfapi.h>
 #include <mferror.h>
 #include <mfidl.h>
 #include <mfreadwrite.h>
 #include <wrl/client.h>
using Microsoft::WRL::ComPtr;
#endif

namespace ada
{

namespace
{
    const char* const mfFormatName = "M4A/AAC (Media Foundation)";

    // --- MP4 box reading (ISO/IEC 14496-12) ------------------------------------------------
    struct Box
    {
        std::string type;
        juce::int64 payload = 0, end = 0; // payload: first byte after the header
    };

    std::optional<Box> readBox (juce::InputStream& in, juce::int64 pos, juce::int64 limit)
    {
        if (pos + 8 > limit || ! in.setPosition (pos))
            return std::nullopt;
        juce::int64 size = (juce::int64) (juce::uint32) in.readIntBigEndian();
        char type[4] = {};
        if (in.read (type, 4) != 4)
            return std::nullopt;
        juce::int64 header = 8;
        if (size == 1)
        {
            size = in.readInt64BigEndian();
            header = 16;
        }
        else if (size == 0) // extends to the end of the enclosing box
            size = limit - pos;
        if (size < header || size > limit - pos)
            return std::nullopt;
        return Box { std::string (type, 4), pos + header, pos + size };
    }

    template <typename Visit> // Visit (const Box&) -> bool: false stops
    void forEachBox (juce::InputStream& in, juce::int64 from, juce::int64 to, Visit visit)
    {
        for (auto pos = from; auto box = readBox (in, pos, to); pos = box->end)
            if (! visit (*box))
                return;
    }

    std::optional<Box> findBox (juce::InputStream& in, juce::int64 from, juce::int64 to, const char* type)
    {
        std::optional<Box> found;
        forEachBox (in, from, to, [&] (const Box& b) {
            if (b.type == type)
                found = b;
            return ! found;
        });
        return found;
    }

    // mvhd / mdhd: version, flags, two (v0: 32-bit, v1: 64-bit) times, then the time scale
    std::optional<double> readTimescale (juce::InputStream& in, const Box& b)
    {
        if (! in.setPosition (b.payload))
            return std::nullopt;
        const int version = in.readByte();
        in.skipNextBytes (3 + (version == 1 ? 16 : 8));
        const auto scale = (juce::uint32) in.readIntBigEndian();
        return scale > 0 ? std::optional<double> ((double) scale) : std::nullopt;
    }

    struct Edit
    {
        juce::int64 duration = 0;  // movie time scale
        juce::int64 mediaTime = 0; // media time scale
    };

    // The first non-empty edit (an empty edit, media time -1, would insert silence; ignored)
    std::optional<Edit> readFirstEdit (juce::InputStream& in, const Box& elst)
    {
        if (! in.setPosition (elst.payload))
            return std::nullopt;
        const int version = in.readByte();
        in.skipNextBytes (3);
        const auto count = (juce::uint32) in.readIntBigEndian();
        const juce::int64 entrySize = version == 1 ? 20 : 12;
        for (juce::uint32 i = 0; i < count && i < 64 && in.getPosition() + entrySize <= elst.end; ++i)
        {
            Edit e;
            if (version == 1)
            {
                e.duration = in.readInt64BigEndian();
                e.mediaTime = in.readInt64BigEndian();
            }
            else
            {
                e.duration = (juce::int64) (juce::uint32) in.readIntBigEndian();
                e.mediaTime = in.readIntBigEndian();
            }
            in.skipNextBytes (4); // media rate
            if (e.mediaTime >= 0)
                return e;
        }
        return std::nullopt;
    }

    // iTunes metadata: moov/udta/meta/ilst/----, a "name" child "iTunSMPB" and a "data" child
    // holding " 00000000 <priming> <padding> <length> ..." in hexadecimal
    std::optional<juce::String> readITunSmpb (juce::InputStream& in, const Box& moov)
    {
        std::optional<juce::String> value;
        const auto udta = findBox (in, moov.payload, moov.end, "udta");
        const auto meta = udta ? findBox (in, udta->payload, udta->end, "meta") : std::nullopt;
        if (! meta)
            return value;
        // ISO meta boxes start with version/flags; QuickTime-style ones (some Apple files) start
        // directly with their first child box, usually "hdlr"
        char probe[4] = {};
        const bool quickTime = in.setPosition (meta->payload + 4) && in.read (probe, 4) == 4 && std::string (probe, 4) == "hdlr";
        const auto ilst = findBox (in, meta->payload + (quickTime ? 0 : 4), meta->end, "ilst");
        if (! ilst)
            return value;
        forEachBox (in, ilst->payload, ilst->end, [&] (const Box& item) {
            if (item.type != "----")
                return true;
            juce::String name, data;
            forEachBox (in, item.payload, item.end, [&] (const Box& child) {
                const auto skip = child.type == "name" ? 4 : child.type == "data" ? 8 : -1; // version/flags (+ locale)
                const auto length = child.end - child.payload - skip;
                if (skip < 0 || length <= 0 || length > 1024 || ! in.setPosition (child.payload + skip))
                    return true;
                juce::MemoryBlock bytes;
                in.readIntoMemoryBlock (bytes, (juce::ssize_t) length);
                (child.type == "name" ? name : data) = bytes.toString();
                return true;
            });
            if (name == "iTunSMPB")
                value = data;
            return ! value;
        });
        return value;
    }
} // namespace

Mp4Timing readMp4Timing (juce::InputStream& in, double sampleRate)
{
    Mp4Timing timing;
    const auto total = in.getTotalLength();
    const auto moov = findBox (in, 0, total, "moov");
    if (! moov || sampleRate <= 0.0)
        return timing;

    std::optional<double> movieScale, mediaScale;
    std::optional<Edit> edit;
    bool haveAudioTrack = false;
    forEachBox (in, moov->payload, moov->end, [&] (const Box& b) {
        if (b.type == "mvhd")
            movieScale = readTimescale (in, b);
        else if (b.type == "trak" && ! haveAudioTrack)
        {
            const auto mdia = findBox (in, b.payload, b.end, "mdia");
            const auto hdlr = mdia ? findBox (in, mdia->payload, mdia->end, "hdlr") : std::nullopt;
            char handler[4] = {};
            if (! hdlr || ! in.setPosition (hdlr->payload + 8) || in.read (handler, 4) != 4
                || std::string (handler, 4) != "soun")
                return true;
            haveAudioTrack = true;
            if (const auto mdhd = findBox (in, mdia->payload, mdia->end, "mdhd"))
                mediaScale = readTimescale (in, *mdhd);
            if (const auto edts = findBox (in, b.payload, b.end, "edts"))
                if (const auto elst = findBox (in, edts->payload, edts->end, "elst"))
                    edit = readFirstEdit (in, *elst);
        }
        return true;
    });

    if (edit && movieScale && mediaScale)
    {
        // the edit list, as FFmpeg applies it
        timing.primingSamples = std::llround ((double) edit->mediaTime * sampleRate / *mediaScale);
        if (edit->duration > 0)
            timing.lengthSamples = std::llround ((double) edit->duration * sampleRate / *movieScale);
    }
    else if (const auto smpb = readITunSmpb (in, *moov))
    {
        const auto fields = juce::StringArray::fromTokens (smpb->trim(), " ", "");
        if (fields.size() >= 4)
        {
            const auto priming = fields[1].getHexValue64(), length = fields[3].getHexValue64();
            if (priming >= 0 && priming < 16384 && length > 0) // FFmpeg's limit
            {
                timing.primingSamples = priming;
                timing.lengthSamples = length;
            }
        }
    }
    timing.primingSamples = std::max<juce::int64> (0, timing.primingSamples);
    return timing;
}

#if JUCE_WINDOWS
namespace
{
    constexpr DWORD audioStream = (DWORD) MF_SOURCE_READER_FIRST_AUDIO_STREAM;

    // COM for the current thread (the file-analysis worker has none of its own), undone by the
    // destructor on the same thread. A thread that already has COM keeps its apartment
    // (CoInitializeEx then fails with RPC_E_CHANGED_MODE and nothing is undone).
    class ComScope
    {
    public:
        ComScope() = default;
        ComScope (const ComScope&) = delete;
        ComScope& operator= (const ComScope&) = delete;
        ~ComScope() { leave(); }

        void enter()
        {
            if (thread == nullptr)
            {
                thread = juce::Thread::getCurrentThreadId();
                initialised = SUCCEEDED (CoInitializeEx (nullptr, COINIT_MULTITHREADED));
            }
        }
        void leave()
        {
            // CoUninitialize belongs to the thread that initialised; never undo another thread's COM
            if (initialised && thread == juce::Thread::getCurrentThreadId())
                CoUninitialize();
            initialised = false;
            thread = nullptr;
        }

    private:
        juce::Thread::ThreadID thread = nullptr;
        bool initialised = false;
    };

    // MFStartup / MFShutdown are reference counted
    struct MediaFoundationSession
    {
        MediaFoundationSession() : started (SUCCEEDED (MFStartup (MF_VERSION, MFSTARTUP_LITE))) {}
        ~MediaFoundationSession()
        {
            if (started)
                MFShutdown();
        }
        bool started;
    };

    struct StreamFormat
    {
        UINT32 sampleRate = 0, channels = 0;
    };

    // Opens `path`, selects the first audio stream and asks for 32-bit float at the native rate and
    // channel count (a decode, never a resample). Returns nullptr if that is not possible.
    ComPtr<IMFSourceReader> openSourceReader (const juce::File& path, StreamFormat& format)
    {
        ComPtr<IMFSourceReader> reader;
        if (FAILED (MFCreateSourceReaderFromURL (path.getFullPathName().toWideCharPointer(), nullptr, &reader)))
            return nullptr;
        reader->SetStreamSelection ((DWORD) MF_SOURCE_READER_ALL_STREAMS, FALSE);
        if (FAILED (reader->SetStreamSelection (audioStream, TRUE)))
            return nullptr;

        ComPtr<IMFMediaType> native;
        if (FAILED (reader->GetNativeMediaType (audioStream, 0, &native)))
            return nullptr;
        StreamFormat f;
        if (FAILED (native->GetUINT32 (MF_MT_AUDIO_SAMPLES_PER_SECOND, &f.sampleRate))
            || FAILED (native->GetUINT32 (MF_MT_AUDIO_NUM_CHANNELS, &f.channels)) || f.sampleRate == 0 || f.channels == 0)
            return nullptr;

        ComPtr<IMFMediaType> wanted;
        if (FAILED (MFCreateMediaType (&wanted)))
            return nullptr;
        wanted->SetGUID (MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        wanted->SetGUID (MF_MT_SUBTYPE, MFAudioFormat_Float);
        wanted->SetUINT32 (MF_MT_AUDIO_SAMPLES_PER_SECOND, f.sampleRate);
        wanted->SetUINT32 (MF_MT_AUDIO_NUM_CHANNELS, f.channels);
        wanted->SetUINT32 (MF_MT_AUDIO_BITS_PER_SAMPLE, 32);
        wanted->SetUINT32 (MF_MT_AUDIO_BLOCK_ALIGNMENT, f.channels * 4);
        wanted->SetUINT32 (MF_MT_AUDIO_AVG_BYTES_PER_SECOND, f.channels * 4 * f.sampleRate);
        if (FAILED (reader->SetCurrentMediaType (audioStream, nullptr, wanted.Get())))
            return nullptr;

        // check what the decoder actually delivers
        ComPtr<IMFMediaType> current;
        GUID subtype {};
        StreamFormat got;
        UINT32 bits = 0;
        if (FAILED (reader->GetCurrentMediaType (audioStream, &current)) || FAILED (current->GetGUID (MF_MT_SUBTYPE, &subtype))
            || FAILED (current->GetUINT32 (MF_MT_AUDIO_SAMPLES_PER_SECOND, &got.sampleRate))
            || FAILED (current->GetUINT32 (MF_MT_AUDIO_NUM_CHANNELS, &got.channels))
            || FAILED (current->GetUINT32 (MF_MT_AUDIO_BITS_PER_SAMPLE, &bits)))
            return nullptr;
        if (subtype != MFAudioFormat_Float || bits != 32 || got.sampleRate != f.sampleRate || got.channels != f.channels)
            return nullptr;
        format = f;
        return reader;
    }

    // Duration from the container (MP4: the edit list; ADTS: estimated), in 100 ns units
    std::optional<LONGLONG> durationOf (IMFSourceReader& reader)
    {
        PROPVARIANT value;
        PropVariantInit (&value);
        std::optional<LONGLONG> result;
        if (SUCCEEDED (reader.GetPresentationAttribute ((DWORD) MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &value))
            && value.vt == VT_UI8)
            result = (LONGLONG) value.uhVal.QuadPart;
        PropVariantClear (&value);
        return result;
    }

    juce::int64 toFrames (LONGLONG hundredNs, double sampleRate)
    {
        return (juce::int64) std::llround ((double) hundredNs * sampleRate / 1.0e7);
    }
} // namespace

class MediaFoundationReader : public juce::AudioFormatReader
{
public:
    MediaFoundationReader (juce::InputStream* in, juce::File f) : juce::AudioFormatReader (in, mfFormatName), path (std::move (f)) {}

    // The file analysis destroys the reader on the thread that read it (see FileAnalysis::Worker),
    // so the decoder and its COM scope are released where they were created.
    ~MediaFoundationReader() override
    {
        decoder.Reset();
        session.reset(); // MFShutdown before COM goes
        decoderCom.leave();
    }

    // Reads the stream format and duration; the decoder itself is created by the first read, on
    // the reading thread
    bool open()
    {
        ComScope com; // the probe below is released before this scope ends
        com.enter();
        session = std::make_unique<MediaFoundationSession>();
        if (! session->started)
            return false;
        StreamFormat f;
        auto probe = openSourceReader (path, f);
        if (probe == nullptr)
            return false;
        const auto duration = durationOf (*probe.Get());
        probe.Reset();
        sampleRate = (double) f.sampleRate;
        numChannels = f.channels;
        bitsPerSample = 32;
        usesFloatingPointData = true;
        // Media Foundation ignores the edit list / iTunSMPB: drop the priming and cut at the real
        // length ourselves, as FFmpeg does
        juce::FileInputStream container (path);
        const auto timing = container.openedOk() ? readMp4Timing (container, sampleRate) : Mp4Timing {};
        priming = timing.primingSamples;
        // The container's length is trusted only up to Media Foundation's own duration (+0.1 s);
        // a broken or crafted edit list must not turn into days of silence. Without a duration from
        // Media Foundation (e.g. fragmented MP4), the edit list's length up to 24 h is used.
        const auto mfLength = duration && *duration > 0 ? std::optional<juce::int64> (toFrames (*duration, sampleRate))
                                                         : std::nullopt;
        const auto limit = mfLength ? *mfLength + (juce::int64) (sampleRate / 10.0) : (juce::int64) (sampleRate * 24 * 3600);
        if (timing.lengthSamples && *timing.lengthSamples > 0 && *timing.lengthSamples <= limit)
            lengthInSamples = *timing.lengthSamples;
        else if (mfLength)
            lengthInSamples = std::max<juce::int64> (0, *mfLength - priming);
        else
            return false;
        return lengthInSamples > 0;
    }

    void releaseInput() noexcept { input = nullptr; } // the caller keeps its stream after a failed open

    bool readSamples (int* const* destChannels, int numDestChannels, int startOffsetInDestBuffer,
                      juce::int64 startSampleInFile, int numSamples) override
    {
        clearSamplesBeyondAvailableLength (destChannels, numDestChannels, startOffsetInDestBuffer, startSampleInFile,
                                           numSamples, lengthInSamples);
        if (numSamples <= 0)
            return true;
        if (decoder == nullptr || startSampleInFile < pendingStart)
            if (! restart())
                return false;

        const int channels = (int) numChannels;
        int written = 0;
        while (written < numSamples)
        {
            const juce::int64 want = startSampleInFile + written;
            const auto pendingFrames = (juce::int64) (pending.size() / (size_t) channels);
            if (want >= pendingStart && want < pendingStart + pendingFrames)
            {
                const int n = (int) std::min<juce::int64> (numSamples - written, pendingStart + pendingFrames - want);
                const float* src = pending.data() + (size_t) (want - pendingStart) * (size_t) channels;
                for (int ch = 0; ch < numDestChannels; ++ch)
                    if (auto* dest = reinterpret_cast<float*> (destChannels[ch]))
                    {
                        dest += startOffsetInDestBuffer + written;
                        if (ch < channels)
                            for (int i = 0; i < n; ++i)
                                dest[i] = src[(size_t) i * (size_t) channels + (size_t) ch];
                        else
                            std::fill (dest, dest + n, 0.0f);
                    }
                written += n;
                continue;
            }
            if (endOfStream)
            {
                // the container's duration can exceed what the decoder delivers by a few samples
                for (int ch = 0; ch < numDestChannels; ++ch)
                    if (auto* dest = reinterpret_cast<float*> (destChannels[ch]))
                        std::fill (dest + startOffsetInDestBuffer + written, dest + startOffsetInDestBuffer + numSamples, 0.0f);
                break;
            }
            if (! decodeNext())
                return false;
        }
        return true;
    }

private:
    bool restart()
    {
        decoderCom.enter();
        decoder.Reset();
        pending.clear();
        pendingStart = nextStart = 0;
        endOfStream = false;
        firstSample = true;
        StreamFormat f;
        decoder = openSourceReader (path, f);
        return decoder != nullptr && (double) f.sampleRate == sampleRate && f.channels == numChannels;
    }

    // Replaces `pending` with the next decoded block. The first block's timestamp places the
    // stream on the file's time line: samples before 0 (encoder priming, when the container marks
    // it) are dropped.
    bool decodeNext()
    {
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        ComPtr<IMFSample> sample;
        if (FAILED (decoder->ReadSample (audioStream, 0, nullptr, &flags, &timestamp, &sample)))
            return false;
        if ((flags & MF_SOURCE_READERF_ERROR) != 0 || (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) != 0)
            return false;
        pending.clear();
        pendingStart = nextStart;
        if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0)
        {
            endOfStream = true;
            return true;
        }
        if (sample == nullptr) // e.g. a stream tick: nothing decoded
            return true;

        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED (sample->ConvertToContiguousBuffer (&buffer)))
            return false;
        BYTE* data = nullptr;
        DWORD bytes = 0;
        if (FAILED (buffer->Lock (&data, nullptr, &bytes)))
            return false;
        const size_t floats = (bytes / (sizeof (float) * numChannels)) * numChannels;
        pending.resize (floats);
        std::memcpy (pending.data(), data, floats * sizeof (float));
        buffer->Unlock();

        if (firstSample)
        {
            firstSample = false;
            // a stream that starts later than 0 (e.g. cut from a longer one) is read from its first
            // sample: positions never lie ahead of what was decoded, so no read waits forever
            pendingStart = nextStart = std::min<juce::int64> (0, toFrames (timestamp, sampleRate) - priming);
        }
        const auto frames = (juce::int64) (floats / numChannels);
        nextStart = pendingStart + frames;
        if (pendingStart < 0)
        {
            const auto drop = (std::min) (frames, -pendingStart);
            pending.erase (pending.begin(), pending.begin() + (std::ptrdiff_t) (drop * (juce::int64) numChannels));
            pendingStart += drop;
        }
        return true;
    }

    ComScope decoderCom;                           // COM of the reading thread (declared first: released last)
    std::unique_ptr<MediaFoundationSession> session;
    juce::File path;
    ComPtr<IMFSourceReader> decoder;
    std::vector<float> pending;          // interleaved, starts at file frame `pendingStart`
    juce::int64 pendingStart = 0, nextStart = 0;
    juce::int64 priming = 0; // encoder priming samples (from the container) dropped at the start
    bool endOfStream = false, firstSample = true;
};
#endif

MediaFoundationAudioFormat::MediaFoundationAudioFormat() : juce::AudioFormat (mfFormatName, ".m4a .aac") {}

juce::AudioFormatReader* MediaFoundationAudioFormat::createReaderFor (juce::InputStream* source, bool deleteStreamIfOpeningFails)
{
#if JUCE_WINDOWS
    // Media Foundation opens the file itself; only streams read from a file can be handled
    if (auto* fileStream = dynamic_cast<juce::FileInputStream*> (source))
    {
        auto reader = std::make_unique<MediaFoundationReader> (source, fileStream->getFile());
        if (reader->open())
            return reader.release();
        reader->releaseInput(); // `source` is deleted below, or stays with the caller
    }
#endif
    if (deleteStreamIfOpeningFails)
        delete source;
    return nullptr;
}

void registerAudioFormats (juce::AudioFormatManager& formats)
{
    formats.registerBasicFormats();
#if JUCE_WINDOWS
    formats.registerFormat (new MediaFoundationAudioFormat(), false);
#endif
}

} // namespace ada
