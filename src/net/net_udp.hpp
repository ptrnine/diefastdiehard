#pragma once

#include <chrono>
#include <cstddef>
#include <random>
#include <span>
#include <vector>

#include <SFML/Network/UdpSocket.hpp>
#include <SFML/Network/SocketSelector.hpp>
#include <SFML/Network/IpAddress.hpp>

#include "base/types.hpp"
#include "net_protocol.hpp"

namespace dfdh::net {

/**
 * A single received datagram (copied, always valid)
 */
struct received_datagram {
    sf::IpAddress   address;
    u16             port      = 0;
    std::vector<u8> data;
};

/**
 * A small RAII wrapper around an sf::UdpSocket.
 *
 * - bind(port) opens the socket and binds it (port 0 = ephemeral)
 * - connect(address, port) sets the peer for connected sends
 * - recv(timeout) drains all pending datagrams, blocking up to the
 *   given timeout for the first datagram
 */
class udp_channel {
public:
    udp_channel() = default;

    /* Open the socket and bind to the given port (0 = ephemeral) */
    bool bind(u16 port) {
        if (_open)
            return false;

        if (_sock.bind(port) != sf::Socket::Done)
            return false;

        _sock.setBlocking(false);
        _open = true;
        return true;
    }

    /* Set the peer for connected sends (SFML 2.5 has no udp connect; the peer is tracked here) */
    bool connect(const sf::IpAddress& address, u16 port) {
        /* Auto-open on an ephemeral port if the socket is not bound yet */
        if (!_open && !bind(0))
            return false;

        _peer      = address;
        _peer_port = port;
        _connected = true;
        return true;
    }

    [[nodiscard]]
    bool is_open() const {
        return _open;
    }

    [[nodiscard]]
    u16 local_port() const {
        return _sock.getLocalPort();
    }

    bool send_to(const sf::IpAddress& address, u16 port, const std::span<const u8>& data) {
        if (!_open || data.empty())
            return false;

#if defined(DFDH_DEBUG_PING)
        /* Simulated latency: queue the datagram and let it go out only after
         * its one-way delay has elapsed */
        flush_due_sends();
        _pending_sends.push_back({std::chrono::steady_clock::now() + one_way_delay(),
                                  address, port, std::vector<u8>(data.begin(), data.end())});
        return true;
#else
        return _sock.send(data.data(), data.size(), address, port) == sf::Socket::Done;
#endif
    }

    /* Send to the connected peer (connect() required) */
    bool send(const std::span<const u8>& data) {
        if (!_connected)
            return false;

        return send_to(_peer, _peer_port, data);
    }

    /* Drain all pending datagrams, blocking up to the timeout for the first one */
    std::vector<received_datagram> recv(std::chrono::milliseconds timeout) {
        std::vector<received_datagram> out;
        if (!_open)
            return out;

#if defined(DFDH_DEBUG_PING)
        flush_due_sends();
#endif

        sf::SocketSelector selector;
        selector.add(_sock);
        selector.wait(sf::milliseconds(static_cast<i32>(timeout.count())));
        if (!selector.isReady(_sock))
#if defined(DFDH_DEBUG_PING)
            return release_due_recvs();
#else
            return out;
#endif

        for (;;) {
            size_t recv_size = 0;
            u8 data[max_datagram_size];
            sf::IpAddress addr;
            u16 port = 0;
            auto status = _sock.receive(data, sizeof(data), recv_size, addr, port);
            if (status != sf::Socket::Done || recv_size == 0)
                break;

            received_datagram d;
            d.address = addr;
            d.port    = port;
            d.data    = std::vector<u8>(data, data + recv_size);
#if defined(DFDH_DEBUG_PING)
            /* Simulated latency: deliver the datagram only after its
             * one-way delay has elapsed */
            _held_recvs.push_back({std::chrono::steady_clock::now() + one_way_delay(),
                                   std::move(d)});
#else
            out.push_back(std::move(d));
#endif
        }

#if defined(DFDH_DEBUG_PING)
        return release_due_recvs();
#else
        return out;
#endif
    }

private:
    static constexpr size_t max_datagram_size = 65536;

#if defined(DFDH_DEBUG_PING)
    /*
     * Simulated network latency (DFDH_DEBUG_PING): every one-way hop
     * (client->server, server->client) is delayed by a random duration
     * between 80ms and 170ms (average ~125ms), so the round-trip ping
     * between client and server is around 250ms
     */
    static constexpr double one_way_delay_min_ms = 80.0;
    static constexpr double one_way_delay_max_ms = 170.0;

    struct delayed_send {
        std::chrono::steady_clock::time_point deadline;
        sf::IpAddress          address;
        u16                    port = 0;
        std::vector<u8>        data;
    };

    struct delayed_recv {
        std::chrono::steady_clock::time_point deadline;
        received_datagram    datagram;
    };

    std::chrono::steady_clock::duration one_way_delay() {
        std::uniform_real_distribution<double> dist(one_way_delay_min_ms, one_way_delay_max_ms);
        return std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double, std::milli>(dist(_ping_rng)));
    }

    /* Send every queued datagram whose simulated deadline has passed */
    void flush_due_sends() {
        auto now = std::chrono::steady_clock::now();
        auto it  = _pending_sends.begin();
        while (it != _pending_sends.end()) {
            if (it->deadline <= now) {
                _sock.send(it->data.data(), it->data.size(), it->address, it->port);
                it = _pending_sends.erase(it);
            }
            else {
                ++it;
            }
        }
    }

    /* Take every received datagram whose simulated delivery time has passed */
    std::vector<received_datagram> release_due_recvs() {
        std::vector<received_datagram> due;
        auto now = std::chrono::steady_clock::now();
        auto it  = _held_recvs.begin();
        while (it != _held_recvs.end()) {
            if (it->deadline <= now) {
                due.push_back(std::move(it->datagram));
                it = _held_recvs.erase(it);
            }
            else {
                ++it;
            }
        }
        return due;
    }

    std::mt19937              _ping_rng{std::random_device{}()};
    std::vector<delayed_send> _pending_sends;
    std::vector<delayed_recv> _held_recvs;
#endif

    sf::UdpSocket _sock;
    bool          _open      = false;
    bool          _connected = false;
    sf::IpAddress _peer;
    u16           _peer_port = 0;
};

} // namespace dfdh::net
