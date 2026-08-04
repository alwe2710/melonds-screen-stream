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

#ifndef MELONDS_STREAMING_FINLINKWEBSOCKET_H
#define MELONDS_STREAMING_FINLINKWEBSOCKET_H

// RFC6455 WebSocket transport for BottomScreenStream.cpp: reading/parsing
// the plain-HTTP upgrade request, computing Sec-WebSocket-Accept, sending
// unmasked server->client frames, and receiving masked client->server
// frames. Deliberately only the transport -- neither the app-level handshake
// (FinlinkMessages.h) nor the Video/Input binary message formats
// (BottomScreenStream.cpp) live here.
//
// Same wire format as the finlink WebSocket transport already implemented
// twice elsewhere (dolphin-gba-stream's GBAStreamWebSocket.h against SFML,
// azahar's websocket_transport.h against boost::asio) -- this is the third,
// against raw BSD/Winsock sockets, matching how src/debug/GdbStub.cpp (the
// one other TCP server in this codebase) talks to sockets directly rather
// than through a library, since melonDS has neither boost nor any other
// sockets abstraction available in core.

#ifdef _WIN32
#include <ws2tcpip.h>
#include <winsock.h>
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#define closesocket(x) close(x)
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstring>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "finlink/websocket.h"
#include "sha1/sha1.hpp"
#include "Base64.h"

namespace melonDS::Streaming
{

inline void SocketSetNonBlocking(int fd)
{
#ifdef _WIN32
    unsigned long mode = 1;
    ioctlsocket(fd, FIONBIO, &mode);
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags != -1)
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
}

inline void SocketSetNoDelay(int fd)
{
    int enable = 1;
#ifdef _WIN32
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&enable, sizeof(enable));
#else
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enable, sizeof(enable));
#endif
}

inline bool SocketWouldBlock()
{
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

struct HttpRequest
{
    std::string Path;
    std::map<std::string, std::string> Headers; // keys lowercased
};

// Reads and minimally parses one HTTP request (request line + headers) off
// a non-blocking socket. Returns nullopt on a malformed request, a dead
// connection, a 16 KiB size cap, or if `stop` becomes set while waiting.
inline std::optional<HttpRequest> ReadHttpRequest(int fd, const std::atomic_bool& stop)
{
    std::string request;
    std::array<char, 4096> buf{};
    while (request.find("\r\n\r\n") == std::string::npos && request.size() < 16384)
    {
        if (stop)
            return std::nullopt;
        int received = recv(fd, buf.data(), (int)buf.size(), 0);
        if (received > 0)
        {
            request.append(buf.data(), (size_t)received);
            continue;
        }
        if (received == 0)
            return std::nullopt; // Peer closed.
        if (SocketWouldBlock())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        return std::nullopt;
    }
    if (request.find("\r\n\r\n") == std::string::npos)
        return std::nullopt;

    HttpRequest result;
    std::istringstream stream(request);
    std::string requestLine;
    std::getline(stream, requestLine);
    {
        auto firstSpace = requestLine.find(' ');
        auto secondSpace =
            firstSpace == std::string::npos ? std::string::npos : requestLine.find(' ', firstSpace + 1);
        if (firstSpace != std::string::npos && secondSpace != std::string::npos)
            result.Path = requestLine.substr(firstSpace + 1, secondSpace - firstSpace - 1);
    }
    std::string line;
    while (std::getline(stream, line) && line != "\r" && !line.empty())
    {
        auto colon = line.find(':');
        if (colon == std::string::npos)
            continue;
        std::string key = line.substr(0, colon);
        std::string value = line.substr(colon + 1);
        while (!value.empty() && value.front() == ' ')
            value.erase(value.begin());
        while (!value.empty() && (value.back() == '\r' || value.back() == '\n'))
            value.pop_back();
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return std::tolower(c); });
        result.Headers[key] = value;
    }
    return result;
}

inline bool IsWebSocketUpgradeRequest(const HttpRequest& request)
{
    auto it = request.Headers.find("upgrade");
    if (it == request.Headers.end() || !request.Headers.count("sec-websocket-key"))
        return false;
    std::string upgrade = it->second;
    std::transform(upgrade.begin(), upgrade.end(), upgrade.begin(), [](unsigned char c) { return std::tolower(c); });
    return upgrade == "websocket";
}

// Sends `size` bytes on a non-blocking socket, retrying on would-block.
// Bounded so a stalled peer can't block this thread forever; `stop` is
// checked on every retry. 3 seconds turned out too tight in practice: a
// routine Wi-Fi hiccup (brief congestion, a roam between APs) can leave a
// send still buffered past that point even though the peer is still very
// much alive, and hitting the deadline kills the whole session (the client
// has to fully reconnect) rather than just this one frame arriving late --
// 10 seconds absorbs that without meaningfully changing behavior for an
// actually-dead peer, which was never going to un-stall in the next 7
// seconds either. finlink_core now has this as a named constant,
// FINLINK_WS_SEND_TIMEOUT_MS (core/include/finlink/websocket.h) -- not
// referenced directly here yet because src/finlink/ was last hand-synced
// from finlink's main branch (see src/finlink/README.md), which doesn't
// have that commit yet (it landed on finlink's transcoding branch, still
// unmerged as of this fix). Switch this literal to the constant on the
// next re-sync past that point.
inline bool SendAllBytes(int fd, const void* data, size_t size, const std::atomic_bool& stop)
{
    const auto* bytes = static_cast<const unsigned char*>(data);
    size_t sentTotal = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (sentTotal < size)
    {
        if (stop || std::chrono::steady_clock::now() > deadline)
            return false;
        int sent = send(fd, (const char*)(bytes + sentTotal), (int)(size - sentTotal), 0);
        if (sent > 0)
        {
            sentTotal += (size_t)sent;
            continue;
        }
        if (SocketWouldBlock())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        return false;
    }
    return true;
}

