#include <variant>
#include <Hermes/Socket/Async/AsyncListenerSocket.hpp>
#include <Hermes/Socket/Async/_base/ExecutionContext/FastIoExecutionContext.hpp>
#include <stdexec/execution.hpp>
#include <exec/task.hpp>
#include <exec/repeat_until.hpp>
#include <exec/start_detached.hpp>
#include <Api/SocketReceiver/SocketReceiverAcceptPolicy.hpp>
#include <Api/Definitions.hpp>
#include <Api/Dataset.hpp>

#include <sys/mman.h>
#include <array>
#include <charconv>
#include <string>
#include <string_view>
#include <memory>
#include <algorithm>
#include <print>
#include <ranges>
#include <simd>

#include <Api/Response.hpp>
#include <stdexec/execution.hpp>


// simdjson::SIMDJSON_PADDING, im not including <simdjson.h> just to it
constexpr int simdjsonPadding{ 64 };

using namespace std::string_view_literals;
namespace rg = std::ranges;

static exec::task<int> S_ParseHttp(ClientState& state) {
    static constexpr std::string_view endHeaders{ "\r\n\r\n" };
    state.socketView.clear();
    state.socketView.reserve(4096);

    while (!state.socketView.contains(endHeaders)) {
        const size_t received{ co_await state.client.Recv(state.buffer, Hermes::RecvModeEnum::Any) };
        state.socketView.append_range(state.buffer | std::views::take(received));
    }

    auto& socketView{ state.socketView };
    const auto headerLimitIdx{ socketView.find(endHeaders) };
    std::string_view headersStr{ socketView.data(), headerLimitIdx };

    if (socketView.starts_with("GET "))
        co_return 0;

    state.keepAlive = !socketView.contains("connection: close") && !socketView.contains("Connection: close");
    state.bodyIdx   = headerLimitIdx + endHeaders.size();

    auto clPos{ headersStr.find("content-length: ") };
    if (clPos == std::string_view::npos) clPos = headersStr.find("Content-Length: ");

    if (clPos == std::string_view::npos) {
        state.contentLength = 0;
        throw std::invalid_argument{ "Only content-length is implemented" };
    }

    headersStr.remove_prefix(clPos + 16);
    std::from_chars(headersStr.data(), headersStr.data() + headersStr.size(), state.contentLength);

    auto lastSize{ socketView.size() };
    socketView.reserve(state.bodyIdx + state.contentLength + simdjsonPadding);
    socketView.resize(state.bodyIdx + state.contentLength);

    if (lastSize - state.bodyIdx >= state.contentLength)
        co_return 0;

    co_await state.client.Recv(state.socketView | std::views::drop(lastSize), Hermes::RecvModeEnum::All);
    co_return 0;
}

template<class T>
static void S_HandleException(const T& e){
    if constexpr (std::same_as<T, std::exception_ptr>){
        if (!e) return;
        try { std::rethrow_exception(e); }
        catch (const std::string&                 err) { S_HandleException<>(err);            }
        catch (const std::exception&              err) { S_HandleException<>(err);            }
        catch (const Hermes::ConnectionErrorEnum& err) { S_HandleException<>(err);            }
        catch (...                                   ) { S_HandleException(std::monostate{}); }
    }
    else if constexpr (std::derived_from<T, std::exception>) std::println("[SERVER ERROR]: {}", e.what());
    else if constexpr (std::formattable<T, char>)            std::println("[SERVER ERROR]: {}", e);
    else                                                     std::println("[SERVER ERROR]: unknown error (not an exception)");
}

static exec::task<void> S_HandleClientAsync(ClientState state) {
    try {
        do {
            co_await S_ParseHttp(state);
            ProcessRequest(state);
            co_await state.client.Send(state.response);

            state.socketView.clear();
            state.bodyIdx = 0;
            state.contentLength = 0;
        } while (state.keepAlive);
    } catch (...) { S_HandleException(std::current_exception()); }
}

static exec::task<void> S_ServeLoop(ReceiverSocket& listener, Hermes::FastIoLoop& loop) try {
    SocketReceiverAcceptPolicy::AcceptOptions opts{ .scheduler = &loop };

    while (true) {
        while (true) {
            exec::start_detached(stdexec::on(
                loop.GetScheduler(),
                S_HandleClientAsync(ClientState{ co_await listener.AsyncAcceptOne(opts) })
            ));
        }
    }
}
catch (...) { std::println("[FATAL ERROR]"); }

int main(int argc, char* argv[]) {
    std::setvbuf(stdout, NULL, _IONBF, 0);
    std::setvbuf(stderr, NULL, _IONBF, 0);

    LoadDataset();

    const std::string sockPath{ argc > 1 ? argv[1] : "/sockets/api1.sock" };
    Hermes::FastIoLoop loop{ 1 };

    SocketReceiverData data{ sockPath };
    const SocketReceiverAcceptPolicy::ListenOptions listenOpts{ .scheduler = &loop };

    auto serve{ ReceiverSocket::Listen(std::move(data), listenOpts)
            | stdexec::let_value([&loop](auto& listener) noexcept {
                return S_ServeLoop(listener, loop);
            })
    };

    stdexec::sync_wait(std::move(serve));
    return 0;
}