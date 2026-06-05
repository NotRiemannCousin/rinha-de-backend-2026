#include <Hermes/Socket/Async/AsyncListenerSocket.hpp>
#include <Hermes/Socket/Async/_base/ExecutionContext/FastIoExecutionContext.hpp>
#include <Hermes/Endpoint/IpEndpoint/IpAddress.hpp>
#include <stdexec/execution.hpp>
#include <exec/repeat_until.hpp>
#include "SocketForwarderAcceptPolicy.hpp"
#include <vector>
#include <string>
#include <print>

using ForwarderSocket = Hermes::AsyncListenerSocket<
    SocketForwarderData,
    SocketForwarderAcceptPolicy,
    Hermes::DefaultAsyncTransferPolicy<SocketForwarderData>
>;

int main() {
    Hermes::FastIoLoop loop{ 1 }; // Inicia o scheduler

    SocketForwarderData data{
        Hermes::IpEndpoint{Hermes::IpAddress::FromIpv4({0, 0, 0, 0}), 9999},
        std::vector<std::string>{"/sockets/api1.sock", "/sockets/api2.sock"}
    };

    SocketForwarderAcceptPolicy::ListenOptions listenOpts{};
    listenOpts.scheduler = &loop;

    auto s_acceptConn = [&loop](auto& listener) {
        std::println("Load Balancer escutando.");

        SocketForwarderAcceptPolicy::AcceptOptions acceptOpts{};
        acceptOpts.scheduler = &loop;

        auto s_handleClient = [](auto&& clientSocket) {
            // Repasse SCM_RIGHTS já foi feito no Accept. Apenas fecha a representação local.
            clientSocket.Close();

            // Retorna false para o repeat_until() continuar em loop infinito
            return stdexec::just(false);
        };

        return listener.AsyncAcceptOne(acceptOpts)
             | stdexec::let_value(s_handleClient)
             | exec::repeat_until(); // <-- Modificado aqui
    };
    auto serve = ForwarderSocket::Listen(std::move(data), listenOpts)
               | stdexec::let_value(s_acceptConn)
               | stdexec::upon_error([](auto err) {
                     std::println("Erro no listener do Load Balancer.");
                 });

    // Inicia e bloqueia a thread
    stdexec::sync_wait(std::move(serve));

    return 0;
}