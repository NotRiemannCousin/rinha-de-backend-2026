#include <LoadBalancer/SocketForwarder/SocketForwarderAcceptPolicy.hpp>
#include <unistd.h>

void SocketForwarderAcceptPolicy::S_SendFd(int unixSocket, int fdToSend) noexcept {
    msghdr msg{};
    iovec iov[1];
    char dummyData = 'A'; 
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

    cmsghdr* cmptr = CMSG_FIRSTHDR(&msg);
    cmptr->cmsg_len = CMSG_LEN(sizeof(int));
    cmptr->cmsg_level = SOL_SOCKET;
    cmptr->cmsg_type = SCM_RIGHTS;
    *reinterpret_cast<int*>(CMSG_DATA(cmptr)) = fdToSend;

    sendmsg(unixSocket, &msg, 0);
}

Hermes::ConnectionResultOper SocketForwarderAcceptPolicy::Listen(SocketForwarderData& data, int backlog, ListenOptions options) {
    auto addrRes{ data.endpoint.ToSockAddr() };
    if (!addrRes) return std::unexpected{ Hermes::ConnectionErrorEnum::InvalidEndpoint };
    auto [addr, addrLen, addrFamily]{ *addrRes };

    data.socket = socket(static_cast<int>(addrFamily), static_cast<int>(Type), 0);
    if (data.socket == macroINVALID_SOCKET) return std::unexpected{ Hermes::ConnectionErrorEnum::ConnectionFailed };

    if (options.reuseAddress) {
        int opt{ 1 };
        setsockopt(data.socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    }

    if (bind(data.socket, reinterpret_cast<sockaddr*>(&addr), static_cast<int>(addrLen)) == macroSOCKET_ERROR) {
        close(data.socket);
        data.socket = macroINVALID_SOCKET;
        return std::unexpected{ Hermes::ConnectionErrorEnum::AddressInUse };
    }

    if (listen(data.socket, backlog) == macroSOCKET_ERROR) {
        close(data.socket);
        data.socket = macroINVALID_SOCKET;
        return std::unexpected{ Hermes::ConnectionErrorEnum::ListenFailed };
    }

    if (options.scheduler && !options.scheduler->RegisterHandle(reinterpret_cast<Hermes::SocketHandle>(data.socket))) {
        return std::unexpected{ Hermes::ConnectionErrorEnum::NoScheduler };
    }

    for (const auto& path : data.unixPaths) {
        int unixFd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (unixFd == macroINVALID_SOCKET) continue;

        sockaddr_un unAddr{};
        unAddr.sun_family = AF_UNIX;
        strncpy(unAddr.sun_path, path.c_str(), sizeof(unAddr.sun_path) - 1);

        if (connect(unixFd, reinterpret_cast<sockaddr*>(&unAddr), sizeof(unAddr)) == 0) {
            data.unixSockets.push_back(unixFd);
        } else {
            close(unixFd);
        }
    }

    return {};
}

void SocketForwarderAcceptPolicy::Close(SocketForwarderData& data) noexcept {
    if (data.socket != macroINVALID_SOCKET) {
        close(data.socket);
        data.socket = macroINVALID_SOCKET;
    }
    for (auto fd : data.unixSockets) {
        if (fd != macroINVALID_SOCKET) close(fd);
    }
    data.unixSockets.clear();
}

void SocketForwarderAcceptPolicy::Abort(SocketForwarderData& data) noexcept {
    Close(data);
}