//  Copyright (c) 2022-2024 Fredrik Mellbin
//
//  Permission is hereby granted, free of charge, to any person obtaining a copy
//  of this software and associated documentation files (the "Software"), to deal
//  in the Software without restriction, including without limitation the rights
//  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
//  copies of the Software, and to permit persons to whom the Software is
//  furnished to do so, subject to the following conditions:
//
//  The above copyright notice and this permission notice shall be included in
//  all copies or substantial portions of the Software.
//
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
//  THE SOFTWARE.

#include "audiosource.h"
#include "version.h"
#include <algorithm>
#include <thread>
#include <cassert>
#include <iterator>

#include "../libp2p/p2p_api.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hash.h>
}

#undef NDEBUG

#ifndef NDEBUG
static void DebugPrint(const std::string_view Message, int64_t RequestedN = -1, int64_t CurrentN = -1) {
    printf("Req/Current: %" PRId64 "/%" PRId64 ", %s\n", RequestedN, CurrentN, Message.data());
}
#else
#define DebugPrint(x, ...)
#endif

bool LWAudioDecoder::ReadPacket() {
    while (av_read_frame(FormatContext, Packet) >= 0) {
        if (Packet->stream_index == TrackNumber)
            return true;
        av_packet_unref(Packet);
    }
    return false;
}

bool LWAudioDecoder::DecodeNextFrame(bool SkipOutput) {
    if (!DecodeFrame) {
        DecodeFrame = av_frame_alloc();
        if (!DecodeFrame)
            throw AudioException("Couldn't allocate frame");
    }

    while (true) {
        int Ret = avcodec_receive_frame(CodecContext, DecodeFrame);
        if (Ret == 0) {
            return true;
        } else if (Ret == AVERROR(EAGAIN)) {
            if (ResendPacket || ReadPacket()) {
                int SendRet = avcodec_send_packet(CodecContext, Packet);
                ResendPacket = (SendRet == AVERROR(EAGAIN));
                if (!ResendPacket)
                    av_packet_unref(Packet);
            } else {
                avcodec_send_packet(CodecContext, nullptr);
            }
        } else {
            break; // Probably EOF or some unrecoverable error so stop here
        }
    }

    return false;
}

void LWAudioDecoder::OpenFile(const std::string &SourceFile, int Track, bool VariableFormat, int Threads, const std::map<std::string, std::string> &LAVFOpts, double DrcScale) {
    TrackNumber = Track;

    AVDictionary *Dict = nullptr;
    for (const auto &Iter : LAVFOpts)
        av_dict_set(&Dict, Iter.first.c_str(), Iter.second.c_str(), 0);

    if (avformat_open_input(&FormatContext, SourceFile.c_str(), nullptr, &Dict) != 0)
        throw AudioException("Couldn't open '" + SourceFile + "'");

    av_dict_free(&Dict);

    if (avformat_find_stream_info(FormatContext, nullptr) < 0) {
        avformat_close_input(&FormatContext);
        FormatContext = nullptr;
        throw AudioException("Couldn't find stream information");
    }

    if (TrackNumber < 0) {
        for (int i = 0; i < static_cast<int>(FormatContext->nb_streams); i++) {
            if (FormatContext->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                if (TrackNumber == -1) {
                    TrackNumber = i;
                    break;
                } else {
                    TrackNumber++;
                }
            }
        }
    }

    if (TrackNumber < 0 || TrackNumber >= static_cast<int>(FormatContext->nb_streams))
        throw AudioException("Invalid track index");

    if (FormatContext->streams[TrackNumber]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)
        throw AudioException("Not an audio track");

    for (int i = 0; i < static_cast<int>(FormatContext->nb_streams); i++)
        if (i != TrackNumber)
            FormatContext->streams[i]->discard = AVDISCARD_ALL;

    const AVCodec *Codec = avcodec_find_decoder(FormatContext->streams[TrackNumber]->codecpar->codec_id);

    if (Codec == nullptr)
        throw AudioException("Audio codec not found");

    CodecContext = avcodec_alloc_context3(Codec);
    if (CodecContext == nullptr)
        throw AudioException("Could not allocate video decoding context");

    if (avcodec_parameters_to_context(CodecContext, FormatContext->streams[TrackNumber]->codecpar) < 0)
        throw AudioException("Could not copy video codec parameters");

    if (Threads < 1) {
        int HardwareConcurrency = std::thread::hardware_concurrency();
        Threads = std::min(HardwareConcurrency, 16);
    }
    CodecContext->thread_count = Threads;

    // FIXME, implement for newer ffmpeg versions
    if (!VariableFormat) {
        // Probably guard against mid-stream format changes
        CodecContext->flags |= AV_CODEC_FLAG_DROPCHANGED;
    }

    if (DrcScale < 0)
        throw AudioException("Invalid drc_scale value");

    AVDictionary *CodecDict = nullptr;
    if (Codec->id == AV_CODEC_ID_AC3 || Codec->id == AV_CODEC_ID_EAC3)
        av_dict_set(&CodecDict, "drc_scale", std::to_string(DrcScale).c_str(), 0);

    if (avcodec_open2(CodecContext, Codec, nullptr) < 0)
        throw AudioException("Could not open audio codec");
}

