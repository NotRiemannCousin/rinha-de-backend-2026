#include <Hermes/Socket/Async/AsyncListenerSocket.hpp>
#include <Hermes/Socket/Async/_base/ExecutionContext/FastIoExecutionContext.hpp>
#include <stdexec/execution.hpp>
#include <exec/repeat_until.hpp>
#include <exec/variant_sender.hpp>
#include <exec/start_detached.hpp>
#include "SocketReceiverAcceptPolicy.hpp"

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <array>
#include <charconv>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <tuple>
#include <memory>
#include <algorithm>
#include <print>
#include <ranges>
#include <span>

#include <simdjson.h>

using namespace std::string_view_literals;

struct alignas(32) Vector16 {
    float v[16];
};

static size_t s_datasetCount{};
static const Vector16* s_features{};
static const bool* s_isLegit{};

static size_t s_clustersCount{};
static const Vector16* s_centroids{};
static const uint32_t* s_clusterStarts{};
static const uint32_t* s_clusterEnds{};

static std::array<float, 10000> s_mccRisk{};

constexpr std::string_view S_FRAUD_RESPONSES[]{
    "HTTP/1.1 200 OK\r\nContent-Length: 35\r\nContent-Type: application/json\r\nConnection: keep-alive\r\n\r\n{\"approved\":true,\"fraud_score\":0.0}"sv,
    "HTTP/1.1 200 OK\r\nContent-Length: 35\r\nContent-Type: application/json\r\nConnection: keep-alive\r\n\r\n{\"approved\":true,\"fraud_score\":0.2}"sv,
    "HTTP/1.1 200 OK\r\nContent-Length: 35\r\nContent-Type: application/json\r\nConnection: keep-alive\r\n\r\n{\"approved\":true,\"fraud_score\":0.4}"sv,
    "HTTP/1.1 200 OK\r\nContent-Length: 36\r\nContent-Type: application/json\r\nConnection: keep-alive\r\n\r\n{\"approved\":false,\"fraud_score\":0.6}"sv,
    "HTTP/1.1 200 OK\r\nContent-Length: 36\r\nContent-Type: application/json\r\nConnection: keep-alive\r\n\r\n{\"approved\":false,\"fraud_score\":0.8}"sv,
    "HTTP/1.1 200 OK\r\nContent-Length: 36\r\nContent-Type: application/json\r\nConnection: keep-alive\r\n\r\n{\"approved\":false,\"fraud_score\":1.0}"sv,
};

constexpr std::string_view S_READY_RESPONSE{ "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: keep-alive\r\n\r\nOK" };
constexpr std::string_view S_NOT_FOUND_RESPONSE{ "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n" };

static void S_LoadDataset() {
    int fdData{ open("/data/dataset.bin", O_RDONLY) };
    if (fdData >= 0) {
        struct stat sb{};
        if (fstat(fdData, &sb) == 0) {
            // Cada vetor tem 64 bytes (16 floats). O array de booleanos vem depois.
            // Tamanho total = N * 64 + N * 1 = N * 65 bytes
            s_datasetCount = static_cast<size_t>(sb.st_size / 65);
            void* mapped{ mmap(nullptr, static_cast<size_t>(sb.st_size), PROT_READ, MAP_SHARED, fdData, 0) };
            if (mapped != MAP_FAILED) {
                s_features = static_cast<const Vector16*>(mapped);
                // Os labels (bool) começam imediatamente após o array de Vector16 (que tem s_datasetCount elementos)
                s_isLegit = reinterpret_cast<const bool*>(s_features + s_datasetCount);
            }
        }
        close(fdData);
    }

    int fdIdx{ open("/data/indexes.bin", O_RDONLY) };
    if (fdIdx >= 0) {
        struct stat sb{};
        if (fstat(fdIdx, &sb) == 0) {
            // Cada cluster guarda: 1 Vector16 (64 bytes) + 1 start (4 bytes) + 1 end (4 bytes) = 72 bytes por cluster.
            s_clustersCount = static_cast<size_t>(sb.st_size / 72);
            void* mapped{ mmap(nullptr, static_cast<size_t>(sb.st_size), PROT_READ, MAP_SHARED, fdIdx, 0) };
            if (mapped != MAP_FAILED) {
                s_centroids = static_cast<const Vector16*>(mapped);
                s_clusterStarts = reinterpret_cast<const uint32_t*>(s_centroids + s_clustersCount);
                s_clusterEnds = s_clusterStarts + s_clustersCount;
            }
        }
        close(fdIdx);
    }
}

[[nodiscard]] static inline float S_Clamp01(const float v) noexcept {
    return std::clamp(v, 0.0f, 1.0f);
}

