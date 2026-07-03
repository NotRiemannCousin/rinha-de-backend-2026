#include <spanstream>
#include <Api/Response.hpp>
#include <Api/Dataset.hpp>
#include <algorithm>
#include <concepts>
#include <chrono>
#include <print>
#include <array>
#include <span>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-literal-operator"
#include <simdjson.h>
#pragma GCC diagnostic pop

namespace chr = std::chrono;

static chr::sys_seconds FromStr(const std::string_view input) {
    chr::sys_seconds tp;
    std::ispanstream sp{ input };
    chr::from_stream(sp, "%Y-%m-%dT%H:%M:%SZ", tp);

    return tp;
}

namespace Response {
    constexpr std::string_view fraudResponses[]{
        "HTTP/1.1 200 OK\r\nContent-Length: 35\r\nContent-Type: application/json\r\nConnection: keep-alive\r\n\r\n{\"approved\":true,\"fraud_score\":0.0}",
        "HTTP/1.1 200 OK\r\nContent-Length: 35\r\nContent-Type: application/json\r\nConnection: keep-alive\r\n\r\n{\"approved\":true,\"fraud_score\":0.2}",
        "HTTP/1.1 200 OK\r\nContent-Length: 35\r\nContent-Type: application/json\r\nConnection: keep-alive\r\n\r\n{\"approved\":true,\"fraud_score\":0.4}",
        "HTTP/1.1 200 OK\r\nContent-Length: 36\r\nContent-Type: application/json\r\nConnection: keep-alive\r\n\r\n{\"approved\":false,\"fraud_score\":0.6}",
        "HTTP/1.1 200 OK\r\nContent-Length: 36\r\nContent-Type: application/json\r\nConnection: keep-alive\r\n\r\n{\"approved\":false,\"fraud_score\":0.8}",
        "HTTP/1.1 200 OK\r\nContent-Length: 36\r\nContent-Type: application/json\r\nConnection: keep-alive\r\n\r\n{\"approved\":false,\"fraud_score\":1.0}",
    };

    constexpr std::string_view readyResponse    { "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: keep-alive\r\n\r\nOK" };
    constexpr std::string_view notFoundResponse { "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n" };
    constexpr std::string_view serverErrResponse{ "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n" };
}

namespace Constants {
    constexpr float maxAmount           { 10000 };
    constexpr float maxInstallments     { 12    };
    constexpr float amountVsAvgRatio    { 10    };
    constexpr float maxMinutes          { 1440  };
    constexpr float maxKm               { 1000  };
    constexpr float maxTxCount24H       { 20    };
    constexpr float maxMerchantAvgAmount{ 10000 };
}

static constexpr float S_GetMccRisk(const size_t mcc) noexcept {
    switch (mcc) {
        case 4511: return 0.35f; case 5311: return 0.25f;
        case 5411: return 0.15f; case 5812: return 0.30f;
        case 5912: return 0.20f; case 5944: return 0.45f;
        case 5999: return 0.50f; case 7801: return 0.80f;
        case 7802: return 0.75f; case 7995: return 0.85f;
        default:   return 0.50f;
    }
}


namespace { struct Arr{}; struct Int{}; struct Float{}; }

template<class T>
static auto Get(simdjson::dom::object& obj, std::string_view key) {
    const auto val{ obj[key] };

    if      constexpr (std::same_as<T, float>)                      return val.get_double(); // NOLINT(*-branch-clone)
    else if constexpr (std::same_as<T, double>)                     return val.get_double();
    else if constexpr (std::same_as<T, Arr>)                        return val.get_array();
    else if constexpr (std::same_as<T, std::string_view>)           return val.get_string(); // NOLINT(*-branch-clone)
    else if constexpr (std::same_as<T, Int>)                        return val.get_string();
    else if constexpr (std::same_as<T, Float>)                      return val.get_string();
    else if constexpr (std::same_as<T, simdjson::dom::object>)      return val.get_object();
    else if constexpr (std::same_as<T, bool>)                       return val.get_bool();
    else if constexpr (std::same_as<T, int>)                        return val.get_int64();
    else static_assert(false, "Unaccepted type");
}

template<class T>
static auto GetVal(simdjson::dom::object& obj, std::string_view key) {
    if constexpr (std::same_as<T, Float>) {
        const auto str{ Get<std::string_view>(obj, key).value() };
        float res{ .0f };
        std::from_chars(str.data(), str.data() + str.size(), res);

        return res;
    }
    else if constexpr (std::same_as<T, Int>) {
        const auto str{ Get<std::string_view>(obj, key).value() };
        int64_t res{ 0 };
        std::from_chars(str.data(), str.data() + str.size(), res);

        return res;
    }
    else if constexpr (std::same_as<T, float> || std::same_as<T, double>) {
        const auto val{ obj[key].value() };

        if (val.is_int64())  return static_cast<T>(val.get_int64().value());
        if (val.is_uint64()) return static_cast<T>(val.get_uint64().value());

        return static_cast<T>(val.get_double().value());
    }
    else
        return Get<T>(obj, key).value();
}