LWAudioDecoder::LWAudioDecoder(const std::string &SourceFile, int Track, bool VariableFormat, int Threads, const std::map<std::string, std::string> &LAVFOpts, double DrcScale) {
    try {
        Packet = av_packet_alloc();
        OpenFile(SourceFile, Track, VariableFormat, Threads, LAVFOpts, DrcScale);
    } catch (...) {
        Free();
        throw;
    }
}

void LWAudioDecoder::Free() {
    av_packet_free(&Packet);
    av_frame_free(&DecodeFrame);
    avcodec_free_context(&CodecContext);
    avformat_close_input(&FormatContext);
}

LWAudioDecoder::~LWAudioDecoder() {
    Free();
}

int64_t LWAudioDecoder::GetSourceSize() const {
    return avio_size(FormatContext->pb);
}

int64_t LWAudioDecoder::GetSourcePostion() const {
    return avio_tell(FormatContext->pb);
}

int LWAudioDecoder::GetTrack() const {
    return TrackNumber;
}

int64_t LWAudioDecoder::GetFrameNumber() const {
    return CurrentFrame;
}

int64_t LWAudioDecoder::GetSamplePos() const {
    return CurrentSample;
}

void LWAudioDecoder::SetFrameNumber(int64_t N, int64_t SampleNumber) {
    CurrentFrame = N;
    CurrentSample = SampleNumber;
}

void LWAudioDecoder::GetAudioProperties(AudioProperties &AP) {
    assert(CurrentFrame == 0);
    AP = {};
    AVFrame *PropFrame = GetNextFrame();
    assert(PropFrame);
    if (!PropFrame)
        return;

    AP.IsFloat = (PropFrame->format == AV_SAMPLE_FMT_FLTP || PropFrame->format == AV_SAMPLE_FMT_FLT || PropFrame->format == AV_SAMPLE_FMT_DBLP || PropFrame->format == AV_SAMPLE_FMT_DBL);
    AP.BytesPerSample = av_get_bytes_per_sample(static_cast<AVSampleFormat>(PropFrame->format));
    AP.BitsPerSample = CodecContext->bits_per_raw_sample ? (CodecContext->bits_per_raw_sample) : (AP.BytesPerSample * 8); // assume all bits are relevant if not specified
    AP.SampleRate = PropFrame->sample_rate;
    AP.Channels = PropFrame->ch_layout.nb_channels;

    if (PropFrame->ch_layout.order == AV_CHANNEL_ORDER_NATIVE) {
        AP.ChannelLayout = PropFrame->ch_layout.u.mask;
    } else if (PropFrame->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) {
        AVChannelLayout ch = {};
        av_channel_layout_default(&ch, PropFrame->ch_layout.nb_channels);
        AP.ChannelLayout = ch.u.mask;
    } else {
        throw AudioException("Ambisonics and custom channel orders not supported");
    }

    AP.NumSamples = (FormatContext->duration * PropFrame->sample_rate) / AV_TIME_BASE - FormatContext->streams[TrackNumber]->codecpar->initial_padding;
    if (PropFrame->pts != AV_NOPTS_VALUE)
        AP.StartTime = (static_cast<double>(FormatContext->streams[TrackNumber]->time_base.num) * PropFrame->pts) / FormatContext->streams[TrackNumber]->time_base.den;

    if (AP.BytesPerSample <= 0)
        throw AudioException("Codec returned zero size audio");
}

AVFrame *LWAudioDecoder::GetNextFrame() {
    if (DecodeSuccess) {
        DecodeSuccess = DecodeNextFrame();
        if (DecodeSuccess) {
            CurrentFrame++;
            CurrentSample += DecodeFrame->nb_samples;
            AVFrame *Tmp = DecodeFrame;
            DecodeFrame = nullptr;
            return Tmp;
        }
    } 
    return nullptr;
}

bool LWAudioDecoder::SkipFrames(int64_t Count) {
    while (Count-- > 0) {
        if (DecodeSuccess) {
            DecodeSuccess = DecodeNextFrame(true);
            if (DecodeSuccess) {
                CurrentFrame++;
                CurrentSample += DecodeFrame->nb_samples;
            }
        } else {
            break;
        }
    }
    return DecodeSuccess;
}

bool LWAudioDecoder::HasMoreFrames() const {
    return DecodeSuccess;
}

