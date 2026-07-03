#pragma once
#include <Api/SocketReceiver/SocketReceiverAcceptPolicy.hpp>
#include <Hermes/Socket/Async/AsyncListenerSocket.hpp>

#include <string_view>
#include <string>
#include <array>
#include <simd>


using Query = std::simd::basic_vec<float, std::simd::__deduce_abi_t<float, 16>>;


using ReceiverSocket = Hermes::AsyncListenerSocket<
    SocketReceiverData,
    SocketReceiverAcceptPolicy,
    Hermes::DefaultAsyncTransferPolicy<SocketReceiverData>>;
using ClientSocket = ReceiverSocket::ServerSocketType;


struct ClientState {
    ClientSocket client;
    std::array<char, 16384> buffer{};
    std::string socketView{};
    size_t bodyIdx{};
    size_t contentLength{};
    bool keepAlive{ true };
    std::string_view response{};
};