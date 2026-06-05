#pragma once
#include "SocketReceiverData.hpp"
#include <Hermes/Socket/Async/_base/ExecutionContext/FastIoExecutionContext.hpp>
#include <Hermes/Socket/_base.hpp>
#include <stdexec/execution.hpp>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>

struct SocketReceiverAcceptPolicy {
    static constexpr auto Family{ SocketReceiverData::Family };
    static constexpr auto Type{ SocketReceiverData::Type };
    using EndpointType = SocketReceiverData::EndpointType;

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

    static Hermes::ConnectionResultOper Listen(SocketReceiverData& data, int backlog, ListenOptions options);
    static AcceptSender Accept(SocketReceiverData& listenData, AcceptOptions options);
    static ShutdownSender Shutdown(SocketReceiverData& data);

    static void Close(SocketReceiverData& data) noexcept;
    static void Abort(SocketReceiverData& data) noexcept;

    static int S_RecvFd(int unixSocket) noexcept;
};

struct SocketReceiverAcceptPolicy::AcceptSender {
    using sender_concept = stdexec::sender_t;
    using completion_signatures = stdexec::completion_signatures<
        stdexec::set_value_t(SocketReceiverData),
        stdexec::set_error_t(Hermes::ConnectionErrorEnum)
    >;
    
    SocketReceiverData* listenData;
    AcceptOptions options;

    template<class Receiver>
    struct OperationState {
        SocketReceiverData* listenData;
        SocketReceiverData clientData;
        AcceptOptions options;
        Receiver receiver;
        Hermes::TransferOperStatus status{};

        OperationState(SocketReceiverData* data, AcceptOptions opts, Receiver recv) :
            listenData{ data }, options{ opts }, receiver{ std::move(recv) }, clientData{ data->MakeChild() } {}

        static void IoCallback(void* context, size_t res, bool success) noexcept {
            auto* self{ static_cast<OperationState*>(context) };
            
            if (!success || static_cast<int>(res) < 0) {
                SocketReceiverAcceptPolicy::Close(self->clientData);
                stdexec::set_error(std::move(self->receiver), Hermes::ConnectionErrorEnum::ConnectionFailed);
                return;
            }

            int receivedFd = SocketReceiverAcceptPolicy::S_RecvFd(static_cast<int>(self->listenData->socket));
            if (receivedFd < 0) {
                SocketReceiverAcceptPolicy::Close(self->clientData);
                stdexec::set_error(std::move(self->receiver), Hermes::ConnectionErrorEnum::ConnectionFailed);
                return;
            }

            self->clientData.socket = static_cast<Hermes::SocketFd>(receivedFd);
            stdexec::set_value(std::move(self->receiver), std::move(self->clientData));
        }

        friend void tag_invoke(stdexec::start_t, OperationState& self) noexcept {
            self.status = {};
            self.status.context = &self;
            self.status.callback = IoCallback;

            auto* loop = Hermes::FastIoLoop::GetLoopForSocket(static_cast<int>(self.listenData->socket));
            if (!loop) {
                stdexec::set_error(std::move(self.receiver), Hermes::ConnectionErrorEnum::NoScheduler);
                return;
            }

            loop->SubmitIo([&](struct io_uring_sqe* sqe) {
                io_uring_prep_poll_add(sqe, static_cast<int>(self.listenData->socket), POLLIN);
                io_uring_sqe_set_data(sqe, &self.status);
            });
        }
    };

    template<class Receiver>
    friend OperationState<Receiver> tag_invoke(stdexec::connect_t, const AcceptSender& self, Receiver r) {
        return { self.listenData, self.options, std::move(r) };
    }
};

struct SocketReceiverAcceptPolicy::ShutdownSender {
    using sender_concept = stdexec::sender_t;
    using completion_signatures = stdexec::completion_signatures<
        stdexec::set_value_t(),
        stdexec::set_error_t(Hermes::ConnectionErrorEnum),
        stdexec::set_stopped_t()
    >;
    
    SocketReceiverData* data;

    template<class Receiver>
    struct OperationState {
        SocketReceiverData* data;
        Receiver receiver;

        friend void tag_invoke(stdexec::start_t, OperationState& self) noexcept {
            if (self.data->socket != macroINVALID_SOCKET) {
                shutdown(self.data->socket, SHUT_RDWR);
            }
            stdexec::set_value(std::move(self.receiver));
        }
    };

    template<class Receiver>
    friend OperationState<Receiver> tag_invoke(stdexec::connect_t, const ShutdownSender& self, Receiver r) {
        return { self.data, std::move(r) };
    }
};

inline SocketReceiverAcceptPolicy::AcceptSender SocketReceiverAcceptPolicy::Accept(SocketReceiverData& listenData, AcceptOptions options) {
    return AcceptSender{ &listenData, options };
}

inline SocketReceiverAcceptPolicy::ShutdownSender SocketReceiverAcceptPolicy::Shutdown(SocketReceiverData& data) {
    return ShutdownSender{ &data };
}

static_assert(Hermes::AsyncAcceptPolicyConcept<SocketReceiverAcceptPolicy, SocketReceiverData>);