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

// Server implementation for the NDS_BOTTOM_SCREEN Unison stream type: a
// single-slot WebSocket server that streams the DS bottom screen (256x192)
// to one remote client, and accepts touch, buttons, and microphone input
// back, per Unison's docs/protocol.md. Deliberately does NOT forward DS
// speaker audio to the client -- only video out, mic in. Sibling
// implementation to azahar's own src/core/streaming/bottom_screen_stream.
// {h,cpp} (N3DS_BOTTOM_SCREEN) and Cemu's WiiuGamepadStream (WIIU_GAMEPAD)
// -- same wire protocol, same overall shape, adapted to melonDS's much
// simpler architecture (no HLE service layer, no GPU-backend split: the
// software renderer's bottom-screen framebuffer is a plain, persistently-
// allocated RAM array, see GPU_Soft.h's Framebuffer[2][2]).
//
// Unlike Cemu/Azahar's mic forwarding, there's no "the game currently has
// the mic open" signal to gate UNISON_MSG_MIC_ENABLE on here -- melonDS's
// own Mic::FeedBuffer()/Platform::Mic_ReadInput() poll continuously
// regardless of what the game does with the samples (no MICStatus.isOpen-
// style state exists at this layer), matching the DS's own simpler
// hardware. RunSession sends MIC_ENABLE(1) once, right when streaming
// starts, and never needs to send MIC_ENABLE(0) before disconnect.
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
// Touch/button injection: deliberately NOT done from OnFrameEnd() or from
// the network thread directly -- NDS::TouchScreen()/ReleaseScreen()/
// SetKeyMask() have no documented thread-safety guarantee against calls
// from the CPU-execution thread while the CPU is running, and the network
// thread is not that thread. Instead, GetInputOverride() exposes the
// latest touch+button state (read off lock-free atomics, written by the
// network thread as n3ds_touch_and_buttons frames arrive) for the Qt
// frontend's own existing per-frame input-apply site (EmuThread.cpp, which
// *is* the CPU-execution thread) to consume instead of local mouse/
// keyboard input while a client is actively streaming -- narrow override,
// exactly mirroring azahar's own hid.cpp change for the same feature.
//
// Mic injection follows the same "network thread only ever writes a
// buffer, the frontend's own per-frame site is what actually calls into
// the emulated console" rule, for the same reason -- PollMicAudio() is
// drained by EmuThread.cpp too, which feeds it into EmuInstance's existing
// micResample()/micExtBuffer machinery (see EmuInstanceAudio.cpp), the
// same one the Qt frontend's own external-mic-device capture already uses.

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <unison/protocol.h>

#include "UnisonMessages.h"

namespace melonDS
{
class NDS;

namespace Streaming
{

class BottomScreenStream
{
public:
    BottomScreenStream(melonDS::NDS& nds, uint16_t port);
    ~BottomScreenStream();

    BottomScreenStream(const BottomScreenStream&) = delete;
    BottomScreenStream& operator=(const BottomScreenStream&) = delete;

    // Called from GPU::FinishFrame(), on the emu thread, once per emulated
    // frame. `bottomBgra` points at width*height 32-bit BGRA pixels, top-
    // down row-major (see GPU_Soft.h's Framebuffer[frontbuf][1], or
    // GLRenderer::CaptureBottomScreenBGRA8() for the OpenGL renderer's own
    // upscaled equivalent) -- copied out immediately, not retained past
    // this call. width/height vary with the active renderer's resolution
    // (native 256x192 for the software renderer, upscaled for the OpenGL
    // one), unlike the fixed kStreamWidth/kStreamHeight this used to
    // assume -- RunSession() reads back whatever was captured most
    // recently instead of hardcoding those constants.
    void OnFrameEnd(const uint32_t* bottomBgra, uint32_t width, uint32_t height) noexcept;

    // Touch + buttons, combined per Unison's "touch_and_buttons"
    // input_encoding (unison_touch_and_buttons, unison/protocol.h) --
    // a dedicated encoding with no stick fields at all, since the DS has
    // no analog stick (see UnisonMessages.h's own comment on why this
    // isn't "n3ds_touch_and_buttons"/unison_extended_input instead).
    // nullopt whenever no client is in an active (post-session_ready)
    // session -- caller should fall back to local touch/button input in
    // that case.
    [[nodiscard]] std::optional<unison_touch_and_buttons> GetInputOverride() const noexcept;

