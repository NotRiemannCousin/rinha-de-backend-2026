#include <Hermes/Socket/Async/AsyncListenerSocket.hpp>
#include <Hermes/Socket/Async/_base/ExecutionContext/FastIoExecutionContext.hpp>
#include <stdexec/execution.hpp>
#include <exec/repeat_until.hpp>
#include <exec/variant_sender.hpp>
#include <exec/start_detached.hpp>
#include "SocketReceiverAcceptPolicy.hpp"

#include <array>
#include <charconv>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <tuple>
#include <memory>
#include <iostream>
#include <print>

#include <simdjson.h>

using namespace std::string_view_literals;

// ════════════════════════════════════════════════════════════════════════════
// Dataset
// ════════════════════════════════════════════════════════════════════════════

#pragma pack(push, 1)
struct DatasetElement {
    std::array<float, 14> features;
    float squaredNorm;
    bool  isLegit;
};
#pragma pack(pop)

constexpr size_t DatasetSize = 3'000'000;

std::array<DatasetElement, DatasetSize> g_dataset{};
std::array<float, 10000>               g_mccRisk{};

constexpr std::string_view S_FRAUD_RESPONSES[6] = {
    "HTTP/1.1 200 OK\r\nContent-Length: 35\r\nContent-Type: application/json\r\nConnection: keep-alive\r\n\r\n{\"approved\":true,\"fraud_score\":0.0}"sv,
    "HTTP/1.1 200 OK\r\nContent-Length: 35\r\nContent-Type: application/json\r\nConnection: keep-alive\r\n\r\n{\"approved\":true,\"fraud_score\":0.2}"sv,
    "HTTP/1.1 200 OK\r\nContent-Length: 35\r\nContent-Type: application/json\r\nConnection: keep-alive\r\n\r\n{\"approved\":true,\"fraud_score\":0.4}"sv,
    "HTTP/1.1 200 OK\r\nContent-Length: 36\r\nContent-Type: application/json\r\nConnection: keep-alive\r\n\r\n{\"approved\":false,\"fraud_score\":0.6}"sv,
    "HTTP/1.1 200 OK\r\nContent-Length: 36\r\nContent-Type: application/json\r\nConnection: keep-alive\r\n\r\n{\"approved\":false,\"fraud_score\":0.8}"sv,
    "HTTP/1.1 200 OK\r\nContent-Length: 36\r\nContent-Type: application/json\r\nConnection: keep-alive\r\n\r\n{\"approved\":false,\"fraud_score\":1.0}"sv,
};

constexpr std::string_view S_READY_RESPONSE =
    "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: keep-alive\r\n\r\n"sv;

constexpr std::string_view S_NOT_FOUND_RESPONSE =
    "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"sv;

// ════════════════════════════════════════════════════════════════════════════
// Utilitários e Carga
// ════════════════════════════════════════════════════════════════════════════

