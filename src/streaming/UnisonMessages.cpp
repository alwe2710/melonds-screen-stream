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

#include "UnisonMessages.h"

#include <cstdio>
#include <cstring>
#include <sstream>

#include "unison/json.h"

namespace melonDS::Streaming
{

namespace
{

const char* ErrorCodeToString(HandshakeErrorCode code)
{
    switch (code)
    {
    case HandshakeErrorCode::VersionMismatch: return "version_mismatch";
    case HandshakeErrorCode::SlotUnavailable: return "slot_unavailable";
    case HandshakeErrorCode::MalformedRequest: return "malformed_request";
    }
    return "malformed_request";
}

// Escapes a string for embedding as a JSON string literal. Only `code`'s
// values (fixed literals above) and hardcoded `detail` text are ever passed
// through this in practice, but handshake_error's detail is meant to be
// human-readable free text, so this is defensive rather than provably
// unnecessary.
std::string JsonEscape(const std::string& in)
{
    std::string out;
    out.reserve(in.size() + 8);
    for (char c : in)
    {
        switch (c)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if ((unsigned char)c < 0x20)
            {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            }
            else
            {
                out += c;
            }
        }
    }
    return out;
}

// whole_object(): the top-level JSON object always spans the entire
// payload -- unison_json_find_member() skips leading whitespace itself, so
// passing (0, size) directly works without locating the braces by hand.
// Same trivial helper the Unison client-side code
// (core/src/handshake.c's own whole_object()) uses for the same reason.
unison_json_span WholeObject(size_t size)
{
    unison_json_span span;
    span.found = 1;
    span.start = 0;
    span.end = size;
    return span;
}

}

std::string BuildHelloMessage(uint32_t width, uint32_t height)
{
    std::ostringstream out;
    out.precision(10);
    out << "{"
        << "\"message\":\"hello\","
        << "\"protocol_version\":" << kProtocolVersion << ","
        << "\"stream_type\":\"" << kStreamType << "\","
        << "\"slots\":[{\"index\":0,\"label\":\"Bottom\",\"occupied\":false}],"
        << "\"video\":{"
        << "\"width\":" << width << ","
        << "\"height\":" << height << ","
        << "\"pixel_format\":\"rgb565\","
        << "\"fps\":" << kStreamFps
        << "},"
        << "\"input_encoding\":\"" << kInputEncoding << "\""
        << "}";
    return out.str();
}

std::optional<HandshakeAck> ParseHelloAck(const std::vector<uint8_t>& payload)
{
    if (payload.empty())
        return std::nullopt;

    const char* text = reinterpret_cast<const char*>(payload.data());
    const unison_json_span obj = WholeObject(payload.size());

    char message[16];
    if (unison_json_get_string(text, unison_json_find_member(text, obj.start, obj.end, "message"),
                                 message, sizeof(message)) == (size_t)-1)
        return std::nullopt;
    if (strcmp(message, "hello_ack") != 0)
        return std::nullopt;

    const unison_json_span versionSpan = unison_json_find_member(text, obj.start, obj.end, "protocol_version");
    const unison_json_span slotSpan = unison_json_find_member(text, obj.start, obj.end, "requested_slot");
    if (!versionSpan.found || !slotSpan.found)
        return std::nullopt;

    HandshakeAck ack;
    ack.ProtocolVersion = (int)unison_json_get_number(text, versionSpan);
    ack.RequestedSlot = (int)unison_json_get_number(text, slotSpan);
    // 16, not UNISON_VIDEO_MODE_LEN: src/unison/'s vendored unison_core
    // predates hello_ack.video_mode/UNISON_VIDEO_MODE_LEN existing at all
    // (it was last hand-synced from Unison's main branch, see
    // src/unison/README.md -- video_mode itself only exists on Unison's
    // still-unmerged transcoding branch). Matches the value
    // UNISON_VIDEO_MODE_LEN has on transcoding today; switch this to the
    // real constant once src/unison/ is re-synced past that point (same
    // situation as UnisonWebSocket.h's UNISON_WS_SEND_TIMEOUT_MS comment).
    char videoMode[16];
    if (unison_json_get_string(text, unison_json_find_member(text, obj.start, obj.end, "video_mode"), videoMode, sizeof(videoMode)) != (size_t)-1)
        ack.VideoMode = videoMode;
    return ack;
}

std::string BuildSessionReadyMessage()
{
    // Like azahar's equivalent: no real video negotiation for this stream
    // type (fixed 256x192, small enough that no realistic client's
    // video_limits would ever need to shrink it), no audio (this stream
    // type never sends console/speaker audio -- only mic input, which
    // isn't part of this negotiation, see UNISON_MSG_MIC_ENABLE), no
    // redirect (single slot).
    //
    // video_mode is always "legacy" regardless of what was requested in
    // hello_ack (see HandshakeAck::VideoMode's own comment) -- this stream
    // type has no TILES/H264/H265 encoder, only ever sends a full raw frame
    // (SendVideoFrame's hardcoded format=0, see BottomScreenStream.cpp).
    // Reporting the honest fallback here, per Unison's docs/protocol.md
    // "Video-mode fallback", is what lets a client that requested something
    // else show a fallback prompt instead of silently getting legacy video
    // with no explanation.
    std::ostringstream out;
    out.precision(10);
    out << "{"
        << "\"message\":\"session_ready\","
        << "\"slot\":0,"
        << "\"video\":{"
        << "\"width\":" << kStreamWidth << ","
        << "\"height\":" << kStreamHeight << ","
        << "\"fps\":" << kStreamFps
        << "},"
        << "\"video_mode\":\"legacy\""
        << "}";
    return out.str();
}

std::string BuildHandshakeErrorMessage(HandshakeErrorCode code, const std::string& detail)
{
    std::ostringstream out;
    out << "{"
        << "\"message\":\"handshake_error\","
        << "\"code\":\"" << ErrorCodeToString(code) << "\","
        << "\"detail\":\"" << JsonEscape(detail) << "\""
        << "}";
    return out.str();
}

}