[[nodiscard]] static std::tuple<float, float, int64_t> S_ParseIso8601(std::string_view s) noexcept {
    if (s.size() < 20) return { 0.0f, 0.0f, 0LL };
    const int y{ (s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 + (s[3] - '0') };
    const int mo{ (s[5] - '0') * 10  + (s[6] - '0') };
    const int dy{ (s[8] - '0') * 10  + (s[9] - '0') };
    const int h{ (s[11] - '0') * 10 + (s[12] - '0') };
    const int mi{ (s[14] - '0') * 10 + (s[15] - '0') };
    const int yZ{ y - (mo < 3 ? 1 : 0) };
    const int mZ{ mo + (mo < 3 ? 12 : 0) };
    const int k{ yZ % 100 };
    const int j{ yZ / 100 };
    const int dow{ ((dy + 13 * (mZ + 1) / 5 + k + k / 4 + j / 4 + 5 * j) % 7 + 5) % 7 };
    const int64_t totalDays{ dy + (153 * (mo + 12 * (mo < 3 ? 1 : 0) - 3) + 2) / 5 + 365LL * y + y / 4 - y / 100 + y / 400 - 32045 };
    return { static_cast<float>(h), static_cast<float>(dow), totalDays * 1440LL + h * 60LL + mi };
}

static void S_LoadMccRisk() {
    s_mccRisk.fill(0.5f);
    simdjson::ondemand::parser parser{};
    auto json{ simdjson::padded_string::load("/data/mcc_risk.json") };
    if (json.error()) return;
    simdjson::ondemand::document doc{ parser.iterate(json.value()) };
    for (auto field : doc.get_object()) {
        std::string_view key{ field.unescaped_key().value() };
        int mcc{};
        std::from_chars(key.data(), key.data() + key.size(), mcc);
        if (mcc >= 0 && mcc < 10000) {
            s_mccRisk[mcc] = static_cast<float>(field.value().get_double().value_unsafe());
        }
    }
}

template <typename SocketT>
struct ClientState {
    SocketT client;
    std::array<char, 16384> buffer{};
    std::string socketView{};
    size_t bodyIdx{};
    size_t contentLength{};
    bool keepAlive{ true };

    Hermes::FastIoLoop* loop{};
    std::array<float, 16> q{};
    struct Neighbor { float dist; uint32_t idx; };
    std::array<Neighbor, 5> top5{};
    uint32_t calcIdx{};
    uint32_t calcEnd{};
    std::string_view response{};
};

template <typename SocketT>
static auto S_HandleClientAsync(std::shared_ptr<ClientState<SocketT>> state) {

    auto s_appendReadBytes = [state](const size_t count) {
        state->socketView.append_range(state->buffer | std::views::take(count));
        return stdexec::just(state->socketView.contains("\r\n\r\n"));
    };

    auto s_extractHeaders = [state]() {
        using VariantSender = exec::variant_sender<
            decltype(state->client.Recv(state->socketView | std::views::drop(0), Hermes::RecvModeEnum::All)),
            decltype(stdexec::just()),
            decltype(stdexec::just_error(Hermes::ConnectionErrorEnum{}))
        >;

        constexpr std::string_view endKey{ "\r\n\r\n" };
        auto& socketView{ state->socketView };
        const auto headerLimitIdx{ socketView.find(endKey) };
        std::string_view headersStr{ socketView.data(), headerLimitIdx };

        state->keepAlive = !socketView.contains("connection: close") && !socketView.contains("Connection: close");
        state->bodyIdx = headerLimitIdx + endKey.size();

        auto clPos{ headersStr.find("content-length: ") };
        if (clPos == std::string_view::npos) clPos = headersStr.find("Content-Length: ");

        if (clPos == std::string_view::npos) {
            state->contentLength = 0;
            return VariantSender{ stdexec::just() };
        }

        headersStr.remove_prefix(clPos + 16);
        std::from_chars(headersStr.data(), headersStr.data() + headersStr.size(), state->contentLength);

        auto lastSize{ socketView.size() };
        socketView.reserve(state->bodyIdx + state->contentLength + simdjson::SIMDJSON_PADDING);
        socketView.resize(state->bodyIdx + state->contentLength);

        if (lastSize - state->bodyIdx >= state->contentLength) {
            return VariantSender{ stdexec::just() };
        }

        auto requestMore{ state->client.Recv(state->socketView | std::views::drop(lastSize), Hermes::RecvModeEnum::All) };
        return VariantSender{ requestMore };
    };

    auto s_parseAndInitCalc = [state](auto...) {
        const std::string_view req{ state->socketView.data(), state->bodyIdx };

        if (req.starts_with("GET ") || req.starts_with("get ")) {
            state->response = S_READY_RESPONSE;
            state->calcIdx = 0;
            state->calcEnd = 0;
            return stdexec::just();
        } else if (!req.starts_with("POST ") && !req.starts_with("post ")) {
            state->response = S_NOT_FOUND_RESPONSE;
            state->calcIdx = 0;
            state->calcEnd = 0;
            return stdexec::just();
        }

        const char* body{ state->socketView.data() + state->bodyIdx };
        try {
            thread_local simdjson::ondemand::parser s_parser{};
            auto doc{ s_parser.iterate(body, state->contentLength, state->socketView.capacity() - state->bodyIdx) };

            auto txObj{ doc["transaction"] };
            const float amount{ static_cast<float>(txObj["amount"].get_double().value()) };
            const float installments{ static_cast<float>(txObj["installments"].get_double().value()) };
            const auto[h, dow, currMins]{ S_ParseIso8601(txObj["requested_at"].get_string().value()) };

            auto custObj{ doc["customer"] };
            const float avgAmount{ static_cast<float>(custObj["avg_amount"].get_double().value()) };
            const float txCount24h{ static_cast<float>(custObj["tx_count_24h"].get_double().value()) };

            std::array<std::string_view, 64> knownMerchants{};
            size_t knownCount{};
            for (auto km : custObj["known_merchants"].get_array()) {
                if (knownCount < std::size(knownMerchants)) {
                    knownMerchants[knownCount++] = km.get_string().value();
                }
            }

            auto mercObj{ doc["merchant"] };
            const std::string_view mId{ mercObj["id"].get_string().value() };
            const std::string_view mMcc{ mercObj["mcc"].get_string().value() };
            const float mAvg{ static_cast<float>(mercObj["avg_amount"].get_double().value()) };

            float unknownMerch{ 1.0f };
            for (size_t i{}; i < knownCount; ++i) {
                if (knownMerchants[i] == mId) { unknownMerch = 0.0f; break; }
            }

            int mccInt{};
            std::from_chars(mMcc.data(), mMcc.data() + mMcc.size(), mccInt);
            const float mccRisk{ (mccInt >= 0 && mccInt < 10000) ? s_mccRisk[mccInt] : 0.5f };

            auto termObj{ doc["terminal"] };
            const bool isOnline{ termObj["is_online"].get_bool().value() };
            const bool cardPresent{ termObj["card_present"].get_bool().value() };
            const float kmHome{ static_cast<float>(termObj["km_from_home"].get_double().value()) };

            float minSinceLast{ -1.0f };
            float kmFromLast{ -1.0f };
            auto lastTxVal{ doc["last_transaction"] };
            if (!lastTxVal.is_null()) {
                const auto[lh, ldow, lastMins]{ S_ParseIso8601(lastTxVal["timestamp"].get_string().value()) };
                const float kmCurr{ static_cast<float>(lastTxVal["km_from_current"].get_double().value()) };
                minSinceLast = S_Clamp01(static_cast<float>(currMins - lastMins) / 1440.0f);
                kmFromLast   = S_Clamp01(kmCurr / 1000.0f);
            }

            state->q = {
                S_Clamp01(amount / 10000.0f),
                S_Clamp01(installments / 12.0f),
                S_Clamp01(avgAmount > 0.0f ? (amount / avgAmount) / 10.0f : 0.0f),
                h / 23.0f,
                dow / 6.0f,
                minSinceLast,
                kmFromLast,
                S_Clamp01(kmHome / 1000.0f),
                S_Clamp01(txCount24h / 20.0f),
                isOnline ? 1.0f : 0.0f,
                cardPresent ? 1.0f : 0.0f,
                unknownMerch,
                mccRisk,
                S_Clamp01(mAvg / 10000.0f),
                0.0f, 0.0f
            };

            auto centroidsSpan{ std::span{ s_centroids, s_clustersCount } };
            auto getDist = [&state](const Vector16& centroid) {
                float dist{ 0.0f };
                for (int j{ 0 }; j < 16; ++j) {
                    float diff{ state->q[j] - centroid.v[j] };
                    dist += diff * diff;
                }
                return dist;
            };

            // Proteção contra s_clustersCount == 0 (ex: dataset.bin não foi gerado corretamente)
            if (s_clustersCount > 0) {
                auto bestCentroidIt{ std::ranges::min_element(centroidsSpan, std::ranges::less{}, getDist) };
                uint32_t bestCluster{ static_cast<uint32_t>(std::distance(centroidsSpan.begin(), bestCentroidIt)) };
                state->calcIdx = s_clusterStarts[bestCluster];
                state->calcEnd = s_clusterEnds[bestCluster];
            } else {
                state->calcIdx = 0;
                state->calcEnd = 0;
            }

            state->top5.fill({ std::numeric_limits<float>::infinity(), UINT32_MAX });

        } catch (...) {
            state->response = S_FRAUD_RESPONSES[5];
            state->calcIdx = 0;
            state->calcEnd = 0;
        }

        return stdexec::just();
    };

    auto s_calcChunk = [state](auto&&...) {
        return stdexec::schedule(state->loop->GetScheduler())
             | stdexec::let_value([state]() {
                   if (state->calcIdx >= state->calcEnd) {
                       if (state->response.empty()) {
                           int fraudCount{ 0 };
                           for (const auto& n : state->top5) {
                               if (n.idx < s_datasetCount && !s_isLegit[n.idx]) fraudCount++;
                           }
                           state->response = S_FRAUD_RESPONSES[fraudCount];
                       }
                       return stdexec::just(true);
                   }

                   uint32_t end{ std::min(state->calcIdx + 16384, state->calcEnd) };
                   auto chunk{ std::span{ s_features, s_datasetCount }.subspan(state->calcIdx, end - state->calcIdx) };

                   for (auto&& [i, feature] : std::views::enumerate(chunk)) {
                       float dist{ 0.0f };
                       for (int j{ 0 }; j < 16; ++j) {
                           float diff{ state->q[j] - feature.v[j] };
                           dist += diff * diff;
                       }

                       if (dist < state->top5.back().dist) {
                           state->top5.back() = { dist, static_cast<uint32_t>(state->calcIdx + i) };
                           std::ranges::sort(state->top5, [](const auto& a, const auto& b) { return a.dist < b.dist; });
                       }
                   }

                   state->calcIdx = end;
                   return stdexec::just(false);
               });
    };

    auto s_processAndSend = [state](auto...) {
        return state->client.Send(state->response);
    };

    auto s_onComplete = [state](const auto&...) {
        state->socketView.clear();
        state->bodyIdx = 0;
        state->contentLength = 0;
        return stdexec::just(!state->keepAlive);
    };

    return state->client.Recv(state->buffer, Hermes::RecvModeEnum::Any)
         | stdexec::let_value(s_appendReadBytes)
         | exec::repeat_until()
         | stdexec::let_value(s_extractHeaders)
         | stdexec::let_value(s_parseAndInitCalc)
         | stdexec::let_value(s_calcChunk)
         | exec::repeat_until()
         | stdexec::let_value(s_processAndSend)
         | stdexec::let_value(s_onComplete)
         | exec::repeat_until();
}

using ReceiverSocket = Hermes::AsyncListenerSocket<
    SocketReceiverData,
    SocketReceiverAcceptPolicy,
    Hermes::DefaultAsyncTransferPolicy<SocketReceiverData>>;

static auto S_ServeLoop(ReceiverSocket& listener, Hermes::FastIoLoop& loop) {
    SocketReceiverAcceptPolicy::AcceptOptions opts{ .scheduler = &loop };

    return listener.AsyncAcceptOne(opts)
         | stdexec::let_value([&loop](auto&& clientSocket) {
               using ClientT = std::decay_t<decltype(clientSocket)>;
               auto state{ std::make_shared<ClientState<ClientT>>(ClientState<ClientT>{ std::move(clientSocket) }) };
               state->loop = &loop;

               exec::start_detached(
                   S_HandleClientAsync(state)
                       | stdexec::let_error([](auto&&) { return stdexec::just(); })
               );
               return stdexec::just(false);
           })
         | exec::repeat_until();
}

int main(int argc, char* argv[]) {
    std::setvbuf(stdout, NULL, _IONBF, 0);
    std::setvbuf(stderr, NULL, _IONBF, 0);

    S_LoadDataset();
    S_LoadMccRisk();

    const std::string sockPath{ (argc > 1) ? argv[1] : "/sockets/api1.sock" };
    Hermes::FastIoLoop loop{ 1 };

    SocketReceiverData data{ sockPath };
    SocketReceiverAcceptPolicy::ListenOptions listenOpts{};
    listenOpts.scheduler = &loop;

    auto serve{ ReceiverSocket::Listen(std::move(data), listenOpts)
               | stdexec::let_value([&loop](auto& listener) {
                     return S_ServeLoop(listener, loop);
                 })
               | stdexec::let_error([](auto) {
                     return stdexec::just();
                 }) };

    stdexec::sync_wait(std::move(serve));
    return 0;
}