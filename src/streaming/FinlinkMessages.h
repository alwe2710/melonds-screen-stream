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

#ifndef MELONDS_STREAMING_FINLINKMESSAGES_H
#define MELONDS_STREAMING_FINLINKMESSAGES_H

// App-level handshake (hello / hello_ack / session_ready / handshake_error)
// for the NDS_BOTTOM_SCREEN stream type, exchanged as WebSocket text frames
// before any Video/Input binary frame, per finlink's docs/protocol.md.
// Mirrors azahar's src/core/streaming/handshake_messages.h/.cpp (same wire
// shapes, same simplification for a fixed single-slot, audio-less stream
// type -- no redirect step, no audio negotiation) -- hand-written JSON
// instead of nlohmann::json (not available here) for building, and
// finlink/json.h's span-based reader (vendored at src/finlink/, see its own
// header comment: "building the one JSON shape this client ever sends...
// done directly with snprintf-style formatting instead", the same idea
// applied here to the server's outgoing messages) for parsing hello_ack.
//
// Pure message (de)serialization -- no socket I/O, mirroring
// FinlinkWebSocket.h's own separation of transport from message content.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace melonDS::Streaming
{

constexpr int kProtocolVersion = 2;
constexpr char kStreamType[] = "NDS_BOTTOM_SCREEN";
// Dedicated encoding, not "n3ds_touch_and_buttons" -- the DS has no analog
// stick at all, so reusing finlink_extended_input would mean always
// sending 8 bytes of stick fields that could only ever be zero. This is
// finlink_touch_and_buttons (protocol.h) instead: touch + buttons only,
// no stick fields in the wire format at all. See docs/protocol.md's
// WebSocket-binäre-Frames section.
constexpr char kInputEncoding[] = "touch_and_buttons";
constexpr uint32_t kStreamWidth = 256;
constexpr uint32_t kStreamHeight = 192;
// Sample rate FINLINK_MSG_MIC_ENABLE tells the client to capture its own
// microphone at (BottomScreenStream.cpp) -- matches the rate
// EmuInstanceAudio.cpp's micOpen() sets for the Finlink mic input type, so
// no extra resampling is needed beyond micResample()'s existing internal
// conversion to the DS's native ~47743Hz mic rate.
constexpr uint32_t kMicSampleRate = 48000;
// 33513982 Hz is the DS's system clock (see RTC.cpp/Wifi.cpp for the same
// constant); GPU.cpp's FRAME_CYCLES is 355*6*263 cycles/frame, so
// 33513982 / (355*6*263) is the exact native refresh rate.
constexpr double kStreamFps = 33513982.0 / (355.0 * 6.0 * 263.0);

// video_mode: what the client requested (finlink's protocol.md "tiles"/
// "legacy"/"h264"/"h265", empty if unset/unrecognized) -- this stream type
// has no TILES/H264/H265 encoder, only ever sends a full raw frame (see
// BuildSessionReadyMessage()), so this is parsed only so the server can
// honestly report the fallback in session_ready.video_mode instead of
// silently ignoring the request. Never actually changes server behavior.
struct HandshakeAck
{
    int ProtocolVersion;
    int RequestedSlot;
    std::string VideoMode;
};

enum class HandshakeErrorCode
{
    VersionMismatch,
    SlotUnavailable,
    MalformedRequest,
};

// width/height default to kStreamWidth/kStreamHeight (the native
// resolution) -- callers pass the actual current capture resolution when
// known (BottomScreenStream tracks whatever CaptureBottomScreenBGRA8()
// last reported, which is upscaled when the OpenGL renderer is active).
// Purely informational: the client sizes its own decode buffers from each
// FINLINK_MSG_VIDEO frame's own width/height fields instead of trusting
// this value to stay accurate for the whole session.
std::string BuildHelloMessage(uint32_t width = kStreamWidth, uint32_t height = kStreamHeight);

// Parses a `hello_ack` text frame payload. Returns nullopt if the JSON is
// malformed or missing required fields -- caller should treat that as
// HandshakeErrorCode::MalformedRequest.
std::optional<HandshakeAck> ParseHelloAck(const std::vector<uint8_t>& payload);

std::string BuildSessionReadyMessage();

std::string BuildHandshakeErrorMessage(HandshakeErrorCode code, const std::string& detail);

}

#endif // MELONDS_STREAMING_FINLINKMESSAGES_H