// Computes and sends the 101 Switching Protocols response. `request` must
// satisfy IsWebSocketUpgradeRequest().
inline bool SendWebSocketUpgradeResponse(int fd, const HttpRequest& request, const std::atomic_bool& stop)
{
    const std::string concatenated = request.Headers.at("sec-websocket-key") + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

    SHA1_CTX ctx;
    unsigned char digest[20];
    SHA1Init(&ctx);
    SHA1Update(&ctx, (const unsigned char*)concatenated.data(), (uint32_t)concatenated.size());
    SHA1Final(digest, &ctx);

    const std::string acceptB64 = Base64Encode(digest, sizeof(digest));

    std::ostringstream response;
    response << "HTTP/1.1 101 Switching Protocols\r\n"
             << "Upgrade: websocket\r\n"
             << "Connection: Upgrade\r\n"
             << "Sec-WebSocket-Accept: " << acceptB64 << "\r\n\r\n";
    const std::string responseStr = response.str();
    return SendAllBytes(fd, responseStr.data(), responseStr.size(), stop);
}

constexpr uint8_t WS_OPCODE_TEXT = 0x1;
constexpr uint8_t WS_OPCODE_BINARY = 0x2;

// Sends one unmasked, unfragmented server->client frame (server frames in
// this protocol are never masked).
inline bool SendWebSocketFrame(int fd, uint8_t opcode, const std::vector<uint8_t>& payload, const std::atomic_bool& stop)
{
    std::vector<uint8_t> frame;
    frame.reserve(payload.size() + 10);
    frame.push_back((uint8_t)(0x80 | (opcode & 0x0F))); // FIN=1, given opcode.

    const size_t len = payload.size();
    if (len < 126)
    {
        frame.push_back((uint8_t)len);
    }
    else if (len <= 0xFFFF)
    {
        frame.push_back(126);
        frame.push_back((uint8_t)((len >> 8) & 0xFF));
        frame.push_back((uint8_t)(len & 0xFF));
    }
    else
    {
        frame.push_back(127);
        for (int shift = 56; shift >= 0; shift -= 8)
            frame.push_back((uint8_t)(((uint64_t)len >> shift) & 0xFF));
    }
    frame.insert(frame.end(), payload.begin(), payload.end());

    return SendAllBytes(fd, frame.data(), frame.size(), stop);
}

inline bool SendWebSocketBinaryFrame(int fd, const std::vector<uint8_t>& payload, const std::atomic_bool& stop)
{
    return SendWebSocketFrame(fd, WS_OPCODE_BINARY, payload, stop);
}

inline bool SendWebSocketTextFrame(int fd, const std::string& payload, const std::atomic_bool& stop)
{
    return SendWebSocketFrame(fd, WS_OPCODE_TEXT, std::vector<uint8_t>(payload.begin(), payload.end()), stop);
}

struct ReceivedFrame
{
    finlink_ws_opcode Opcode;
    std::vector<uint8_t> Payload;
};

// Tries to parse one client->server (masked) frame from the front of `buf`
// via finlink_ws_parse_frame() (core/include/finlink/websocket.h, vendored
// at src/finlink/ -- its unmasking logic is generic despite being documented
// from a client's perspective, see that header's own comment), consuming
// those bytes from `buf` on success. Returns nullopt (leaving `buf`
// untouched) if there isn't a full frame yet; sets `*protocolError` if the
// frame was malformed/oversized/fragmented (buf is cleared in that case,
// caller should treat this as a disconnect).
inline std::optional<ReceivedFrame> TryParseOneFrame(std::vector<uint8_t>& buf, bool* protocolError)
{
    *protocolError = false;
    if (buf.empty())
        return std::nullopt;
    finlink_ws_frame frame{};
    auto status = finlink_ws_parse_frame(buf.data(), buf.size(), &frame);
    if (status == FINLINK_WS_FRAME_INCOMPLETE)
        return std::nullopt;
    if (status == FINLINK_WS_FRAME_ERR)
    {
        *protocolError = true;
        buf.clear();
        return std::nullopt;
    }
    ReceivedFrame result;
    result.Opcode = frame.opcode;
    result.Payload.assign(frame.payload, frame.payload + frame.payload_size);
    buf.erase(buf.begin(), buf.begin() + (long)frame.frame_size);
    return result;
}

// Reads off a non-blocking, already-upgraded socket until one full
// WebSocket frame has been received or `timeout` elapses. Used for the
// app-level handshake (FinlinkMessages.h), where exactly one text frame
// (hello_ack) is expected before any Video/Input binary frame.
inline std::optional<ReceivedFrame> ReceiveOneWebSocketFrame(int fd, const std::atomic_bool& stop,
                                                               std::chrono::milliseconds timeout)
{
    std::vector<uint8_t> recvBuffer;
    std::array<uint8_t, 4096> readBuf{};
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (stop)
            return std::nullopt;
        int received = recv(fd, (char*)readBuf.data(), (int)readBuf.size(), 0);
        if (received == 0)
            return std::nullopt; // Peer closed.
        if (received < 0)
        {
            if (SocketWouldBlock())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            return std::nullopt;
        }
        recvBuffer.insert(recvBuffer.end(), readBuf.begin(), readBuf.begin() + received);
        bool protocolError = false;
        auto frame = TryParseOneFrame(recvBuffer, &protocolError);
        if (frame)
            return frame;
        if (protocolError)
            return std::nullopt;
    }
    return std::nullopt;
}

}

#endif // MELONDS_STREAMING_FINLINKWEBSOCKET_H
