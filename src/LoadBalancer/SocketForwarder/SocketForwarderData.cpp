#include <LoadBalancer/SocketForwarder/SocketForwarderData.hpp>
#include <utility>

SocketForwarderData::SocketForwarderData(Hermes::IpEndpoint tcpEndpoint, std::vector<std::string> paths)
    : endpoint{ std::move(tcpEndpoint) }, unixPaths{ std::move(paths) } {}

SocketForwarderData::SocketForwarderData(SocketForwarderData&& other) noexcept
    : endpoint{ std::move(other.endpoint) },
      unixPaths{ std::move(other.unixPaths) },
      socket{ std::exchange(other.socket, macroINVALID_SOCKET) },
      unixSockets{ std::move(other.unixSockets) },
      state{ std::move(other.state) },
      options{ std::move(other.options) } {}

SocketForwarderData& SocketForwarderData::operator=(SocketForwarderData&& other) noexcept {
    if (this != &other) {
        endpoint    = std::move(other.endpoint);
        unixPaths   = std::move(other.unixPaths);
        socket      = std::exchange(other.socket, macroINVALID_SOCKET);
        unixSockets = std::move(other.unixSockets);
        state       = std::move(other.state);
        options     = std::move(other.options);
    }
    return *this;
}

SocketForwarderData SocketForwarderData::MakeChild() const {
    return SocketForwarderData{ endpoint, unixPaths };
}