[[nodiscard]] static inline float S_Clamp01(float v) noexcept {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

[[nodiscard]] static std::tuple<float, float, int64_t>
S_ParseIso8601(std::string_view s) noexcept {
    if (s.size() < 20) return {0.0f, 0.0f, 0LL};
    const int y  = (s[0]-'0')*1000 + (s[1]-'0')*100 + (s[2]-'0')*10 + (s[3]-'0');
    const int mo = (s[5]-'0')*10  + (s[6]-'0');
    const int dy = (s[8]-'0')*10  + (s[9]-'0');
    const int h  = (s[11]-'0')*10 + (s[12]-'0');
    const int mi = (s[14]-'0')*10 + (s[15]-'0');
    const int yZ = y - (mo < 3 ? 1 : 0);
    const int mZ = mo + (mo < 3 ? 12 : 0);
    const int k  = yZ % 100, j = yZ / 100;
    const int dow = ((dy + (13*(mZ+1))/5 + k + k/4 + j/4 + 5*j) % 7 + 5) % 7;
    const int64_t totalDays = dy + (153*(mo + 12*(mo < 3 ? 1 : 0) - 3) + 2)/5 + 365LL*y + y/4 - y/100 + y/400 - 32045;
    return { static_cast<float>(h), static_cast<float>(dow), totalDays * 1440LL + h * 60LL + mi };
}

static void S_LoadDataset() {
    std::ifstream file("/data/dataset.bin", std::ios::binary);
    if (!file) return;

    #pragma pack(push, 1)
    struct FileRec { std::array<float, 14> features; bool isLegit; };
    #pragma pack(pop)

    FileRec rec;
    for (size_t i = 0; i < DatasetSize; ++i) {
        if (!file.read(reinterpret_cast<char*>(&rec), sizeof(rec))) break;
        auto& el   = g_dataset[i];
        el.features = rec.features;
        el.isLegit  = rec.isLegit;
        float sq = 0.0f;
        for (float f : el.features) sq += f * f;
        el.squaredNorm = sq;
    }
}

static void S_LoadMccRisk() {
    g_mccRisk.fill(0.5f);
    simdjson::ondemand::parser parser;
    auto json = simdjson::padded_string::load("/data/mcc_risk.json");
    if (json.error()) return;
    simdjson::ondemand::document doc = parser.iterate(json.value());
    for (auto field : doc.get_object()) {
        std::string_view key = field.unescaped_key().value();
        int mcc = 0;
        std::from_chars(key.data(), key.data() + key.size(), mcc);
        if (mcc >= 0 && mcc < 10000)
            g_mccRisk[mcc] = static_cast<float>(field.value().get_double().value_unsafe());
    }
}

[[nodiscard]] static int S_FraudNeighborCount(const std::array<float, 14>& q) noexcept {
    struct Neighbor { float dist; bool isLegit; };
    std::array<Neighbor, 5> top5;
    top5.fill({std::numeric_limits<float>::infinity(), true});
    float worstDist = std::numeric_limits<float>::infinity();
    int worstIdx = 0;

    for (size_t i = 0; i < DatasetSize; ++i) {
        const auto& ref = g_dataset[i];
        float dot = 0.0f;
        for (int j = 0; j < 14; ++j) dot += q[j] * ref.features[j];
        const float dist = ref.squaredNorm - 2.0f * dot;
        if (dist < worstDist) {
            top5[worstIdx] = {dist, ref.isLegit};
            worstDist = top5[0].dist;
            worstIdx  = 0;
            for (int j = 1; j < 5; ++j) {
                if (top5[j].dist > worstDist) {
                    worstDist = top5[j].dist;
                    worstIdx  = j;
                }
            }
        }
    }
    int n = 0;
    for (const auto& nb : top5) n += !nb.isLegit;
    return n;
}

// ════════════════════════════════════════════════════════════════════════════
// Processamento de requisição / Lógica ML (Isolada)
// ════════════════════════════════════════════════════════════════════════════

[[nodiscard]] static std::string_view S_ComputeFraudScore(
        const char* body,
        size_t bodyLen,
        size_t bufCap) noexcept {

    try {
        thread_local simdjson::ondemand::parser s_parser;
        auto doc = s_parser.iterate(body, bodyLen, bufCap);

        // USANDO .value() AO INVÉS DE .value_unsafe() PARA EVITAR CRASH FATAL!
        auto txObj = doc["transaction"];
        const float amount       = static_cast<float>(txObj["amount"].get_double().value());
        const float installments = static_cast<float>(txObj["installments"].get_double().value());
        const std::string_view reqAt = txObj["requested_at"].get_string().value();
        const auto [h, dow, currMins] = S_ParseIso8601(reqAt);

        auto custObj = doc["customer"];
        const float avgAmount  = static_cast<float>(custObj["avg_amount"].get_double().value());
        const float txCount24h = static_cast<float>(custObj["tx_count_24h"].get_double().value());

        std::array<std::string_view, 64> knownMerchants{};
        size_t knownCount = 0;
        for (auto km : custObj["known_merchants"].get_array()) {
            if (knownCount < std::size(knownMerchants))
                knownMerchants[knownCount++] = km.get_string().value();
        }

        auto mercObj = doc["merchant"];
        const std::string_view mId  = mercObj["id"].get_string().value();
        const std::string_view mMcc = mercObj["mcc"].get_string().value();
        const float mAvg = static_cast<float>(mercObj["avg_amount"].get_double().value());

        float unknownMerch = 1.0f;
        for (size_t i = 0; i < knownCount; ++i) {
            if (knownMerchants[i] == mId) { unknownMerch = 0.0f; break; }
        }

        int mccInt = 0;
        std::from_chars(mMcc.data(), mMcc.data() + mMcc.size(), mccInt);
        const float mccRisk = (mccInt >= 0 && mccInt < 10000) ? g_mccRisk[mccInt] : 0.5f;

        auto termObj = doc["terminal"];
        const bool  isOnline    = termObj["is_online"].get_bool().value();
        const bool  cardPresent = termObj["card_present"].get_bool().value();
        const float kmHome      = static_cast<float>(termObj["km_from_home"].get_double().value());

        float minSinceLast = -1.0f, kmFromLast = -1.0f;
        auto lastTxVal = doc["last_transaction"];
        if (!lastTxVal.is_null()) {
            const std::string_view lastTs = lastTxVal["timestamp"].get_string().value();
            const float kmCurr = static_cast<float>(lastTxVal["km_from_current"].get_double().value());
            const int64_t lastMins = std::get<2>(S_ParseIso8601(lastTs));
            minSinceLast = S_Clamp01(static_cast<float>(currMins - lastMins) / 1440.0f);
            kmFromLast   = S_Clamp01(kmCurr / 1000.0f);
        }

        const std::array<float, 14> q = {
            S_Clamp01(amount / 10000.0f),
            S_Clamp01(installments / 12.0f),
            S_Clamp01(avgAmount > 0.0f ? (amount / avgAmount) / 10.0f : 0.0f),
            h / 23.0f,
            dow / 6.0f,
            minSinceLast,
            kmFromLast,
            S_Clamp01(kmHome / 1000.0f),
            S_Clamp01(txCount24h / 20.0f),
            isOnline    ? 1.0f : 0.0f,
            cardPresent ? 1.0f : 0.0f,
            unknownMerch,
            mccRisk,
            S_Clamp01(mAvg / 10000.0f),
        };

        const int fraudCount = S_FraudNeighborCount(q);
        return S_FRAUD_RESPONSES[fraudCount];

    } catch (const std::exception& e) {
        std::println("API CRASH PREVENIDO: Erro ao fazer parse do JSON: {}", e.what());
        return S_FRAUD_RESPONSES[5]; // Resposta genérica em caso de erro no JSON
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Hermes Async Pipeline
// ════════════════════════════════════════════════════════════════════════════

template <typename SocketT>
struct ClientState {
    SocketT client;
    std::array<char, 16384> buffer{};
    std::string socketView{};
    size_t bodyIdx{ 0 };
    size_t contentLength{ 0 };
    bool keepAlive{ true };
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

        constexpr std::string_view clKey{ "content-length: " };
        constexpr std::string_view clKeyCap{ "Content-Length: " };
        constexpr std::string_view closeKey{ "connection: close" };
        constexpr std::string_view closeKeyCap{ "Connection: close" };
        constexpr std::string_view endKey{ "\r\n\r\n" };

        auto& socketView = state->socketView;
        const auto headerLimitIdx = socketView.find(endKey);
        std::string_view headersStr{ socketView.data(), headerLimitIdx };

        state->keepAlive = !socketView.contains(closeKey) && !socketView.contains(closeKeyCap);
        state->bodyIdx = headerLimitIdx + endKey.size();

        auto clPos = headersStr.find(clKey);
        if (clPos == std::string_view::npos) clPos = headersStr.find(clKeyCap);

        if (clPos == std::string_view::npos) {
            state->contentLength = 0;
            return VariantSender{ stdexec::just() };
        }

        headersStr.remove_prefix(clPos + 16);
        std::from_chars(headersStr.data(), headersStr.data() + headersStr.size(), state->contentLength);

        auto lastSize = socketView.size();

        // Garante a margem para o simdjson sem foder o tamanho visível do recv
        socketView.reserve(state->bodyIdx + state->contentLength + simdjson::SIMDJSON_PADDING);
        socketView.resize(state->bodyIdx + state->contentLength);

        if (lastSize - state->bodyIdx >= state->contentLength) {
            return VariantSender{ stdexec::just() };
        }

        auto requestMore = state->client.Recv(state->socketView | std::views::drop(lastSize), Hermes::RecvModeEnum::All);
        return VariantSender{ requestMore };
    };

    auto s_processAndSend = [state](auto...) {
        std::string_view req(state->socketView.data(), state->bodyIdx);
        std::string_view response;

        if (req.starts_with("GET ")) {
            response = S_READY_RESPONSE;
        } else if (!req.starts_with("POST ")) {
            response = S_NOT_FOUND_RESPONSE;
        } else {
            const char* body = state->socketView.data() + state->bodyIdx;
            response = S_ComputeFraudScore(body, state->contentLength, state->socketView.capacity() - state->bodyIdx);
        }

        return state->client.Send(response);
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
         | stdexec::let_value(s_processAndSend)
         | stdexec::let_value(s_onComplete)
         | exec::repeat_until();
}

using ReceiverSocket = Hermes::AsyncListenerSocket<
    SocketReceiverData,
    SocketReceiverAcceptPolicy,
    Hermes::DefaultAsyncTransferPolicy<SocketReceiverData>>;

static auto S_ServeLoop(ReceiverSocket& listener, Hermes::FastIoLoop& loop) {
    SocketReceiverAcceptPolicy::AcceptOptions opts{};
    opts.scheduler = &loop;

    return listener.AsyncAcceptOne(opts)
         | stdexec::let_value([](auto&& clientSocket) {
               std::println("API: +++ Novo socket TCP (FD) repassado com sucesso via SCM_RIGHTS! +++");

               using ClientT = std::decay_t<decltype(clientSocket)>;
               auto state = std::make_shared<ClientState<ClientT>>(ClientState<ClientT>{ std::move(clientSocket) });

               exec::start_detached(
                   S_HandleClientAsync(state)
                       | stdexec::let_error([](auto&& err) { return stdexec::just(); })
               );

               return stdexec::just(false);
           })
         | exec::repeat_until();
}

int main(int argc, char* argv[]) {
    // IMPORTANTE: Desativar buffer para vermos os logs no Docker na hora!
    std::setvbuf(stdout, NULL, _IONBF, 0);
    std::setvbuf(stderr, NULL, _IONBF, 0);

    std::println("API: Inicializando...");

    S_LoadDataset();
    S_LoadMccRisk();

    const std::string sockPath = (argc > 1) ? argv[1] : "/sockets/api1.sock";
    std::println("API: Criando e escutando Socket UNIX em: {}", sockPath);

    Hermes::FastIoLoop loop{1};

    SocketReceiverData data{sockPath};
    SocketReceiverAcceptPolicy::ListenOptions listenOpts{};
    listenOpts.scheduler = &loop;

    auto serve = ReceiverSocket::Listen(std::move(data), listenOpts)
               | stdexec::let_value([&loop](auto& listener) {
                     std::println("API: Escuta iniciada. Aguardando repasse de FD do LoadBalancer...");
                     return S_ServeLoop(listener, loop);
                 })
               | stdexec::let_error([](auto err) {
                     std::println("API: Erro fatal no Listener.");
                     return stdexec::just();
                 });

    stdexec::sync_wait(std::move(serve));
    return 0;
}