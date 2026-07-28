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
// Reusing "n3ds_touch" rather than a new "nds_touch" name: the wire format
// (u8 pressed + u16le x + u16le y) is identical, and the coordinate space is
// already documented as "whatever hello.video declares", not hardcoded to
// the 3DS's 320x240 -- see docs/protocol.md's WebSocket-binäre-Frames
// section.
constexpr char kInputEncoding[] = "n3ds_touch";
constexpr uint32_t kStreamWidth = 256;
constexpr uint32_t kStreamHeight = 192;
// 33513982 Hz is the DS's system clock (see RTC.cpp/Wifi.cpp for the same
// constant); GPU.cpp's FRAME_CYCLES is 355*6*263 cycles/frame, so
// 33513982 / (355*6*263) is the exact native refresh rate.
constexpr double kStreamFps = 33513982.0 / (355.0 * 6.0 * 263.0);

struct HandshakeAck
{
    int ProtocolVersion;
    int RequestedSlot;
};

enum class HandshakeErrorCode
{
    VersionMismatch,
    SlotUnavailable,
    MalformedRequest,
};

std::string BuildHelloMessage();

// Parses a `hello_ack` text frame payload. Returns nullopt if the JSON is
// malformed or missing required fields -- caller should treat that as
// HandshakeErrorCode::MalformedRequest.
std::optional<HandshakeAck> ParseHelloAck(const std::vector<uint8_t>& payload);

std::string BuildSessionReadyMessage();

std::string BuildHandshakeErrorMessage(HandshakeErrorCode code, const std::string& detail);

}

#endif // MELONDS_STREAMING_FINLINKMESSAGES_H
