#include "SocketReceiverAcceptPolicy.hpp"
#include <unistd.h>

int SocketReceiverAcceptPolicy::S_RecvFd(int unixSocket) noexcept {
    msghdr msg{};
    iovec iov[1];
    char dummyData;
    
    iov[0].iov_base = &dummyData;
    iov[0].iov_len = 1;
    msg.msg_iov = iov;
    msg.msg_iovlen = 1;

    union {
        cmsghdr cm;
        char control[CMSG_SPACE(sizeof(int))];
    } controlUn;

    msg.msg_control = controlUn.control;
    msg.msg_controllen = sizeof(controlUn.control);

    if (recvmsg(unixSocket, &msg, 0) <= 0) {
        return -1;
    }

    cmsghdr* cmptr = CMSG_FIRSTHDR(&msg);
    if (cmptr != nullptr && cmptr->cmsg_len == CMSG_LEN(sizeof(int)) &&
        cmptr->cmsg_level == SOL_SOCKET && cmptr->cmsg_type == SCM_RIGHTS) {
        return *reinterpret_cast<int*>(CMSG_DATA(cmptr));
    }
    
    return -1;
}

Hermes::ConnectionResultOper SocketReceiverAcceptPolicy::Listen(SocketReceiverData& data, int backlog, ListenOptions options) {
    data.unixSocket = socket(AF_UNIX, SOCK_STREAM, 0);
    if (data.unixSocket == macroINVALID_SOCKET) return std::unexpected{ Hermes::ConnectionErrorEnum::ConnectionFailed };

    unlink(data.unixPath.c_str());

    sockaddr_un unAddr{};
    unAddr.sun_family = AF_UNIX;
    strncpy(unAddr.sun_path, data.unixPath.c_str(), sizeof(unAddr.sun_path) - 1);

    if (bind(data.unixSocket, reinterpret_cast<sockaddr*>(&unAddr), sizeof(unAddr)) == macroSOCKET_ERROR) {
        close(data.unixSocket);
        data.unixSocket = macroINVALID_SOCKET;
        return std::unexpected{ Hermes::ConnectionErrorEnum::AddressInUse };
    }

    if (listen(data.unixSocket, backlog) == macroSOCKET_ERROR) {
        close(data.unixSocket);
        data.unixSocket = macroINVALID_SOCKET;
        return std::unexpected{ Hermes::ConnectionErrorEnum::ListenFailed };
    }

    data.socket = accept(data.unixSocket, nullptr, nullptr);
    if (data.socket == macroINVALID_SOCKET) {
        close(data.unixSocket);
        data.unixSocket = macroINVALID_SOCKET;
        return std::unexpected{ Hermes::ConnectionErrorEnum::ConnectionFailed };
    }

    if (options.scheduler && !options.scheduler->RegisterHandle(reinterpret_cast<Hermes::SocketHandle>(data.socket))) {
        return std::unexpected{ Hermes::ConnectionErrorEnum::NoScheduler };
    }

    return {};
}

void SocketReceiverAcceptPolicy::Close(SocketReceiverData& data) noexcept {
    if (data.socket != macroINVALID_SOCKET) {
        close(data.socket);
        data.socket = macroINVALID_SOCKET;
    }
    if (data.unixSocket != macroINVALID_SOCKET) {
        close(data.unixSocket);
        data.unixSocket = macroINVALID_SOCKET;
        unlink(data.unixPath.c_str());
    }
}

void SocketReceiverAcceptPolicy::Abort(SocketReceiverData& data) noexcept {
    Close(data);
}