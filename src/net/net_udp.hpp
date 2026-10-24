#pragma once

#include <chrono>
#include <cstddef>
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

        return _sock.send(data.data(), data.size(), address, port) == sf::Socket::Done;
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

        sf::SocketSelector selector;
        selector.add(_sock);
        selector.wait(sf::milliseconds(static_cast<i32>(timeout.count())));
        if (!selector.isReady(_sock))
            return out;

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
            out.push_back(std::move(d));
        }

        return out;
    }

private:
    static constexpr size_t max_datagram_size = 65536;

    sf::UdpSocket _sock;
    bool          _open      = false;
    bool          _connected = false;
    sf::IpAddress _peer;
    u16           _peer_port = 0;
};

} // namespace dfdh::net