template<class T>
static float Clamp01(const T f, const float div = 1.0f) {
    return std::clamp(static_cast<float>(f) / div, 0.0f, 1.0f);
}

static std::string_view S_CreateJson(const char* body, size_t len, size_t capacity) try {
    using Obj = simdjson::dom::object;
    using Str = std::string_view;

    thread_local simdjson::dom::parser s_parser{};
    auto doc{ s_parser.parse(body, len, capacity) };
    auto rootObj{ doc.get_object().value() };

    auto transaction{ GetVal<Obj>(rootObj, "transaction") };
    auto customer   { GetVal<Obj>(rootObj, "customer"   ) };
    auto merchant   { GetVal<Obj>(rootObj, "merchant"   ) };
    auto terminal   { GetVal<Obj>(rootObj, "terminal"   ) };

    auto lastTrans{ Get<Obj>(rootObj, "last_transaction") };


    const auto date{ FromStr(GetVal<Str>(transaction, "requested_at")) };
    const std::optional lastTransDate{
        lastTrans.error() == simdjson::error_code::SUCCESS
            ? std::optional<chr::sys_seconds>{ FromStr(GetVal<Str>(lastTrans.value(), "timestamp")) }
            : std::optional<chr::sys_seconds>{}
    };

    static constexpr auto& Gis{ GetVal<Int> };
    static constexpr auto& Gs{ GetVal<std::string_view> };
    static constexpr auto& Gf{ GetVal<float> };
    static constexpr auto& Gb{ GetVal<bool> };
    static constexpr auto& Ga{ GetVal<Arr> };

    static constexpr auto s_contains = [](const simdjson::dom::array arr, const std::string_view target) {
        for (auto val : arr) // NOLINT(*-use-anyofallof)
            if (val.get_string().value() == target) return true;
        return false;
    };

    const Query q{ std::simd::unchecked_load<Query>(std::array{
        Clamp01(Gf(transaction, "amount")                                               , Constants::maxAmount       ),
        Clamp01(Gf(transaction, "installments")                                         , Constants::maxInstallments ),
        Clamp01(Gf(transaction, "amount") / std::max(1.0f, Gf(customer, "avg_amount"))  , Constants::amountVsAvgRatio),
        Clamp01(chr::floor<chr::hours>(date.time_since_epoch() % chr::days{ 1 }).count(), 23                         ),
        Clamp01(chr::weekday{ chr::floor<chr::days>(date) }.iso_encoding() - 1          , 6                          ),

        !lastTransDate ? -1.f : Clamp01(chr::floor<chr::minutes>(date - *lastTransDate).count(), Constants::maxMinutes),
        !lastTransDate ? -1.f : Clamp01(Gf(lastTrans.value(), "km_from_current")               , Constants::maxKm     ),

        Clamp01(Gf(terminal, "km_from_home") / Constants::maxKm        ),
        Clamp01(Gf(customer, "tx_count_24h") / Constants::maxTxCount24H),
        static_cast<float>(Gb(terminal, "is_online"   )),
        static_cast<float>(Gb(terminal, "card_present")),
        static_cast<float>(!s_contains(Ga(customer, "known_merchants"), Gs(merchant, "id"))),
        S_GetMccRisk(Gis(merchant, "mcc")),
        Clamp01(Gf(merchant, "avg_amount"), Constants::maxMerchantAvgAmount),

        .0f, .0f
    }) };

    return Response::fraudResponses[Search(q)];

} catch (const simdjson::simdjson_error& e) {
    std::println("simdjson error: {}", e.what());
    return Response::serverErrResponse;
} catch (...) {
    std::println("unknown exception in parse");
    return Response::serverErrResponse;
}

void ProcessRequest(ClientState& state) {
    state.response = {};

    const std::string_view req{ state.socketView.data(), state.bodyIdx };

    if (req.starts_with("GET ") || req.starts_with("get ")) {
        state.response  = Response::readyResponse;
        state.keepAlive = false;
        return;
    }

    if (!req.starts_with("POST ") && !req.starts_with("post ")) {
        state.response = Response::notFoundResponse;

        return;
    }

    state.response = S_CreateJson(
        state.socketView.data() + state.bodyIdx,
        state.contentLength,
        state.socketView.capacity() - state.bodyIdx
    );
}