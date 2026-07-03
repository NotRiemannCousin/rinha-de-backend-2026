#include <Api/SocketReceiver/SocketReceiverData.hpp>
#include <utility>

SocketReceiverData::SocketReceiverData(std::string path)
    : unixPath{ std::move(path) } {}

SocketReceiverData::SocketReceiverData(SocketReceiverData&& other) noexcept
    : endpoint{ std::move(other.endpoint) },
      unixPath{ std::move(other.unixPath) },
      socket{ std::exchange(other.socket, macroINVALID_SOCKET) },
      unixSocket{ std::exchange(other.unixSocket, macroINVALID_SOCKET) },
      state{ std::move(other.state) },
      options{ std::move(other.options) } {}

SocketReceiverData& SocketReceiverData::operator=(SocketReceiverData&& other) noexcept {
    if (this != &other) {
        endpoint   = std::move(other.endpoint);
        unixPath   = std::move(other.unixPath);
        socket     = std::exchange(other.socket, macroINVALID_SOCKET);
        unixSocket = std::exchange(other.unixSocket, macroINVALID_SOCKET);
        state      = std::move(other.state);
        options    = std::move(other.options);
    }
    return *this;
}

SocketReceiverData SocketReceiverData::MakeChild() const {
    return SocketReceiverData{ unixPath };
}