bool LWAudioDecoder::Seek(int64_t PTS) {
    Seeked = true;
    avcodec_flush_buffers(CodecContext);
    CurrentFrame = INT64_MIN;
    CurrentSample = INT64_MIN;
    // Mild variable reuse, if seek fails then there's no point to decode more either
    DecodeSuccess = (av_seek_frame(FormatContext, TrackNumber, PTS, AVSEEK_FLAG_BACKWARD) >= 0);
    return DecodeSuccess;
}

bool LWAudioDecoder::HasSeeked() const {
    return Seeked;
}

BestAudioFrame::BestAudioFrame(AVFrame *F) {
    assert(F);
    Frame = av_frame_clone(F);
    // FIXME, fill in
    Pts = Frame->pts;
    NumSamples = Frame->nb_samples;
}

BestAudioFrame::~BestAudioFrame() {
    av_frame_free(&Frame);
}

const AVFrame *BestAudioFrame::GetAVFrame() const {
    return Frame;
};

static std::array<uint8_t, 16> GetHash(const AVFrame *Frame) {
    std::array<uint8_t, 16> Hash;

    bool IsPlanar = av_sample_fmt_is_planar(static_cast<AVSampleFormat>(Frame->format));
    int BytesPerSample = av_get_bytes_per_sample(static_cast<AVSampleFormat>(Frame->format));

    AVHashContext *hctx;
    av_hash_alloc(&hctx, "md5");

    if (IsPlanar) {
        int NumPlanes = Frame->ch_layout.nb_channels;
        for (int p = 0; p < NumPlanes; p++)
            av_hash_update(hctx, Frame->extended_data[p], BytesPerSample * Frame->nb_samples);
    } else {
        av_hash_update(hctx, Frame->data[0], BytesPerSample * Frame->ch_layout.nb_channels * Frame->nb_samples);
    }

    av_hash_final(hctx, Hash.data());
    av_hash_freep(&hctx);
    return Hash;
}

BestAudioSource::Cache::CacheBlock::CacheBlock(int64_t FrameNumber, AVFrame *Frame) : FrameNumber(FrameNumber), Frame(Frame) {
    assert(Frame->nb_samples > 0);
    for (int i = 0; i < Frame->nb_extended_buf; i++)
        if (Frame->extended_buf[i])
            Size += Frame->extended_buf[i]->size;
}

BestAudioSource::Cache::CacheBlock::~CacheBlock() {
    av_frame_free(&Frame);
}

void BestAudioSource::Cache::ApplyMaxSize() {
    while (Size > MaxSize) {
        Size -= Data.back().Size;
        Data.pop_back();
    }
}

void BestAudioSource::Cache::Clear() {
    Data.clear();
    Size = 0;
}

void BestAudioSource::Cache::SetMaxSize(size_t Bytes) {
    MaxSize = Bytes;
    ApplyMaxSize();
}

void BestAudioSource::Cache::CacheFrame(int64_t FrameNumber, AVFrame *Frame) {
    assert(Frame);
    assert(FrameNumber >= 0);
    // Don't cache the same frame twice, get rid of the oldest copy instead
    for (auto Iter = Data.begin(); Iter != Data.end(); ++Iter) {
        if (Iter->FrameNumber == FrameNumber) {
            Size -= Iter->Size;
            Data.erase(Iter);
            break;
        }
    }

    Data.emplace_front(FrameNumber, Frame);
    Size += Data.front().Size;
    ApplyMaxSize();
}

BestAudioFrame *BestAudioSource::Cache::GetFrame(int64_t N) {
    for (auto Iter = Data.begin(); Iter != Data.end(); ++Iter) {
        if (Iter->FrameNumber == N) {
            AVFrame *F = Iter->Frame;
            Data.splice(Data.begin(), Data, Iter);
            return new BestAudioFrame(F);
        }
    }
    return nullptr;
}

BestAudioSource::BestAudioSource(const std::string &SourceFile, int Track, bool VariableFormat, int Threads, const std::string &CachePath, const std::map<std::string, std::string> *LAVFOpts, double DrcScale, const std::function<void(int Track, int64_t Current, int64_t Total)> &Progress)
    : Source(SourceFile), AudioTrack(Track), VariableFormat(VariableFormat), Threads(Threads), DrcScale(DrcScale) {
    if (LAVFOpts)
        LAVFOptions = *LAVFOpts;

    std::unique_ptr<LWAudioDecoder> Decoder(new LWAudioDecoder(Source, AudioTrack, VariableFormat, Threads, LAVFOptions, DrcScale));

    Decoder->GetAudioProperties(AP);
    AudioTrack = Decoder->GetTrack();
    
    if (!ReadAudioTrackIndex(CachePath.empty() ? SourceFile : CachePath)) {
        if (!IndexTrack(Progress))
            throw AudioException("Indexing of '" + SourceFile + "' track #" + std::to_string(AudioTrack) + " failed");

        WriteAudioTrackIndex(CachePath.empty() ? SourceFile : CachePath);
    }

    AP.NumFrames = TrackIndex.Frames.size();
    AP.NumSamples = TrackIndex.Frames.back().Start + TrackIndex.Frames.back().Length;

    Decoders[0] = std::move(Decoder);
}

