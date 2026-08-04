// FinlinkMessages.{h,cpp} had zero test coverage before this file, despite
// being exactly where hello_ack.video_mode gets parsed and
// session_ready.video_mode gets reported -- the two fields the "Video-mode
// fallback" negotiation feature (finlink/docs/protocol.md) actually runs
// on. Standalone, deliberately not wired into melonDS's own (huge) CMake
// build: FinlinkMessages.cpp only depends on finlink/json.h's span parser
// (see its own header comment, "no socket I/O"), so this links against
// just that, not the rest of melonDS -- see tests/CMakeLists.txt.
//
// Uses the top-level finlink/core (not src/finlink/'s own vendored, hand-
// synced copy) for the round-trip assertions below: that vendored copy
// still predates hello_ack.video_mode/FINLINK_VIDEO_MODE_LEN entirely
// (see FinlinkMessages.cpp's own comment on why ParseHelloAck() uses a
// literal 16 instead of the real constant), so it can't even declare
// finlink_session_ready.video_mode to parse into. Testing this fork's own
// message-building logic against the current, correct protocol shape is
// the more useful reference regardless -- src/finlink/'s own staleness is
// a separate, already-tracked issue (its own README's re-sync note).

#include "../FinlinkMessages.h"

#include "finlink/handshake.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CHECK(cond)                                                           \
    do                                                                        \
    {                                                                         \
        if (!(cond))                                                         \
        {                                                                     \
            std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                    \
        }                                                                     \
    } while (0)

using namespace melonDS::Streaming;

namespace
{

std::vector<uint8_t> ToBytes(const std::string& s)
{
    return std::vector<uint8_t>(s.begin(), s.end());
}

void TestBuildHelloMessage()
{
    const std::string hello = BuildHelloMessage();
    CHECK(hello.find("\"message\":\"hello\"") != std::string::npos);
    CHECK(hello.find("\"stream_type\":\"NDS_BOTTOM_SCREEN\"") != std::string::npos);
    CHECK(hello.find("\"width\":256") != std::string::npos);
    CHECK(hello.find("\"height\":192") != std::string::npos);
    CHECK(hello.find("\"input_encoding\":\"touch_and_buttons\"") != std::string::npos);
    // No "audio" member at all for this stream type (see BuildHelloMessage's
    // own comment on FINLINK_MSG_MIC_ENABLE being separate).
    CHECK(hello.find("\"audio\"") == std::string::npos);

    finlink_hello parsed;
    CHECK(finlink_parse_hello(reinterpret_cast<const uint8_t*>(hello.data()), hello.size(), &parsed) ==
          FINLINK_HANDSHAKE_OK);
    CHECK(parsed.protocol_version == kProtocolVersion);
    CHECK(std::strcmp(parsed.stream_type, kStreamType) == 0);
    CHECK(!parsed.has_audio);

    // The overload's actual point: an upscaled capture resolution
    // (BottomScreenStream tracks the real current one) must flow through,
    // not always report the fixed native default.
    const std::string upscaled = BuildHelloMessage(512, 384);
    CHECK(upscaled.find("\"width\":512") != std::string::npos);
    CHECK(upscaled.find("\"height\":384") != std::string::npos);
}

void TestParseHelloAckVideoMode()
{
    // No video_mode field at all -- must stay empty (this fork's
    // HandshakeAck has no default-to-"tiles" behavior the way Cemu's does;
    // it's parsed purely for reporting, see FinlinkMessages.h's own
    // comment), not garbage/uninitialized.
    const auto no_mode =
        ParseHelloAck(ToBytes(R"({"message":"hello_ack","protocol_version":2,"requested_slot":0})"));
    CHECK(no_mode.has_value());
    CHECK(no_mode->ProtocolVersion == 2);
    CHECK(no_mode->RequestedSlot == 0);
    CHECK(no_mode->VideoMode.empty());

    // Unlike Cemu's ParseHelloAck (which whitelists "legacy"/"h264"/"h265"
    // and falls back to "tiles" otherwise), this one copies whatever was
    // sent verbatim -- it's never actually acted on either way (see
    // BuildSessionReadyMessage()'s own comment), only carried through for
    // an honest report. Confirm that's really the current behavior rather
    // than assuming it matches Cemu's stricter validation.
    for (const char* mode : {"tiles", "legacy", "h264", "h265", "vp9"})
    {
        std::string json = R"({"message":"hello_ack","protocol_version":2,"requested_slot":0,"video_mode":")";
        json += mode;
        json += "\"}";
        const auto ack = ParseHelloAck(ToBytes(json));
        CHECK(ack.has_value());
        CHECK(ack->VideoMode == mode);
    }
}

void TestParseHelloAckRejectsMalformed()
{
    CHECK(!ParseHelloAck({}).has_value());
    CHECK(!ParseHelloAck(ToBytes(R"({"message":"session_ready"})")).has_value());
    CHECK(!ParseHelloAck(ToBytes(R"({"message":"hello_ack","requested_slot":0})")).has_value());
}

void TestBuildSessionReadyMessageAlwaysLegacy()
{
    // The whole point of this fork's fallback story: video_mode is always
    // "legacy" in the reply, unconditionally -- there's no way to make
    // this fork claim tiles/h264/h265, since it genuinely can't send any
    // of those (see BuildSessionReadyMessage()'s own comment).
    const std::string ready_json = BuildSessionReadyMessage();
    CHECK(ready_json.find("\"message\":\"session_ready\"") != std::string::npos);

    finlink_session_ready parsed;
    CHECK(finlink_parse_session_ready(reinterpret_cast<const uint8_t*>(ready_json.data()), ready_json.size(),
                                       &parsed) == FINLINK_HANDSHAKE_OK);
    CHECK(std::strcmp(parsed.video_mode, "legacy") == 0);
    CHECK(parsed.video.width == kStreamWidth && parsed.video.height == kStreamHeight);
    // No audio, no redirect -- see BuildSessionReadyMessage()'s own comment.
    CHECK(!parsed.has_audio);
    CHECK(!parsed.has_redirect);
}

void TestBuildHandshakeErrorMessage()
{
    const std::string err_json =
        BuildHandshakeErrorMessage(HandshakeErrorCode::MalformedRequest, R"(bad "field" \ value)");

    finlink_handshake_error parsed;
    CHECK(finlink_parse_handshake_error(reinterpret_cast<const uint8_t*>(err_json.data()), err_json.size(),
                                         &parsed) == FINLINK_HANDSHAKE_OK);
    CHECK(std::strcmp(parsed.code, "malformed_request") == 0);
    CHECK(std::strcmp(parsed.detail, R"(bad "field" \ value)") == 0);
}

} // namespace

int main()
{
    TestBuildHelloMessage();
    TestParseHelloAckVideoMode();
    TestParseHelloAckRejectsMalformed();
    TestBuildSessionReadyMessageAlwaysLegacy();
    TestBuildHandshakeErrorMessage();
    std::printf("finlink_messages: all tests passed\n");
    return 0;
}
