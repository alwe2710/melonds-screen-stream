/*
    Copyright 2016-2026 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#ifndef MELONDS_STREAMING_SOFTWAREVIDEOENCODER_H
#define MELONDS_STREAMING_SOFTWAREVIDEOENCODER_H

#include <cstdint>
#include <vector>

// Ported from the sibling Cemu project's Cemu/unisonStream/SoftwareVideoEncoder
// (same libx264/libx265 wrapper, same interface, already re-ported once to
// azahar's src/core/streaming/software_video_encoder as an intermediate
// step -- this is that same code again, just melonDS's file-naming/license-
// header convention) -- see Cemu's original for the reasoning behind the
// rate-control/profile/keyframe choices baked into the constructor, none of
// which are melonDS-specific.

namespace melonDS::Streaming {

enum class VideoCodec {
    H264,
    H265,
};

// Software H.264/H265 encoder for the Unison N3DS_BOTTOM_SCREEN stream --
// session-local, like Server::RunSession()'s other per-session state: built
// fresh per session and destroyed at session end, so encoder reference-frame
// state never crosses sessions. Wraps whichever of x264/x265's C API the
// chosen VideoCodec needs behind one shared interface, since both are
// near-identical here (RGBA8->I420 conversion, per-session encode, periodic
// forced keyframe) -- see docs/protocol.md's "Keyframe discipline" section
// for why the keyframe cadence exists (a continuous bitstream, unlike
// TILES/legacy, so a dropped frame needs a bounded self-heal instead of
// just resending state next frame).
class SoftwareVideoEncoder {
public:
    // fps is the *effective* capture rate this stream actually sends at --
    // used for encoder rate-control pacing and to derive the forced-keyframe
    // interval, not treated as a hard per-frame clock.
    SoftwareVideoEncoder(VideoCodec codec, uint32_t width, uint32_t height, uint32_t fps);
    ~SoftwareVideoEncoder();

    SoftwareVideoEncoder(const SoftwareVideoEncoder&) = delete;
    SoftwareVideoEncoder& operator=(const SoftwareVideoEncoder&) = delete;

    // True if the encoder opened successfully -- check before calling
    // EncodeFrame(); a construction failure (e.g. codec init rejected the
    // resolution) should make the caller fall back to a different video
    // mode for this session rather than crash.
    bool IsValid() const {
        return m_encoderHandle != nullptr;
    }

    // The actual coded picture size the bitstream is encoded at -- may
    // exceed the constructor's width/height if that isn't a multiple of 16
    // (see m_codedWidth's own comment; the DS's native 256x192 is
    // 16-aligned on both axes, but the OpenGL renderer's upscaled sizes
    // aren't guaranteed to be). SendVideoFrame() must send THIS, not the
    // display width/height, in the video message header that accompanies an
    // H264/H265 frame, since it's what the bitstream actually describes and
    // what the client's decoder needs to configure against.
    uint32_t CodedWidth() const {
        return m_codedWidth;
    }
    uint32_t CodedHeight() const {
        return m_codedHeight;
    }

    // The *display* (constructor-argument) size this encoder was actually
    // built for -- EncodeFrame() blindly trusts every rgba8 buffer it's
    // given to be exactly width*height*4 bytes in this stride, it never
    // re-reads the size from anywhere else. Kept even though this stream
    // type's capture size never actually varies frame to frame (unlike
    // Cemu's WIIU_GAMEPAD, whose DRC content resolution depends on what the
    // game itself renders) -- SendVideoFrame() still compares against this
    // before calling EncodeFrame() for the same defense-in-depth reasoning,
    // just never expected to actually trigger a rebuild here.
    uint32_t Width() const {
        return m_width;
    }
    uint32_t Height() const {
        return m_height;
    }

    // Encodes one RGBA8 frame (width*height*4 bytes) into outNals -- an
    // Annex-B byte stream (one or more NAL units, start-code prefixed),
    // ready to drop straight into a UNISON_MSG_VIDEO frame's compressed_data
    // with UNISON_VIDEO_FORMAT_H264/_H265 set. Returns false only on a real
    // encoder error (caller should treat this the same as SendVideoFrame()'s
    // other "skip this frame" cases) -- an encode call that legitimately
    // produces no output yet (encoder look-ahead buffering) still returns
    // true with outNals left empty.
    bool EncodeFrame(const uint8_t* rgba8, std::vector<uint8_t>& outNals);

private:
    void ConvertRgba8ToI420(const uint8_t* rgba8);

    VideoCodec m_codec;
    uint32_t m_width;
    uint32_t m_height;
    // Both H.264 and H.265 code pictures in fixed macroblock/CTU blocks (16
    // pixels for H.264; also 16 here for simplicity on the H.265 side, a
    // safe common denominator even though HEVC's CTUs can be larger) -- a
    // coded dimension that isn't a multiple of that has to be padded
    // internally and cropped back out via the bitstream's SPS conformance
    // window before display, which is exactly where at least one real
    // hardware decoder has been observed to go wrong for a non-16-aligned
    // width (see Cemu's own port of this file for the concrete case that
    // was found on). Padding up to a 16-aligned size ourselves and never
    // asking any decoder to crop anything sidesteps that class of decoder
    // bug entirely -- moot for this stream type's 320x240 (already
    // 16-aligned both ways) but kept for consistency with the ported code
    // and in case this stream's fixed resolution ever changes.
    uint32_t m_codedWidth;
    uint32_t m_codedHeight;
    uint32_t m_fps;
    // Every Nth frame (see docs/protocol.md's "Keyframe discipline") is
    // forced as a keyframe regardless of what the encoder's own rate
    // control would otherwise pick -- self-healing bound on how long a
    // dropped/corrupted frame can affect the picture for.
    uint32_t m_keyframeInterval;
    uint64_t m_frameCounter = 0;

    // I420 planes, reused across calls (sized once in the constructor) --
    // both x264 and x265 require planar 4:2:0 input, never RGB.
    std::vector<uint8_t> m_planeY;
    std::vector<uint8_t> m_planeU;
    std::vector<uint8_t> m_planeV;

    // Exactly one real encoder handle type is ever behind this, depending
    // on m_codec -- opaque void* here so this header doesn't need to expose
    // x264.h/x265.h (and their near-identical but distinct types) to every
    // includer; SoftwareVideoEncoder.cpp is the only translation unit that
    // needs the real encoder types.
    void* m_encoderHandle = nullptr;
};

} // namespace melonDS::Streaming

#endif // MELONDS_STREAMING_SOFTWAREVIDEOENCODER_H
