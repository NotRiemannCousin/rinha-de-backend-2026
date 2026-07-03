#pragma once
#include <Hermes/Endpoint/IpEndpoint/IpEndpoint.hpp>
#include <Hermes/Socket/_base.hpp>

#include <chrono>
#include <vector>
#include <string>
#include <memory>
#include <array>

struct SocketForwarderData {
    using EndpointType = Hermes::IpEndpoint;

    static constexpr Hermes::SocketTypeEnum Type = Hermes::SocketTypeEnum::Stream;
    static constexpr Hermes::AddressFamilyEnum Family = Hermes::AddressFamilyEnum::Inet6;

    SocketForwarderData() = default;
    SocketForwarderData(Hermes::IpEndpoint tcpEndpoint, std::vector<std::string> paths);
    ~SocketForwarderData() = default;

    SocketForwarderData(SocketForwarderData&& other) noexcept;
    SocketForwarderData& operator=(SocketForwarderData&& other) noexcept;

    SocketForwarderData MakeChild() const;

    struct SocketOptions {
        std::chrono::milliseconds connectTimeout{ std::chrono::seconds{ 30 } };
        std::chrono::milliseconds sendTimeout{ std::chrono::seconds{ 10 } };
        std::chrono::milliseconds recvTimeout{ std::chrono::seconds{ 10 } };
    };

    struct State {
        std::array<std::byte, 0x4000> buffer{};
    };

    Hermes::IpEndpoint endpoint{};
    std::vector<std::string> unixPaths{};

    Hermes::SocketFd socket{ macroINVALID_SOCKET };
    std::vector<Hermes::SocketFd> unixSockets{};

    std::unique_ptr<State> state{};
    SocketOptions options{};
};

static_assert(Hermes::SocketDataConcept<SocketForwarderData>);