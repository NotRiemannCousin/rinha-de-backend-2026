#pragma once
#include <Hermes/Endpoint/IpEndpoint/IpEndpoint.hpp>
#include <Hermes/Socket/_base.hpp>

#include <chrono>
#include <string>
#include <memory>
#include <array>

struct SocketReceiverData {
    using EndpointType = Hermes::IpEndpoint;

    static constexpr Hermes::SocketTypeEnum Type = Hermes::SocketTypeEnum::Stream;
    static constexpr Hermes::AddressFamilyEnum Family = Hermes::AddressFamilyEnum::Inet6;

    SocketReceiverData() = default;
    explicit SocketReceiverData(std::string path);
    ~SocketReceiverData() = default;

    SocketReceiverData(SocketReceiverData&& other) noexcept;
    SocketReceiverData& operator=(SocketReceiverData&& other) noexcept;

    SocketReceiverData MakeChild() const;

    struct SocketOptions {
        std::chrono::milliseconds connectTimeout{ std::chrono::seconds{ 30 } };
        std::chrono::milliseconds sendTimeout{ std::chrono::seconds{ 10 } };
        std::chrono::milliseconds recvTimeout{ std::chrono::seconds{ 10 } };
    };

    struct State {
        std::array<std::byte, 0x4000> buffer{};
    };

    Hermes::IpEndpoint endpoint{};
    std::string unixPath{};

    Hermes::SocketFd socket{ macroINVALID_SOCKET };
    Hermes::SocketFd unixSocket{ macroINVALID_SOCKET };

    std::unique_ptr<State> state{};
    SocketOptions options{};
};

static_assert(Hermes::SocketDataConcept<SocketReceiverData>);