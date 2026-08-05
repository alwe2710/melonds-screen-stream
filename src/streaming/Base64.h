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

#ifndef MELONDS_STREAMING_BASE64_H
#define MELONDS_STREAMING_BASE64_H

// Minimal base64 encoder, only needed for the WebSocket handshake's
// Sec-WebSocket-Accept header (a base64(SHA1(...)) value, see
// UnisonWebSocket.h). core has no other use for base64 and no existing
// encoder (Qt's QByteArray::toBase64() would do this in one line, but core
// is deliberately kept Qt-free -- see src/unison/README.md and
// src/debug/GdbStub.cpp, which lives in core for the same reason).

#include <stddef.h>
#include <string>

namespace melonDS::Streaming
{

inline std::string Base64Encode(const unsigned char* data, size_t len)
{
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string out;
    out.reserve(((len + 2) / 3) * 4);

    size_t i = 0;
    while (i + 3 <= len)
    {
        unsigned int n = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out.push_back(table[(n >> 18) & 0x3F]);
        out.push_back(table[(n >> 12) & 0x3F]);
        out.push_back(table[(n >> 6) & 0x3F]);
        out.push_back(table[n & 0x3F]);
        i += 3;
    }

    size_t remaining = len - i;
    if (remaining == 1)
    {
        unsigned int n = data[i] << 16;
        out.push_back(table[(n >> 18) & 0x3F]);
        out.push_back(table[(n >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    }
    else if (remaining == 2)
    {
        unsigned int n = (data[i] << 16) | (data[i + 1] << 8);
        out.push_back(table[(n >> 18) & 0x3F]);
        out.push_back(table[(n >> 12) & 0x3F]);
        out.push_back(table[(n >> 6) & 0x3F]);
        out.push_back('=');
    }

    return out;
}

}

#endif // MELONDS_STREAMING_BASE64_H