int BestAudioSource::GetTrack() const {
    return AudioTrack;
}

void BestAudioSource::SetMaxCacheSize(size_t Bytes) {
    FrameCache.SetMaxSize(Bytes);
}

void BestAudioSource::SetSeekPreRoll(int64_t Frames) {
    PreRoll = std::max<int64_t>(Frames, 0);
}

bool BestAudioSource::IndexTrack(const std::function<void(int Track, int64_t Current, int64_t Total)> &Progress) {
    std::unique_ptr<LWAudioDecoder> Decoder(new LWAudioDecoder(Source, AudioTrack, VariableFormat, Threads, LAVFOptions, DrcScale));

    int64_t FileSize = Progress ? Decoder->GetSourceSize() : -1;

    // Fixme, implement frame discarding based on first seen format?
    /*
    bool First = true;
    int Format = -1;
    int Width = -1;
    int Height = -1;
    */

    int64_t NumSamples = 0;

    while (true) {
        AVFrame *F = Decoder->GetNextFrame();
        if (!F)
            break;

        TrackIndex.Frames.push_back({ F->pts, NumSamples, F->nb_samples, GetHash(F) });
        NumSamples += F->nb_samples;

        av_frame_free(&F);
        if (Progress)
            Progress(AudioTrack, Decoder->GetSourcePostion(), FileSize);
    };

    if (Progress)
        Progress(AudioTrack, INT64_MAX, INT64_MAX);

    return !TrackIndex.Frames.empty();
}

const AudioProperties &BestAudioSource::GetAudioProperties() const {
    return AP;
}

// Short algorithm summary
// 1. If a current decoder is close to the requested frame simply start from there
//    Determine if a decoder is "close" based on whether or not it is already in the optimal zone based on the existing keyframes
// 2. If a decoder isn't nearby and the seek destination is within the first 100 frames simply start with a fresh decoder to avoid the seek to start issue (technically almost always fresh)
// 3. Seek with an existing or new decoder. Seek to the nearest keyframe at or before frame N-preroll using PTS. If no such point exists more than 100 frames after the start don't seek.
//    After seeking match the hash of the decoded frame. For duplicate hashes match a string of up to 10 frame hashes.
// 4. If the frame is determined to not exist, be beyond the target frame to decode or simply in a string of frames that aren't uniquely identifiable by hashes mark the keyframe as unusable and retry seeking to
//    at least 100 frames earlier.
// 5. If linear decoding after seeking fails handle it the same way as #4 and flag it as a bad seek point and retry from at least 100 frames earlier.

BestAudioFrame *BestAudioSource::GetFrame(int64_t N, bool Linear) {
    if (N < 0 || N >= AP.NumFrames)
        return nullptr;

    std::unique_ptr<BestAudioFrame> F(FrameCache.GetFrame(N));
    if (!F)
        F.reset(Linear ? GetFrameLinearInternal(N) : GetFrameInternal(N));

    return F.release();
}

void BestAudioSource::SetLinearMode() {
    assert(!LinearMode);
    if (!LinearMode) {
        DebugPrint("Linear mode is now forced");
        LinearMode = true;
        FrameCache.Clear();
        for (size_t i = 0; i < MaxVideoSources; i++)
            Decoders[i].reset();
    }
}

int64_t BestAudioSource::GetSeekFrame(int64_t N) {
    // FIXME, maybe needs a version that takes a sample position?
    for (int64_t i = N - PreRoll; i >= 100; i--) {
        if (TrackIndex.Frames[i].PTS != AV_NOPTS_VALUE && !BadSeekLocations.count(i))
            return i;
    }

    return -1;
}

namespace {
    class FrameHolder {
    private:
        std::vector<std::pair<AVFrame *, std::array<uint8_t, 16>>> Data;
    public:
        void clear() {
            for (auto &iter : Data)
                av_frame_free(&iter.first);
            Data.clear();
        }

        void push_back(AVFrame *F) {
            Data.push_back(std::make_pair(F, GetHash(F)));
        }

        size_t size() {
            return Data.size();
        }

        size_t empty() {
            return Data.empty();
        }

        [[nodiscard]] AVFrame *GetFrame(size_t Index, bool Extract = false) {
            AVFrame *Tmp = Data[Index].first;
            if (Extract)
                Data[Index].first = nullptr;
            return Tmp;
        }

