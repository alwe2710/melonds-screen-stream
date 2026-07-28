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

#ifndef MELONDS_STREAMING_BOTTOMSCREENSTREAM_H
#define MELONDS_STREAMING_BOTTOMSCREENSTREAM_H

// Server implementation for the NDS_BOTTOM_SCREEN finlink stream type: a
// single-slot WebSocket server that streams the DS bottom screen (256x192)
// to one remote client and accepts touch input back, per finlink's
// docs/protocol.md. Sibling implementation to azahar's own
// src/core/streaming/bottom_screen_stream.{h,cpp} (N3DS_BOTTOM_SCREEN) --
// same wire protocol, same overall shape, adapted to melonDS's much simpler
// architecture (no HLE service layer, no GPU-backend split: the software
// renderer's bottom-screen framebuffer is a plain, persistently-allocated
// RAM array, see GPU_Soft.h's Framebuffer[2][2]).
//
// Lifecycle: owned by NDS (see NDS::Stream / NDS::SetStreamingArgs()),
// mirroring the ARM/GdbStub ownership pattern -- constructed/destroyed
// whenever streaming is (re)configured via Settings, from
// EmuInstance::updateConsole() (Qt frontend) exactly like GDB stub args are.
//
// Threading: one background accept thread (std::thread, matching
// GPU3D_Soft.h's precedent for std::thread use in core), each accepted
// connection served on its own thread (mirroring the concurrency fix
// applied to the same feature's Dolphin/Azahar implementations: serving a
// connection inline on the accept thread would starve a second prospective
// client's connection attempt for the whole session's duration).
//
// Video capture: GPU::FinishFrame() (src/GPU.cpp) calls OnFrameEnd() once
// per emulated frame, ON THE EMU/CPU THREAD, right after the just-completed
// frame's buffers become the "front" buffer -- this is a plain memcpy, no
// render-thread-callback dance needed (unlike azahar's screenshot-API-based
// capture), since melonDS's software-rendered framebuffer is already a
// persistent RAM array rather than something requiring a GPU readback.
//
// Touch injection: deliberately NOT done from OnFrameEnd() or from the
// network thread directly -- NDS::TouchScreen()/ReleaseScreen() have no
// documented thread-safety guarantee against calls from the CPU-execution
// thread while the CPU is running, and the network thread is not that
// thread. Instead, GetTouchOverride() exposes the latest touch state (read
// off lock-free atomics, written by the network thread as n3ds_touch frames
// arrive) for the Qt frontend's own existing per-frame touch-apply site
// (EmuThread.cpp, which *is* the CPU-execution thread) to consume instead of
// local mouse input while a client is actively streaming -- narrow override,
// exactly mirroring azahar's own hid.cpp change for the same feature.

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace melonDS
{
class NDS;

namespace Streaming
{

struct TouchOverride
{
    bool Pressed;
    uint16_t X;
    uint16_t Y;
};

class BottomScreenStream
{
public:
    BottomScreenStream(melonDS::NDS& nds, uint16_t port);
    ~BottomScreenStream();

    BottomScreenStream(const BottomScreenStream&) = delete;
    BottomScreenStream& operator=(const BottomScreenStream&) = delete;

    // Called from GPU::FinishFrame(), on the emu thread, once per emulated
    // frame. `bottomBgra` points at 256*192 32-bit BGRA pixels (see
    // GPU_Soft.h's Framebuffer[frontbuf][1]) -- copied out immediately, not
    // retained past this call.
    void OnFrameEnd(const uint32_t* bottomBgra) noexcept;

    // nullopt whenever no client is in an active (post-session_ready)
    // session -- caller should fall back to local touch input in that case.
    [[nodiscard]] std::optional<TouchOverride> GetTouchOverride() const noexcept;

private:
    void AcceptLoop();
    // Runs on its own thread, one per accepted connection -- never called
    // inline from AcceptLoop() itself (see this file's top comment).
    void ServeConnection(int fd);
    void RunSession(int fd);

    melonDS::NDS& NDS;
    uint16_t Port;

    int ListenFd = -1;
    std::thread AcceptThread;
    std::atomic_bool Stop{false};

    std::mutex ConnectionThreadsMutex;
    std::vector<std::thread> ConnectionThreads;

    // Claimed by the one session currently allowed to stream (this stream
    // type has exactly one slot, see FinlinkMessages.cpp).
    std::atomic_bool Active{false};

    std::mutex FrameMutex;
    std::vector<uint8_t> LatestFrameBgra; // 256*192*4 bytes
    uint64_t FrameId = 0;

    std::atomic_bool Streaming{false}; // session_ready sent, touch override live
    std::atomic_bool TouchPressed{false};
    std::atomic<uint16_t> TouchX{0};
    std::atomic<uint16_t> TouchY{0};

#ifdef _WIN32
    bool WsaInitialized = false;
#endif
};

}
}

#endif // MELONDS_STREAMING_BOTTOMSCREENSTREAM_H
