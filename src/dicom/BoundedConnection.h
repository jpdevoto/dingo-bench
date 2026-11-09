// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <dcmtk/config/osconfig.h>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <cerrno>
#endif
#include <dcmtk/dcmnet/dcmtrans.h>

#include "dingobench/dicom/DiagnosticSession.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <stop_token>

namespace dingobench::dicom {

struct ClientPhase {
    void begin(int seconds)
    {
        deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        expired = false;
    }

    std::stop_token stop;
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max();
    bool expired = false;
    bool release = true;
    size_t readRemaining = std::numeric_limits<size_t>::max();
    bool readLimitReached = false;
    void limitRead(size_t bytes = std::numeric_limits<size_t>::max())
    {
        readRemaining = bytes;
        readLimitReached = false;
    }
};

// DCMTK owns the connected socket. Bound its reads by a monotonic phase deadline,
// including when a peer sends only part of a PDU, and check for cancellation.
class BoundedConnection final : public DcmTCPConnection {
public:
    BoundedConnection(DcmNativeSocketType socket, ClientPhase &phase, AssociationDetails *capture = nullptr)
        : DcmTCPConnection(socket), m_phase(phase), m_capture(capture)
    {
#ifdef _WIN32
        u_long enabled = 1;
        m_nonblocking = ioctlsocket(socket, FIONBIO, &enabled) == 0;
#else
        const int flags = fcntl(socket, F_GETFL, 0);
        m_nonblocking = flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
#ifdef SO_NOSIGPIPE
        const int enabled = 1;
        setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
    }

    OFBool networkDataAvailable(int timeout) override
    {
        return ready(false, timeout);
    }

    ssize_t read(void *buffer, size_t size) override
    {
        while (m_nonblocking && ready(false)) {
            const auto count = DcmTCPConnection::read(buffer, std::min(size, m_phase.readRemaining));
            if (count > 0)
                m_phase.readRemaining -= size_t(count);
            if (count >= 0 || !retryable())
                return count;
        }
        return 0;
    }

    ssize_t write(void *buffer, size_t size) override
    {
        auto *bytes = static_cast<char *>(buffer);
        size_t offset = 0;
        while (offset < size && m_nonblocking && ready(true)) {
#ifdef MSG_NOSIGNAL
            const auto written = send(getSocket(), bytes + offset, size - offset, MSG_NOSIGNAL);
#else
            const auto written = DcmTCPConnection::write(bytes + offset, size - offset);
#endif
            if (written > 0)
                offset += size_t(written);
            else if (!retryable())
                break;
        }
        if (m_capture && m_firstWrite && offset > 0) {
            // The first requestor write is the A-ASSOCIATE-RQ PDU.
            m_capture->requestSent = size >= 6 && offset == size &&
                static_cast<const unsigned char *>(buffer)[0] == 0x01;
            m_firstWrite = false;
        }
        if (offset == size)
            return static_cast<ssize_t>(offset);
        if (m_phase.expired || m_phase.stop.stop_requested()) {
#ifdef _WIN32
            WSASetLastError(WSAETIMEDOUT);
#else
            errno = ETIMEDOUT;
#endif
        }
        return -1;
    }

private:
    static bool retryable()
    {
#ifdef _WIN32
        const auto error = WSAGetLastError();
        return error == WSAEWOULDBLOCK || error == WSAEINTR;
#else
        return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
    }

    bool ready(bool writing, int timeout = -1)
    {
        if (!writing && m_phase.readRemaining == 0) {
            m_phase.readLimitReached = true;
            return false;
        }
        const auto localDeadline = timeout >= 0 ? std::chrono::steady_clock::now() + std::chrono::seconds(timeout)
                                                : m_phase.deadline;
        while (!m_phase.stop.stop_requested()) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= m_phase.deadline) {
                m_phase.expired = true;
                return false;
            }
            const auto remaining = std::min(localDeadline, m_phase.deadline) - now;
            const auto wait = std::clamp(std::chrono::ceil<std::chrono::milliseconds>(remaining), std::chrono::milliseconds(0), std::chrono::milliseconds(50));
            const auto socket = getSocket();
#ifdef _WIN32
            fd_set set;
            FD_ZERO(&set);
            FD_SET(socket, &set);
            timeval interval{0, static_cast<long>(wait.count() * 1000)};
            const int ready = select(0, writing ? nullptr : &set, writing ? &set : nullptr, nullptr, &interval);
            const bool interrupted = ready < 0 && WSAGetLastError() == WSAEINTR;
#else
            pollfd descriptor{socket, static_cast<short>(writing ? POLLOUT : POLLIN), 0};
            const int ready = poll(&descriptor, 1, static_cast<int>(wait.count()));
            const bool interrupted = ready < 0 && errno == EINTR;
#endif
            if (ready > 0)
                return true;
            if ((ready < 0 && !interrupted) || (ready == 0 && std::chrono::steady_clock::now() >= localDeadline)) {
                m_phase.expired = std::chrono::steady_clock::now() >= m_phase.deadline;
                return false;
            }
        }
        return false;
    }

    ClientPhase &m_phase;
    AssociationDetails *m_capture;
    bool m_firstWrite = true;
    bool m_nonblocking = false;
};

}  // namespace dingobench::dicom