        [[nodiscard]] bool CompareHash(size_t Index, const std::array<uint8_t, 16> &Other) {
            return Data[Index].second == Other;
        }

        ~FrameHolder() {
            clear();
        }
    };
}

BestAudioFrame *BestAudioSource::SeekAndDecode(int64_t N, int64_t SeekFrame, std::unique_ptr<LWAudioDecoder> &Decoder, size_t Depth) {
    if (!Decoder->Seek(TrackIndex.Frames[SeekFrame].PTS)) {
        DebugPrint("Unseekable file", N);
        SetLinearMode();
        return GetFrameLinearInternal(N);
    }

    Decoder->SkipFrames(PreRoll / 2);

    FrameHolder MatchFrames;

    while (true) {
        AVFrame *F = Decoder->GetNextFrame();
        if (!F && MatchFrames.empty()) {
            BadSeekLocations.insert(SeekFrame);
            DebugPrint("No frame could be decoded after seeking, added as bad seek location", N, SeekFrame);
            if (Depth < RetrySeekAttempts) {
                int64_t SeekFrameNext = GetSeekFrame(SeekFrame - 100);
                DebugPrint("Retrying seeking with", N, SeekFrameNext);
                if (SeekFrameNext < 100) { // #2 again
                    Decoder.reset();
                    return GetFrameLinearInternal(N);
                } else {
                    return SeekAndDecode(N, SeekFrameNext, Decoder, Depth + 1);
                }
            } else {
                DebugPrint("Maximum number of seek attempts made, setting linear mode", N, SeekFrame);
                SetLinearMode();
                return GetFrameLinearInternal(N);
            }
        }

        std::set<int64_t> Matches;

        if (F) {
            MatchFrames.push_back(F);

            for (size_t i = 0; i <= TrackIndex.Frames.size() - MatchFrames.size(); i++) {
                bool HashMatch = true;
                for (size_t j = 0; j < MatchFrames.size(); j++)
                    HashMatch = HashMatch && MatchFrames.CompareHash(j, TrackIndex.Frames[i + j].Hash);
                if (HashMatch)
                    Matches.insert(i);
            }
        } else if (!F) {
            bool HashMatch = true;
            for (size_t j = 0; j < MatchFrames.size(); j++)
                HashMatch = HashMatch && MatchFrames.CompareHash(j, TrackIndex.Frames[TrackIndex.Frames.size() - MatchFrames.size() + j].Hash);
            if (HashMatch)
                Matches.insert(TrackIndex.Frames.size() - MatchFrames.size());
        }

        // #3 Seek failure?, fall back to linear
        // Check if any match is in target zone, if not seek further back a couple of times
        bool SuitableCandidate = false;
        for (const auto &iter : Matches)
            if (iter <= N) // Do we care about preroll or is it just a nice thing to have? With seeking it's a lot less important anyway...
                SuitableCandidate = true;

        bool UndeterminableLocation = (Matches.size() > 1 && (!F || MatchFrames.size() >= 10));

#ifndef NDEBUG
        if (!SuitableCandidate && Matches.size() > 0)
            DebugPrint("Seek location beyond destination, have to retry seeking", N, SeekFrame);
        else if (!SuitableCandidate)
            DebugPrint("Seek location yielded corrupt frame, have to retry seeking", N, SeekFrame);

        if (UndeterminableLocation)
            DebugPrint("Seek location cannot be unambiguosly identified, have to retry seeking", N, SeekFrame);
#endif

        if (!SuitableCandidate || UndeterminableLocation) {
            DebugPrint("No destination frame number could be determined after seeking, added as bad seek location", N, SeekFrame);
            BadSeekLocations.insert(SeekFrame);
            MatchFrames.clear();
            if (Depth < RetrySeekAttempts) {
                int64_t SeekFrameNext = GetSeekFrame(SeekFrame - 100);
                DebugPrint("Retrying seeking with", N, SeekFrameNext);
                if (SeekFrameNext < 100) { // #2 again
                    Decoder.reset();
                    return GetFrameLinearInternal(N);
                } else {
                    return SeekAndDecode(N, SeekFrameNext, Decoder, Depth + 1);
                }
            } else {
                DebugPrint("Maximum number of seek attempts made, setting linear mode", N, SeekFrame);
                // Fall back to linear decoding permanently since we failed to seek to any even remotably suitable frame in 3 attempts
                SetLinearMode();
                return GetFrameLinearInternal(N);
            }
        }

        if (Matches.size() == 1) {
            int64_t MatchedN = *Matches.begin();

#ifndef NDEBUG
            if (MatchedN < 100)
                DebugPrint("Seek destination determined to be within 100 frames of start, this was unexpected", N, MatchedN);
#endif

            Decoder->SetFrameNumber(MatchedN + MatchFrames.size(), TrackIndex.Frames[MatchedN + MatchFrames.size()].Start);

            // Insert frames into cache if appropriate
            BestAudioFrame *RetFrame = nullptr;
            for (size_t FramesIdx = 0; FramesIdx < MatchFrames.size(); FramesIdx++) {
                int64_t FrameNumber = MatchedN + FramesIdx;

                if (FrameNumber >= N - PreRoll) {
                    if (FrameNumber == N)
                        RetFrame = new BestAudioFrame(MatchFrames.GetFrame(FramesIdx));

                    FrameCache.CacheFrame(FrameNumber, MatchFrames.GetFrame(FramesIdx, true));
                }
            }

            if (RetFrame)
                return RetFrame;

            // Now that we have done everything we can and aren't holding on to the frame to output let the linear function do the rest
            MatchFrames.clear();
            return GetFrameLinearInternal(N, SeekFrame);
        }

        assert(Matches.size() > 1);

        // Multiple candidates match, go another lap to figure out which one it is
    };

    // All paths should exit elsewhere
    assert(false);
    return nullptr;
}