    // Current video-stream resolution -- whatever CaptureBottomScreenBGRA8()
    // (GPU_OpenGL.cpp)/GetFramebuffers() (GPU_Soft.cpp) last reported via
    // OnFrameEnd() above: native 256x192 for the software renderer, upscaled
    // for the OpenGL one. GetInputOverride()'s touch_x/touch_y are reported
    // by the client in THIS resolution's pixel space (it maps taps through
    // whatever frame size it's actually displaying, same as every other
    // stream type always has) -- callers scale back down to the DS's fixed
    // native 256x192 touchscreen resolution using this, which only actually
    // differs from it once the OpenGL renderer's scale factor is above 1x.
    void GetFrameDimensions(uint32_t& width, uint32_t& height) const noexcept;

    // Drains and returns whatever mic audio the client has sent since the
    // last call (never blocks) -- EmuInstance::micFeedUnisonAudio() (via
    // EmuThread.cpp's per-frame poll) drains this once per emulated frame.
    // Empty if nothing new has arrived. Native s16 samples, mono (the DS
    // mic, like the 3DS's, is mono-only).
    [[nodiscard]] std::vector<int16_t> PollMicAudio();

private:
    void AcceptLoop();
    // Runs on its own thread, one per accepted connection -- never called
    // inline from AcceptLoop() itself (see this file's top comment).
    void ServeConnection(int fd);
    // videoMode is whichever value ServeConnection() decided this session
    // will actually use ("h264"/"h265"/"legacy", see its own call site) --
    // RunSession() owns the matching SoftwareVideoEncoder as a session-local
    // (rebuilt if the captured frame size changes mid-session, e.g. a
    // renderer switch -- see SendVideoFrame()'s own comment), not a
    // BottomScreenStream member: encoder reference-frame state must never
    // cross sessions.
    void RunSession(int fd, const std::string& videoMode);

    // UDP discovery beacon (unison/discovery.h, docs/protocol.md's
    // "Discovery-Beacon (UDP)") -- broadcasts a unison_beacon JSON payload
    // on UNISON_BEACON_PORT every ~2s so the Android client's network
    // search can find this instance, mirroring Cemu's Beacon and azahar's
    // Core::Streaming::Beacon. Kept as a plain thread here (sharing `Stop`)
    // rather than a separate RAII object: ~BottomScreenStream() explicitly
    // stops/joins every socket-using thread in its body *before* the
    // WSACleanup() at the very end, and a separately-owned object torn down
    // via member-destruction order (which runs after the destructor body)
    // would call WSA socket functions after that cleanup on Windows.
    void BeaconLoop();
    std::string BuildBeaconMessage(const std::string& localHost) const;

    melonDS::NDS& NDS;
    uint16_t Port;

    int ListenFd = -1;
    std::thread AcceptThread;
    std::thread BeaconThread;
    std::atomic_bool Stop{false};

    std::mutex ConnectionThreadsMutex;
    std::vector<std::thread> ConnectionThreads;

    // Claimed by the one session currently allowed to stream (this stream
    // type has exactly one slot, see UnisonMessages.cpp).
    std::atomic_bool Active{false};

    mutable std::mutex FrameMutex;
    std::vector<uint8_t> LatestFrameBgra; // LatestFrameWidth*LatestFrameHeight*4 bytes
    uint32_t LatestFrameWidth = kStreamWidth;
    uint32_t LatestFrameHeight = kStreamHeight;
    uint64_t FrameId = 0;

    std::atomic_bool Streaming{false}; // session_ready sent, input override live
    std::atomic_bool TouchPressed{false};
    std::atomic<uint16_t> TouchX{0};
    std::atomic<uint16_t> TouchY{0};
    std::atomic<uint32_t> Buttons{0}; // unison_button_bit bits, see GetInputOverride()

    // Mic input pending delivery to EmuInstance::micFeedUnisonAudio() --
    // FIFO queue (not "latest wins"), same reasoning as the sibling
    // Cemu/Azahar implementations: dropping anything but a bounded backlog
    // would produce audible gaps.
    std::mutex MicMutex;
    std::vector<int16_t> PendingMicAudio;

#ifdef _WIN32
    bool WsaInitialized = false;
#endif
};

// Translates a unison_button_bit bitmask (protocol.h, active-high) into
// NDS::SetKeyMask()'s own bit layout (NDS.cpp: bits 0-9 = A,B,Select,Start,
// Right,Left,Up,Down,R,L; bits 10-11 = X,Y; active-LOW, 1 = released).
// ZL/ZR/HOME have no DS equivalent and are ignored.
uint32_t UnisonButtonsToNdsKeyMask(uint32_t unisonButtons) noexcept;

}
}

#endif // MELONDS_STREAMING_BOTTOMSCREENSTREAM_H
