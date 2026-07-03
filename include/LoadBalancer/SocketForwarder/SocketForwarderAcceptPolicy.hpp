#pragma once
#include <LoadBalancer/SocketForwarder/SocketForwarderData.hpp>
#include <Hermes/Socket/Async/_base/ExecutionContext/FastIoExecutionContext.hpp>
#include <Hermes/Socket/_base.hpp>
#include <stdexec/execution.hpp>
#include <atomic>
#include <sys/socket.h>
#include <sys/un.h>

struct SocketForwarderAcceptPolicy {
    static constexpr auto Family{ SocketForwarderData::Family };
    static constexpr auto Type{ SocketForwarderData::Type };
    using EndpointType = SocketForwarderData::EndpointType;

    struct ListenOptions {
        Hermes::FastIoLoop* scheduler{};
        bool reuseAddress{ true };
        int recvBufferSize{};
        int sendBufferSize{};
    };

    struct AcceptOptions {
        Hermes::FastIoLoop* scheduler{};
        bool tcpNoDelay{ true };
        bool keepAlive{};
        int recvBufferSize{};
        int sendBufferSize{};
    };

    struct AcceptSender;
    struct ShutdownSender;

    static Hermes::ConnectionResultOper Listen(SocketForwarderData& data, int backlog, ListenOptions options);
    static AcceptSender Accept(SocketForwarderData& listenData, SocketForwarderData&& clientData, AcceptOptions options);
    static ShutdownSender Shutdown(SocketForwarderData& data);

    static void Close(SocketForwarderData& data) noexcept;
    static void Abort(SocketForwarderData& data) noexcept;

    static void S_SendFd(int unixSocket, int fdToSend) noexcept;

    inline static std::atomic<size_t> roundRobinCounter{0};
};

struct SocketForwarderAcceptPolicy::AcceptSender {
    using sender_concept = stdexec::sender_t;
    using completion_signatures = stdexec::completion_signatures<
        stdexec::set_value_t(SocketForwarderData),
        stdexec::set_error_t(Hermes::ConnectionErrorEnum),
        stdexec::set_stopped_t()
    >;

    SocketForwarderData* listenData;
    SocketForwarderData clientData;
    AcceptOptions options;

    template<class Receiver>
    struct OperationState {
        SocketForwarderData* listenData;
        SocketForwarderData clientData;
        AcceptOptions options;
        Receiver receiver;
        Hermes::TransferOperStatus status{};
        std::byte buffer[2 * (sizeof(sockaddr_storage) + 16)]{};
        socklen_t addrLen{ sizeof(sockaddr_storage) };

        OperationState(SocketForwarderData* lData, SocketForwarderData cData, AcceptOptions opts, Receiver recv) :
            listenData{ lData }, clientData{ std::move(cData) }, options{ opts }, receiver{ std::move(recv) } {}

        static void S_IoCallback(void* context, size_t bytesTransferred, bool success) noexcept {
            auto* self{ static_cast<OperationState*>(context) };

            if (!success || static_cast<int>(bytesTransferred) < 0) {
                stdexec::set_error(std::move(self->receiver), Hermes::ConnectionErrorEnum::ConnectionFailed);
                return;
            }

            self->clientData = self->listenData->MakeChild();
            self->clientData.socket = static_cast<Hermes::SocketFd>(static_cast<int>(bytesTransferred));

            if (!self->listenData->unixSockets.empty()) {
                size_t index = SocketForwarderAcceptPolicy::roundRobinCounter.fetch_add(1, std::memory_order_relaxed) % self->listenData->unixSockets.size();
                int targetUnixFd = static_cast<int>(self->listenData->unixSockets[index]);
                SocketForwarderAcceptPolicy::S_SendFd(targetUnixFd, static_cast<int>(self->clientData.socket));
            }

            stdexec::set_value(std::move(self->receiver), std::move(self->clientData));
        }

        void start() & noexcept {
            auto& self{ *this };
            self.status = {};
            self.status.context = this;
            self.status.callback = S_IoCallback;

            auto* loop = Hermes::FastIoLoop::GetLoopForSocket(static_cast<int>(self.listenData->socket));
            if (!loop) {
                stdexec::set_error(std::move(self.receiver), Hermes::ConnectionErrorEnum::NoScheduler);
                return;
            }

            loop->SubmitIo([&](struct io_uring_sqe* sqe) {
                self.addrLen = sizeof(sockaddr_storage);
                io_uring_prep_accept(sqe, static_cast<int>(self.listenData->socket),
                    reinterpret_cast<sockaddr*>(self.buffer), &self.addrLen, 0);
                io_uring_sqe_set_data(sqe, &self.status);
            });
        }
    };

    template<class Receiver>
    OperationState<Receiver> connect(Receiver r) && {
        return { listenData, std::move(clientData), options, std::move(r) };
    }
};

struct SocketForwarderAcceptPolicy::ShutdownSender {
    using sender_concept = stdexec::sender_t;
    using completion_signatures = stdexec::completion_signatures<
        stdexec::set_value_t(),
        stdexec::set_error_t(Hermes::ConnectionErrorEnum),
        stdexec::set_stopped_t()
    >;

    SocketForwarderData* data;

    template<class Receiver>
    struct OperationState {
        SocketForwarderData* data;
        Receiver receiver;

        void start() & noexcept {
            if (data->socket != macroINVALID_SOCKET) {
                shutdown(data->socket, SHUT_WR);
            }
            stdexec::set_value(std::move(receiver));
        }
    };

    template<class Receiver>
    OperationState<Receiver> connect(Receiver r) const {
        return { data, std::move(r) };
    }
};

inline SocketForwarderAcceptPolicy::AcceptSender SocketForwarderAcceptPolicy::Accept(SocketForwarderData& listenData, SocketForwarderData&& clientData, AcceptOptions options) {
    return AcceptSender{ &listenData, std::move(clientData), options };
}

inline SocketForwarderAcceptPolicy::ShutdownSender SocketForwarderAcceptPolicy::Shutdown(SocketForwarderData& data) {
    return ShutdownSender{ &data };
}

static_assert(Hermes::AsyncAcceptPolicyConcept<SocketForwarderAcceptPolicy, SocketForwarderData>);