BestAudioFrame *BestAudioSource::GetFrameInternal(int64_t N) {
    if (LinearMode)
        return GetFrameLinearInternal(N);

    // #2 If the seek limit is less than 100 frames away from the start see #2 and do linear decoding
    int64_t SeekFrame = GetSeekFrame(N);

    if (SeekFrame < 100)
        return GetFrameLinearInternal(N);

    // # 1 A suitable linear decoder exists and seeking is out of the question
    for (int i = 0; i < MaxVideoSources; i++) {
        if (Decoders[i] && Decoders[i]->GetFrameNumber() <= N && Decoders[i]->GetFrameNumber() >= SeekFrame)
            return GetFrameLinearInternal(N);
    }

    // #3 Preparations here

    // Grab/create a new decoder to use for seeking, the position is irrelevant
    int EmptySlot = -1;
    int LeastRecentlyUsed = 0;
    for (int i = 0; i < MaxVideoSources; i++) {
        if (!Decoders[i])
            EmptySlot = i;
        if (Decoders[i] && DecoderLastUse[i] < DecoderLastUse[LeastRecentlyUsed])
            LeastRecentlyUsed = i;
    }

    int Index = (EmptySlot >= 0) ? EmptySlot : LeastRecentlyUsed;
    if (!Decoders[Index])
        Decoders[Index].reset(new LWAudioDecoder(Source, AudioTrack, VariableFormat, Threads, LAVFOptions, DrcScale));

    DecoderLastUse[Index] = DecoderSequenceNum++;

    // #3 Actual seeking dance of death starts here
    return SeekAndDecode(N, SeekFrame, Decoders[Index]);
}

BestAudioFrame *BestAudioSource::GetFrameLinearInternal(int64_t N, int64_t SeekFrame, size_t Depth, bool ForceUnseeked) {
    // Check for a suitable existing decoder
    int Index = -1;
    int EmptySlot = -1;
    int LeastRecentlyUsed = 0;
    for (int i = 0; i < MaxVideoSources; i++) {
        if (Decoders[i] && (!ForceUnseeked || !Decoders[i]->HasSeeked()) && Decoders[i]->GetFrameNumber() <= N && (Index < 0 || Decoders[Index]->GetFrameNumber() < Decoders[i]->GetFrameNumber()))
            Index = i;
        if (!Decoders[i])
            EmptySlot = i;
        if (Decoders[i] && DecoderLastUse[i] < DecoderLastUse[LeastRecentlyUsed])
            LeastRecentlyUsed = i;
    }

    // If an empty slot exists simply spawn a new decoder there or reuse the least recently used decoder slot if no free ones exist
    if (Index < 0) {
        Index = (EmptySlot >= 0) ? EmptySlot : LeastRecentlyUsed;
        Decoders[Index].reset(new LWAudioDecoder(Source, AudioTrack, VariableFormat, Threads, LAVFOptions, DrcScale));
    }

    std::unique_ptr<LWAudioDecoder> &Decoder = Decoders[Index];
    DecoderLastUse[Index] = DecoderSequenceNum++;

    BestAudioFrame *RetFrame = nullptr;

    while (Decoder && Decoder->GetFrameNumber() <= N && Decoder->HasMoreFrames()) {
        int64_t FrameNumber = Decoder->GetFrameNumber();
        int64_t SamplePos = Decoder->GetSamplePos();
        if (FrameNumber >= N - PreRoll) {
            AVFrame *Frame = Decoder->GetNextFrame();

            // This is the most central sanity check. It primarily exists to catch the case
            // when a decoder has successfully seeked and had its location identified but
            // still returns frames out of order. Possibly open gop related but hard to tell.

            if (!Frame || TrackIndex.Frames[FrameNumber].Hash != GetHash(Frame)) {
                av_frame_free(&Frame);

                if (Decoder->HasSeeked()) {
                    DebugPrint("Decoded frame does not match hash in GetFrameLinearInternal() or no frame produced at all, added as bad seek location", N, FrameNumber);
                    assert(SeekFrame >= 0);
                    BadSeekLocations.insert(SeekFrame);
                    if (Depth < RetrySeekAttempts) {
                        int64_t SeekFrameNext = GetSeekFrame(SeekFrame - 100);
                        DebugPrint("Retrying seeking with", N, SeekFrameNext);
                        if (SeekFrameNext < 100) { // #2 again
                            Decoder.reset();
                            return GetFrameLinearInternal(N);
                        } else {
                            return SeekAndDecode(N, SeekFrameNext, Decoder, Depth + 1);
                        }
                    } else {
                        DebugPrint("Maximum number of seek attempts made, setting linear mode", N, SeekFrame);
                        SetLinearMode();
                        return GetFrameLinearInternal(N, -1, 0, true);
                    }
                } else {
                    DebugPrint("Linear decoding returned a bad frame, this should be impossible so I'll just return nothing now. Try deleting the index and using threads=1 if you haven't already done so.", N, SeekFrame);
                    return nullptr;
                }
            }

            if (FrameNumber == N)
                RetFrame = new BestAudioFrame(Frame);

            FrameCache.CacheFrame(FrameNumber, Frame);
        } else if (FrameNumber < N) {
            Decoder->SkipFrames(N - PreRoll - FrameNumber);
        }

        if (!Decoder->HasMoreFrames())
            Decoder.reset();
    }

    return RetFrame;
}

