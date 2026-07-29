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

#include "BottomScreenStream.h"

#ifdef _WIN32
#include <ws2tcpip.h>
#include <winsock.h>
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#define closesocket(x) close(x)
#endif

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <sstream>

#include "finlink/deflate.h"
#include "finlink/discovery.h"
#include "finlink/endian.h"
#include "finlink/protocol.h"
#include "FinlinkMessages.h"
#include "FinlinkWebSocket.h"

#include "NDS.h"
#include "Platform.h"

using melonDS::Platform::Log;
using melonDS::Platform::LogLevel;

namespace melonDS::Streaming
{

namespace
{

void AppendU32LE(std::vector<uint8_t>& out, uint32_t value)
{
    out.push_back((uint8_t)(value & 0xFF));
    out.push_back((uint8_t)((value >> 8) & 0xFF));
    out.push_back((uint8_t)((value >> 16) & 0xFF));
    out.push_back((uint8_t)((value >> 24) & 0xFF));
}

// Converts melonDS's native software-renderer BGRA8 bottom-screen buffer
// into row-major u16le RGB565. No vertical flip: unlike a glReadPixels-based
// capture, GPU_Soft.h's Framebuffer[][1] is already top-down row-major (see
// this file's header comment).
void ConvertBgra8ToRgb565(const uint8_t* bgra8, uint32_t width, uint32_t height, std::vector<uint8_t>& outRgb565)
{
    outRgb565.resize((size_t)width * height * 2);
    const uint32_t pixelCount = width * height;
    for (uint32_t i = 0; i < pixelCount; i++)
    {
        const uint8_t b = bgra8[i * 4 + 0];
        const uint8_t g = bgra8[i * 4 + 1];
        const uint8_t r = bgra8[i * 4 + 2];
        const uint16_t pixel = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        outRgb565[i * 2 + 0] = (uint8_t)(pixel & 0xFF);
        outRgb565[i * 2 + 1] = (uint8_t)((pixel >> 8) & 0xFF);
    }
}

bool SendVideoFrame(int fd, const std::vector<uint8_t>& bgra8, uint32_t width, uint32_t height,
                    const std::atomic_bool& stop)
{
    std::vector<uint8_t> rgb565;
    ConvertBgra8ToRgb565(bgra8.data(), width, height, rgb565);

    std::vector<uint8_t> compressed(finlink_deflate_max_size(rgb565.size()));
    size_t compressedSize = 0;
    if (finlink_deflate_raw(rgb565.data(), rgb565.size(), compressed.data(), compressed.size(),
                             &compressedSize) != FINLINK_DEFLATE_OK)
    {
        Log(LogLevel::Error, "[Stream] failed to compress video frame\n");
        return false;
    }
    compressed.resize(compressedSize);

    std::vector<uint8_t> message;
    message.reserve(10 + compressed.size());
    message.push_back((uint8_t)FINLINK_MSG_VIDEO);
    AppendU32LE(message, width);
    AppendU32LE(message, height);
    message.push_back(0); // format = 0: full frame, raw (non-indexed, non-tiled) RGB565.
    message.insert(message.end(), compressed.begin(), compressed.end());

    return SendWebSocketBinaryFrame(fd, message, stop);
}

// UDP "connect" (nothing actually leaves the machine for a connectionless
// socket -- it just resolves local routing) to a well-known external
// address, then reads back which local interface/address the OS picked for
// that route. Doesn't require the address to be reachable, only routable --
// same trick azahar's Beacon::ProbeLocalHost() uses via boost::asio, ported
// to plain BSD/Winsock sockets since melonDS doesn't depend on boost.
std::string ProbeLocalHost()
{
    const int probeFd = socket(AF_INET, SOCK_DGRAM, 0);
    if (probeFd < 0)
        return std::string();

    struct sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(80);
    inet_pton(AF_INET, "8.8.8.8", &target.sin_addr);

    std::string result;
    if (connect(probeFd, (const struct sockaddr*)&target, sizeof(target)) == 0)
    {
        struct sockaddr_in local{};
        socklen_t len = sizeof(local);
        if (getsockname(probeFd, (struct sockaddr*)&local, &len) == 0)
        {
            char buf[INET_ADDRSTRLEN] = {};
            if (inet_ntop(AF_INET, &local.sin_addr, buf, sizeof(buf)))
                result = buf;
        }
    }
    closesocket(probeFd);
    return result;
}

// Escapes a string for embedding as a JSON string literal -- only ever used
// here for game_title, which in principle could contain arbitrary bytes
// from a malformed/homebrew ROM header, so this is defensive rather than
// provably unnecessary (mirrors FinlinkMessages.cpp's own JsonEscape(),
// duplicated here rather than shared since that one is file-local too).
std::string JsonEscapeBeaconString(const std::string& in)
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
                snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
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

}

uint32_t FinlinkButtonsToNdsKeyMask(uint32_t finlinkButtons) noexcept
{
    uint32_t mask = 0xFFF; // start with everything released (active-low)
    auto apply = [&](bool pressed, int ndsBit) {
        if (pressed)
            mask &= ~(1u << ndsBit);
    };
    apply(finlinkButtons & FINLINK_BUTTON_A, 0);
    apply(finlinkButtons & FINLINK_BUTTON_B, 1);
    apply(finlinkButtons & FINLINK_BUTTON_SELECT, 2);
    apply(finlinkButtons & FINLINK_BUTTON_START, 3);
    apply(finlinkButtons & FINLINK_BUTTON_RIGHT, 4);
    apply(finlinkButtons & FINLINK_BUTTON_LEFT, 5);
    apply(finlinkButtons & FINLINK_BUTTON_UP, 6);
    apply(finlinkButtons & FINLINK_BUTTON_DOWN, 7);
    apply(finlinkButtons & FINLINK_BUTTON_R, 8);
    apply(finlinkButtons & FINLINK_BUTTON_L, 9);
    apply(finlinkButtons & FINLINK_BUTTON_X, 10);
    apply(finlinkButtons & FINLINK_BUTTON_Y, 11);
    return mask;
}

BottomScreenStream::BottomScreenStream(melonDS::NDS& nds, uint16_t port) : NDS(nds), Port(port)
{
#ifdef _WIN32
    WSADATA wsa;
    WsaInitialized = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    if (!WsaInitialized)
    {
        Log(LogLevel::Error, "[Stream] winsock could not be initialized\n");
        return;
    }
#endif

    ListenFd = socket(AF_INET, SOCK_STREAM, 0);
    if (ListenFd < 0)
    {
        Log(LogLevel::Error, "[Stream] couldn't create listening socket\n");
        return;
    }

    int enable = 1;
#ifdef _WIN32
    setsockopt(ListenFd, SOL_SOCKET, SO_REUSEADDR, (const char*)&enable, sizeof(enable));
#else
    setsockopt(ListenFd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
#endif

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(Port);

    if (bind(ListenFd, (const struct sockaddr*)&addr, sizeof(addr)) < 0)
    {
        Log(LogLevel::Error, "[Stream] couldn't bind to port %d\n", Port);
        closesocket(ListenFd);
        ListenFd = -1;
        return;
    }
    if (listen(ListenFd, 5) < 0)
    {
        Log(LogLevel::Error, "[Stream] couldn't listen on port %d\n", Port);
        closesocket(ListenFd);
        ListenFd = -1;
        return;
    }
    SocketSetNonBlocking(ListenFd);

    AcceptThread = std::thread([this] { AcceptLoop(); });
    BeaconThread = std::thread([this] { BeaconLoop(); });

    Log(LogLevel::Info, "[Stream] bottom screen stream listening on port %d\n", Port);
}

BottomScreenStream::~BottomScreenStream()
{
    Stop = true;
    if (ListenFd >= 0)
        closesocket(ListenFd);
    if (AcceptThread.joinable())
        AcceptThread.join();
    if (BeaconThread.joinable())
        BeaconThread.join();

    std::vector<std::thread> threadsToJoin;
    {
        std::lock_guard lock(ConnectionThreadsMutex);
        threadsToJoin = std::move(ConnectionThreads);
    }
    for (auto& t : threadsToJoin)
    {
        if (t.joinable())
            t.join();
    }

#ifdef _WIN32
    if (WsaInitialized)
        WSACleanup();
#endif
}

void BottomScreenStream::OnFrameEnd(const uint32_t* bottomBgra, uint32_t width, uint32_t height) noexcept
{
    const size_t byteSize = (size_t)width * height * 4;
    std::lock_guard lock(FrameMutex);
    LatestFrameBgra.resize(byteSize);
    memcpy(LatestFrameBgra.data(), bottomBgra, byteSize);
    LatestFrameWidth = width;
    LatestFrameHeight = height;
    FrameId++;
}

std::optional<finlink_touch_and_buttons> BottomScreenStream::GetInputOverride() const noexcept
{
    if (!Streaming.load(std::memory_order_relaxed))
        return std::nullopt;
    finlink_touch_and_buttons result{};
    result.pressed = TouchPressed.load(std::memory_order_relaxed) ? 1 : 0;
    result.touch_x = TouchX.load(std::memory_order_relaxed);
    result.touch_y = TouchY.load(std::memory_order_relaxed);
    result.buttons = Buttons.load(std::memory_order_relaxed);
    return result;
}

std::vector<int16_t> BottomScreenStream::PollMicAudio()
{
    std::lock_guard lock(MicMutex);
    std::vector<int16_t> result = std::move(PendingMicAudio);
    PendingMicAudio.clear();
    return result;
}

void BottomScreenStream::AcceptLoop()
{
    if (ListenFd < 0)
        return;
    while (!Stop)
    {
        int fd = accept(ListenFd, nullptr, nullptr);
        if (fd >= 0)
        {
            SocketSetNonBlocking(fd);
            SocketSetNoDelay(fd);
            std::lock_guard lock(ConnectionThreadsMutex);
            ConnectionThreads.emplace_back([this, fd] { ServeConnection(fd); });
            continue;
        }
        if (!SocketWouldBlock())
            break; // Listening socket itself errored/closed -- stop.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

std::string BottomScreenStream::BuildBeaconMessage(const std::string& localHost) const
{
    std::string title = "melonDS";
    if (const NDSCart::CartCommon* cart = NDS.GetNDSCart())
    {
        const char* rawTitle = cart->GetHeader().GameTitle;
        const size_t titleLen = strnlen(rawTitle, sizeof(cart->GetHeader().GameTitle));
        if (titleLen > 0)
            title.assign(rawTitle, titleLen);
    }

    std::ostringstream out;
    out << "{"
        << "\"type\":\"finlink_beacon\","
        << "\"protocol_version\":" << kProtocolVersion << ","
        << "\"emulator_identifier\":\"melonDS\","
        << "\"game_title\":\"" << JsonEscapeBeaconString(title) << "\","
        << "\"stream_type\":\"" << kStreamType << "\","
        << "\"host\":\"" << localHost << "\","
        << "\"handshake_port\":" << Port
        << "}";
    return out.str();
}

void BottomScreenStream::BeaconLoop()
{
    const int beaconFd = socket(AF_INET, SOCK_DGRAM, 0);
    if (beaconFd < 0)
    {
        Log(LogLevel::Warn, "[Stream] couldn't create beacon socket, discovery disabled\n");
        return;
    }

    int broadcastEnable = 1;
#ifdef _WIN32
    setsockopt(beaconFd, SOL_SOCKET, SO_BROADCAST, (const char*)&broadcastEnable, sizeof(broadcastEnable));
#else
    setsockopt(beaconFd, SOL_SOCKET, SO_BROADCAST, &broadcastEnable, sizeof(broadcastEnable));
#endif

    const std::string localHost = ProbeLocalHost();

    struct sockaddr_in broadcastAddr{};
    broadcastAddr.sin_family = AF_INET;
    broadcastAddr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    broadcastAddr.sin_port = htons((uint16_t)FINLINK_BEACON_PORT);

    while (!Stop)
    {
        const std::string message = BuildBeaconMessage(localHost);
        // Best-effort: a dropped/failed broadcast just means this tick's
        // beacon didn't go out, no different from ordinary UDP loss -- the
        // next tick covers for it.
        sendto(beaconFd, message.data(), (int)message.size(), 0,
               (const struct sockaddr*)&broadcastAddr, sizeof(broadcastAddr));

        // Polls Stop every 100ms instead of sleeping the full interval in
        // one call, so the destructor doesn't have to wait out an
        // in-progress interval.
        for (int waitedMs = 0; waitedMs < 2000 && !Stop; waitedMs += 100)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    closesocket(beaconFd);
}

void BottomScreenStream::ServeConnection(int fd)
{
    const auto request = ReadHttpRequest(fd, Stop);
    if (!request || !IsWebSocketUpgradeRequest(*request))
    {
        closesocket(fd);
        return;
    }
    if (!SendWebSocketUpgradeResponse(fd, *request, Stop))
    {
        closesocket(fd);
        return;
    }
    uint32_t helloWidth, helloHeight;
    {
        std::lock_guard lock(FrameMutex);
        helloWidth = LatestFrameWidth;
        helloHeight = LatestFrameHeight;
    }
    if (!SendWebSocketTextFrame(fd, BuildHelloMessage(helloWidth, helloHeight), Stop))
    {
        closesocket(fd);
        return;
    }

    const auto frame = ReceiveOneWebSocketFrame(fd, Stop, std::chrono::seconds(5));
    if (!frame || frame->Opcode != FINLINK_WS_OPCODE_TEXT)
    {
        closesocket(fd);
        return;
    }

    const auto ack = ParseHelloAck(frame->Payload);
    if (!ack)
    {
        SendWebSocketTextFrame(
            fd, BuildHandshakeErrorMessage(HandshakeErrorCode::MalformedRequest, "Malformed hello_ack"), Stop);
        closesocket(fd);
        return;
    }
    if (ack->ProtocolVersion != kProtocolVersion)
    {
        SendWebSocketTextFrame(
            fd, BuildHandshakeErrorMessage(HandshakeErrorCode::VersionMismatch, "Protocol version mismatch"), Stop);
        closesocket(fd);
        return;
    }

    bool expected = false;
    if (!Active.compare_exchange_strong(expected, true))
    {
        SendWebSocketTextFrame(fd,
                                BuildHandshakeErrorMessage(HandshakeErrorCode::SlotUnavailable,
                                                            "Bottom screen stream already has an active client"),
                                Stop);
        closesocket(fd);
        return;
    }

    if (!SendWebSocketTextFrame(fd, BuildSessionReadyMessage(), Stop))
    {
        Active = false;
        closesocket(fd);
        return;
    }

    RunSession(fd);

    Streaming = false;
    TouchPressed = false;
    Buttons = 0;
    Active = false;
    // Drop any mic audio this client sent but nobody drained yet -- left
    // sitting here, it would otherwise get fed to
    // EmuInstance::micFeedFinlinkAudio() as if it were fresh once a later
    // session (or a belated poll from this one) reads it, mislabeling
    // stale audio as current.
    {
        std::lock_guard lock(MicMutex);
        PendingMicAudio.clear();
    }
    closesocket(fd);
}

void BottomScreenStream::RunSession(int fd)
{
    Streaming = true;
    uint64_t lastSentFrameId = 0;
    std::vector<uint8_t> recvBuffer;
    std::array<uint8_t, 4096> readBuf{};

    // Mic input has no "the game currently wants it" signal to gate on
    // here (see this file's header comment) -- send the enable signal once
    // up front rather than tracking an edge-detected want-state like
    // Cemu/Azahar do for their own mic forwarding.
    {
        finlink_mic_enable enable;
        enable.enabled = 1;
        enable.sample_rate = kMicSampleRate;
        uint8_t payload[FINLINK_MIC_ENABLE_FRAME_SIZE];
        finlink_build_mic_enable_frame(&enable, payload);
        std::vector<uint8_t> message(payload, payload + FINLINK_MIC_ENABLE_FRAME_SIZE);
        if (!SendWebSocketBinaryFrame(fd, message, Stop))
            return;
    }

    while (!Stop)
    {
        std::vector<uint8_t> frameCopy;
        uint32_t frameWidth = 0, frameHeight = 0;
        uint64_t currentId = 0;
        {
            std::lock_guard lock(FrameMutex);
            currentId = FrameId;
            if (currentId != lastSentFrameId)
            {
                frameCopy = LatestFrameBgra;
                frameWidth = LatestFrameWidth;
                frameHeight = LatestFrameHeight;
            }
        }
        if (!frameCopy.empty())
        {
            if (!SendVideoFrame(fd, frameCopy, frameWidth, frameHeight, Stop))
                return;
            lastSentFrameId = currentId;
        }

        int received = recv(fd, (char*)readBuf.data(), (int)readBuf.size(), 0);
        if (received == 0)
            return; // Disconnected.
        if (received < 0 && !SocketWouldBlock())
            return; // Error.
        if (received > 0)
        {
            recvBuffer.insert(recvBuffer.end(), readBuf.begin(), readBuf.begin() + received);
            for (;;)
            {
                bool protocolError = false;
                auto parsed = TryParseOneFrame(recvBuffer, &protocolError);
                if (!parsed)
                {
                    if (protocolError)
                        return;
                    break;
                }
                if (parsed->Opcode == FINLINK_WS_OPCODE_CLOSE)
                    return;
                if (parsed->Opcode != FINLINK_WS_OPCODE_BINARY)
                    continue;

                finlink_msg_type type;
                if (finlink_peek_type(parsed->Payload.data(), parsed->Payload.size(), &type) != FINLINK_OK)
                    continue;
                if (type == FINLINK_MSG_INPUT)
                {
                    finlink_touch_and_buttons input{};
                    if (finlink_parse_touch_and_buttons_frame(parsed->Payload.data(), parsed->Payload.size(),
                                                               &input) == FINLINK_OK)
                    {
                        TouchPressed = input.pressed != 0;
                        TouchX = input.touch_x;
                        TouchY = input.touch_y;
                        Buttons = input.buttons;
                    }
                }
                else if (type == FINLINK_MSG_MIC_AUDIO)
                {
                    finlink_audio_frame audio{};
                    if (finlink_parse_mic_audio_frame(parsed->Payload.data(), parsed->Payload.size(),
                                                       &audio) == FINLINK_OK)
                    {
                        std::lock_guard lock(MicMutex);
                        // PollMicAudio()/EmuInstance::micFeedFinlinkAudio()
                        // only ever see raw sample bytes, not a rate -- they
                        // trust the client to always send at kMicSampleRate,
                        // the only rate ever advertised via MIC_ENABLE here.
                        // Reject anything else rather than silently mixing
                        // differently-rated audio into one buffer that gets
                        // fed as if it were all kMicSampleRate.
                        if (audio.sample_rate != kMicSampleRate)
                            continue;
                        // ~2s cap at typical mic rates (mono) -- drop the
                        // backlog rather than let it grow unboundedly if
                        // EmuThread.cpp's per-frame drain ever falls behind
                        // this far (same tradeoff WiiuGamepadStream::
                        // SubmitGamepadAudio() makes in Cemu).
                        constexpr size_t kMaxPendingSamples = 48000 * 2;
                        if (PendingMicAudio.size() + audio.sample_count > kMaxPendingSamples)
                            PendingMicAudio.clear();
                        for (size_t i = 0; i < audio.sample_count; i++)
                            PendingMicAudio.push_back(finlink_read_s16le(audio.samples + i * 2));
                    }
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
}

}