BestAudioSource::FrameRange BestAudioSource::GetFrameRangeBySamples(int64_t Start, int64_t Count) const {
    FrameRange Result = { -1, -1, -1 };
    if (Count <= 0 || Start >= AP.NumSamples)
        return Result;
    if (Start < 0) {
        Result.First = 0;
    } else {
        for (size_t i = 0; i < TrackIndex.Frames.size(); i++) {
            if (Start >= TrackIndex.Frames[i].Start && Start < TrackIndex.Frames[i].Start + TrackIndex.Frames[i].Length) {
                Result.First = i;
                break;
            }
        }
    }

    int64_t EndPos = Start + Count;
    if (EndPos >= AP.NumSamples) {
        Result.Last = AP.NumFrames - 1;
    } else {
        for (size_t i = 0; i < TrackIndex.Frames.size(); i++) {
            if (EndPos - 1 >= TrackIndex.Frames[i].Start && EndPos - 1 < TrackIndex.Frames[i].Start + TrackIndex.Frames[i].Length) {
                Result.Last = i;
                break;
            }
        }
    }

    assert(Result.First >= 0 && Result.Last >= 0);

    Result.FirstSamplePos = TrackIndex.Frames[Result.First].Start;

    return Result;
}

void BestAudioSource::ZeroFillStart(uint8_t *Data[], int64_t &Start, int64_t &Count) {
    if (Start < 0) {
        int64_t Length = std::min(Count, -Start);
        size_t ByteLength = Length * AP.BytesPerSample;
        for (int i = 0; i < AP.Channels; i++) {
            memset(Data[i], 0, ByteLength);
            Data[i] += ByteLength;
        }
        Start += Length;
        Count -= Length;
    }
}

void BestAudioSource::ZeroFillEnd(uint8_t *Data[], int64_t Start, int64_t &Count) {
    if (Start + Count > AP.NumSamples) {
        int64_t Length = std::min(Start + Count - AP.NumSamples, Count);
        size_t ByteOffset = std::min<int64_t>(AP.NumSamples - Start, 0) * AP.BytesPerSample;
        for (int i = 0; i < AP.Channels; i++)
            memset(Data[i] + ByteOffset, 0, Length * AP.BytesPerSample);
        Count -= Length;
    }
}

static void UnpackChannels(const uint8_t *Src, uint8_t *Dst[], size_t Length, size_t Channels, size_t BytesPerSample) {
    const uint8_t *S = Src;
    for (size_t i = 0; i < Length; i++) {
        for (size_t c = 0; c < Channels; c++) {
            memcpy(Dst[c], S + c * BytesPerSample, BytesPerSample);
            Dst[c] += BytesPerSample;
        }
        S += Channels * BytesPerSample;
    }
}

bool BestAudioSource::FillInFramePlanar(const BestAudioFrame *Frame, int64_t FrameStartSample, uint8_t *Data[], int64_t &Start, int64_t &Count) {
    const AVFrame *F = Frame->GetAVFrame();
    bool IsPlanar = av_sample_fmt_is_planar(static_cast<AVSampleFormat>(F->format));
    if ((Start >= FrameStartSample) && (Start < FrameStartSample + Frame->NumSamples)) {
        int64_t Length = std::min(Count, Frame->NumSamples - Start + FrameStartSample);
        if (Length == 0)
            return false;

        if (IsPlanar) {
            size_t ByteLength = Length * AP.BytesPerSample;
            size_t ByteOffset = (Start - FrameStartSample) * AP.BytesPerSample;
            for (int i = 0; i < AP.Channels; i++) {
                memcpy(Data[i], Frame->GetAVFrame()->extended_data[i] + ByteOffset, ByteLength);
                Data[i] += ByteLength;
            }
        } else {
            size_t ByteOffset = (Start - FrameStartSample) * AP.BytesPerSample * F->ch_layout.nb_channels;
            UnpackChannels(F->extended_data[0] + ByteOffset, Data, Length, F->ch_layout.nb_channels, AP.BytesPerSample);
        }
        Start += Length;
        Count -= Length;
        return true;
    } else {
        assert(false);
    }
    return false;
}

void BestAudioSource::GetPlanarAudio(uint8_t *const *const Data, int64_t Start, int64_t Count) {
    Start -= 0 /* FIXME implement SampleDelay again */;

    std::vector<uint8_t *> DataV;
    DataV.reserve(AP.Channels);
    for (int i = 0; i < AP.Channels; i++)
        DataV.push_back(Data[i]);

    ZeroFillStart(DataV.data(), Start, Count);

    ZeroFillEnd(DataV.data(), Start, Count);

    auto Range = GetFrameRangeBySamples(Start, Count);
    if (Range.First == -1)
        return;

    for (int64_t i = Range.First; i <= Range.Last; i++) {
        std::unique_ptr<BestAudioFrame> F(GetFrame(i));
        FillInFramePlanar(F.get(), Range.FirstSamplePos, DataV.data(), Start, Count);
        Range.FirstSamplePos += F->NumSamples;
    }

    assert(Count == 0);

    if (Count == 0)
        return;

    if (Count != 0)
        throw AudioException("Code error, failed to provide all samples");
}

////////////////////////////////////////
// Index read/write

bool BestAudioSource::WriteAudioTrackIndex(const std::string &CachePath) {
    file_ptr_t F = OpenCacheFile(CachePath, AudioTrack, true);
    if (!F)
        return false;
    WriteBSHeader(F, false);
    // FIXME, file size, hash or something else here to make sure the index is for the right file?
    WriteInt(F, AudioTrack);
    WriteInt(F, VariableFormat);
    // FIXME, DRCSCale

    WriteInt(F, static_cast<int>(LAVFOptions.size()));
    for (const auto &Iter : LAVFOptions) {
        WriteString(F, Iter.first);
        WriteString(F, Iter.second);
    }

    WriteInt64(F, TrackIndex.Frames.size());

    for (const auto &Iter : TrackIndex.Frames) {
        fwrite(Iter.Hash.data(), 1, Iter.Hash.size(), F.get());
        WriteInt64(F, Iter.PTS);
        WriteInt64(F, Iter.Length);
    }

    return true;
}

bool BestAudioSource::ReadAudioTrackIndex(const std::string &CachePath) {
    file_ptr_t F = OpenCacheFile(CachePath, AudioTrack, false);
    if (!F)
        return false;
    if (!ReadBSHeader(F, false))
        return false;
    // FIXME, file size, hash or something else here to make sure the index is for the right file?
    if (!ReadCompareInt(F, AudioTrack))
        return false;
    if (!ReadCompareInt(F, VariableFormat))
        return false;
    // drcscale
    int LAVFOptCount = ReadInt(F);
    std::map<std::string, std::string> IndexLAVFOptions;
    for (int i = 0; i < LAVFOptCount; i++) {
        std::string Key = ReadString(F);
        IndexLAVFOptions[Key] = ReadString(F);
    }
    if (LAVFOptions != IndexLAVFOptions)
        return false;
    int64_t NumFrames = ReadInt64(F);

    TrackIndex.Frames.reserve(NumFrames);
    AP.NumSamples = 0;

    for (int i = 0; i < NumFrames; i++) {
        AudioTrackIndex::FrameInfo FI = {};
        if (fread(FI.Hash.data(), 1, FI.Hash.size(), F.get()) != FI.Hash.size())
            return false;
        FI.PTS = ReadInt64(F);
        FI.Start = AP.NumSamples;
        FI.Length = ReadInt64(F);
        AP.NumSamples += FI.Length;
        TrackIndex.Frames.push_back(FI);
    }

    return true;
}

