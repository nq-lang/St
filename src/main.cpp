// ---------------------------------------------------------------------------
// main.cpp — QQQ Options Data Terminal
//
// Single-file build by design: data model, Black-Scholes greeks, mock data
// generator, IBKR stub, and all render panels (barcharts / 3D surfaces /
// heatmap / summary) live in this one file. theme.hpp is the only split-out
// header, and it's pure color constants — nothing to debug there.
//
// Sections in this file, in order:
//   1. Data model            (MarketState and friends)
//   2. Greeks math            (Black-Scholes, pure functions)
//   3. Mock data feed         (works today, no network)
//   4. IBKR stub              (TODO-marked, not wired into render loop yet)
//   5. Render: barcharts row  (Row 1, 8 charts)
//   6. Render: summary panel  (Row 1, 9th cell)
//   7. Render: 3D surfaces    (Row 2)
//   8. Render: IV heatmap     (Row 3)
//   9. App / main loop
// ---------------------------------------------------------------------------

#include "theme.hpp"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "implot.h"
#ifdef TERMINAL_HAS_IMPLOT3D
#include "implot3d.h"
#endif

#include <GLFW/glfw3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <expected>
#include <format>
#include <numbers>
#include <ranges>
#include <string>
#include <vector>

// ===========================================================================
// 1. DATA MODEL
// ===========================================================================
namespace data {

enum class FeedError { NotConnected, SubscriptionFailed, MalformedTick, Timeout, RateLimited, Unknown };
template <class T> using FeedResult = std::expected<T, FeedError>;

struct StrikeRow {
    double strike{};
    double call_oi{}, put_oi{};
    double call_iv{}, put_iv{};
    double call_gamma{}, put_gamma{};
    double call_delta{}, put_delta{};
    double call_vanna{}, put_vanna{};
    double call_charm{}, put_charm{};
    double call_theta{}, put_theta{};
};

struct ExposureSeries {
    std::vector<double> strikes, calls, puts, net;
};

struct SkewCurve {
    std::vector<double> strikes, call_iv, put_iv;
};

struct IVSurface {
    int n_moneyness{0}, n_dte{0};
    std::vector<double> moneyness_axis, dte_axis;
    std::vector<float> iv_grid;  // row-major [dte][moneyness]

    [[nodiscard]] float at(int dte_idx, int m_idx) const noexcept {
        return iv_grid[static_cast<size_t>(dte_idx) * static_cast<size_t>(n_moneyness) + static_cast<size_t>(m_idx)];
    }
};

struct IVHeatmapCell {
    double iv_pct{};
    bool has_data{true};
};

struct IVHeatmap {
    std::vector<double> strikes;
    std::vector<std::string> dte_labels;
    std::vector<IVHeatmapCell> cells;  // [strike][dte], row-major
    double low_iv{}, high_iv{};

    [[nodiscard]] const IVHeatmapCell& at(size_t strike_idx, size_t dte_idx) const noexcept {
        return cells[strike_idx * dte_labels.size() + dte_idx];
    }
};

struct GreeksSummary {
    std::chrono::system_clock::time_point timestamp{};
    double spot{}, vix{};
    double atm_iv_0dte{}, atm_iv_30d{};
    double net_gex{}, net_dex{}, net_charm_per_day{}, net_vanna{}, net_theta_per_day{};
    double call_wall{}, put_wall{}, gamma_flip{};
};

enum class ConnectionStatus { Disconnected, Connecting, Streaming, Degraded, MockData };

struct MarketState {
    ConnectionStatus status{ConnectionStatus::MockData};
    std::string underlying{"QQQ"};
    std::vector<StrikeRow> chain;
    ExposureSeries oi, gex, dex, vanna, charm, theta;
    SkewCurve skew_0dte_vs_30d;
    std::vector<double> pc_skew_ratio_strikes, pc_skew_ratio;
    IVSurface put_surface, call_surface;
    IVHeatmap heatmap;
    GreeksSummary summary;
};

}  // namespace data

// ===========================================================================
// 2. GREEKS MATH — pure Black-Scholes, no I/O
// ===========================================================================
namespace greeks {

constexpr double norm_pdf(double x) noexcept {
    return std::exp(-0.5 * x * x) / std::sqrt(2.0 * std::numbers::pi);
}
inline double norm_cdf(double x) noexcept { return 0.5 * std::erfc(-x / std::numbers::sqrt2); }

struct BSInputs {
    double spot, strike, t_years, vol, rate{0.045}, div_yield{0.0};
};

struct BSGreeks {
    double delta_call, delta_put, gamma, vega;
    double theta_call, theta_put, vanna, charm_call, charm_put;
};

[[nodiscard]] inline BSGreeks black_scholes_greeks(const BSInputs& in) noexcept {
    const double t = std::max(in.t_years, 1.0 / 365.0 / 24.0);
    const double sqrt_t = std::sqrt(t);
    const double d1 = (std::log(in.spot / in.strike) + (in.rate - in.div_yield + 0.5 * in.vol * in.vol) * t) /
                       (in.vol * sqrt_t);
    const double d2 = d1 - in.vol * sqrt_t;
    const double disc_r = std::exp(-in.rate * t);
    const double disc_q = std::exp(-in.div_yield * t);
    const double pdf_d1 = norm_pdf(d1);

    BSGreeks g{};
    g.delta_call = disc_q * norm_cdf(d1);
    g.delta_put  = disc_q * (norm_cdf(d1) - 1.0);
    g.gamma      = disc_q * pdf_d1 / (in.spot * in.vol * sqrt_t);
    g.vega       = in.spot * disc_q * pdf_d1 * sqrt_t;
    g.theta_call = (-in.spot * disc_q * pdf_d1 * in.vol / (2.0 * sqrt_t)) - in.rate * in.strike * disc_r * norm_cdf(d2) +
                   in.div_yield * in.spot * disc_q * norm_cdf(d1);
    g.theta_put  = (-in.spot * disc_q * pdf_d1 * in.vol / (2.0 * sqrt_t)) + in.rate * in.strike * disc_r * norm_cdf(-d2) -
                   in.div_yield * in.spot * disc_q * norm_cdf(-d1);
    g.vanna = -disc_q * pdf_d1 * d2 / in.vol;
    const double charm_common =
        -disc_q * pdf_d1 * (2.0 * (in.rate - in.div_yield) * t - d2 * in.vol * sqrt_t) / (2.0 * t * in.vol * sqrt_t);
    g.charm_call = charm_common - in.div_yield * disc_q * norm_cdf(d1);
    g.charm_put  = charm_common + in.div_yield * disc_q * norm_cdf(-d1);
    return g;
}

[[nodiscard]] inline double dollar_gamma(double gamma, double oi, double spot) noexcept {
    return gamma * oi * 100.0 * spot * spot * 0.01;
}
[[nodiscard]] inline double dollar_delta(double delta, double oi, double spot) noexcept {
    return delta * oi * 100.0 * spot;
}

template <std::ranges::input_range CallRange, std::ranges::input_range PutRange>
[[nodiscard]] inline double net_sum(CallRange&& calls, PutRange&& puts) noexcept {
    double c = 0.0, p = 0.0;
    for (auto v : calls) c += v;
    for (auto v : puts) p += v;
    return c - p;
}

}  // namespace greeks

// ===========================================================================
// 3. MOCK DATA FEED — works today, no network. Same output type/shape as
//    the future IBKR feed (see section 4), so swapping data sources later
//    means changing the call site in run(), not this struct's contract.
// ===========================================================================
namespace mockfeed {

constexpr uint64_t xorshift64s(uint64_t& state) noexcept {
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    return state * 0x2545F4914F6CDD1DULL;
}

class MockFeed {
public:
    explicit MockFeed(double base_spot = 485.0) noexcept
        : base_spot_(base_spot), spot_(base_spot), rng_state_(0x9E3779B97F4A7C15ULL) {}

    [[nodiscard]] data::MarketState next_snapshot() {
        t_sim_ += 0.25;
        const double wobble = std::sin(t_sim_ * 0.05) * 0.6 + (rand01() - 0.5) * 0.15;
        spot_ = std::lerp(spot_, base_spot_ + wobble, 0.08);

        data::MarketState st;
        st.status = data::ConnectionStatus::MockData;
        st.underlying = "QQQ";

        constexpr int kNumStrikes = 61;
        constexpr double kStrikeStep = 1.0;
        const double lowest = std::round(spot_ / kStrikeStep) * kStrikeStep - (kNumStrikes / 2) * kStrikeStep;
        const double call_wall_center = spot_ + 5.0;
        const double put_wall_center = spot_ - 10.0;

        st.chain.reserve(kNumStrikes);
        for (int i = 0; i < kNumStrikes; ++i) {
            const double k = lowest + i * kStrikeStep;
            data::StrikeRow row;
            row.strike = k;

            const double call_dist = (k - call_wall_center) / 12.0;
            const double put_dist = (k - put_wall_center) / 14.0;
            row.call_oi = 50000.0 * std::exp(-call_dist * call_dist);
            row.put_oi = 45000.0 * std::exp(-put_dist * put_dist);

            const double moneyness = (k - spot_) / spot_;
            row.call_iv = std::clamp(0.13 - moneyness * 0.35 + std::max(0.0, moneyness) * 0.15, 0.08, 0.60);
            row.put_iv  = std::clamp(0.13 - moneyness * 0.55 + std::max(0.0, -moneyness) * 0.05, 0.08, 0.83);

            constexpr double t_0dte = 0.5 / 365.0;
            auto cg = greeks::black_scholes_greeks({.spot = spot_, .strike = k, .t_years = t_0dte, .vol = row.call_iv});
            auto pg = greeks::black_scholes_greeks({.spot = spot_, .strike = k, .t_years = t_0dte, .vol = row.put_iv});

            row.call_gamma = cg.gamma; row.put_gamma = pg.gamma;
            row.call_delta = cg.delta_call; row.put_delta = pg.delta_put;
            row.call_vanna = cg.vanna; row.put_vanna = pg.vanna;
            row.call_charm = cg.charm_call; row.put_charm = pg.charm_put;
            row.call_theta = cg.theta_call / 365.0; row.put_theta = pg.theta_put / 365.0;

            st.chain.push_back(row);
        }

        std::vector<double> strikes;
        strikes.reserve(st.chain.size());
        for (auto& r : st.chain) strikes.push_back(r.strike);
        st.oi.strikes = st.gex.strikes = st.dex.strikes = st.vanna.strikes = st.charm.strikes = st.theta.strikes = strikes;

        double max_call_oi = 0.0, max_put_oi = 0.0, call_wall = spot_, put_wall = spot_;
        double running_gex = 0.0, gamma_flip = spot_, prev_gex_sign = 0.0;
        bool flip_found = false;

        for (const auto& row : st.chain) {
            st.oi.calls.push_back(row.call_oi);
            st.oi.puts.push_back(row.put_oi);

            const double call_gex = greeks::dollar_gamma(row.call_gamma, row.call_oi, spot_);
            const double put_gex = -greeks::dollar_gamma(row.put_gamma, row.put_oi, spot_);
            st.gex.calls.push_back(call_gex); st.gex.puts.push_back(put_gex); st.gex.net.push_back(call_gex + put_gex);
            running_gex += call_gex + put_gex;

            const double gex_sign = (call_gex + put_gex) >= 0 ? 1.0 : -1.0;
            if (prev_gex_sign != 0.0 && gex_sign != prev_gex_sign && !flip_found) { gamma_flip = row.strike; flip_found = true; }
            prev_gex_sign = gex_sign;

            const double call_dex = greeks::dollar_delta(row.call_delta, row.call_oi, spot_);
            const double put_dex = greeks::dollar_delta(row.put_delta, row.put_oi, spot_);
            st.dex.calls.push_back(call_dex); st.dex.puts.push_back(put_dex); st.dex.net.push_back(call_dex + put_dex);

            const double call_vanna_e = row.call_vanna * row.call_oi * 100.0;
            const double put_vanna_e = row.put_vanna * row.put_oi * 100.0;
            st.vanna.calls.push_back(call_vanna_e); st.vanna.puts.push_back(put_vanna_e);
            st.vanna.net.push_back(call_vanna_e - put_vanna_e);

            const double call_charm_e = row.call_charm * row.call_oi * 100.0;
            const double put_charm_e = row.put_charm * row.put_oi * 100.0;
            st.charm.calls.push_back(call_charm_e); st.charm.puts.push_back(put_charm_e);
            st.charm.net.push_back(call_charm_e - put_charm_e);

            const double call_theta_e = row.call_theta * row.call_oi * 100.0;
            const double put_theta_e = row.put_theta * row.put_oi * 100.0;
            st.theta.calls.push_back(call_theta_e); st.theta.puts.push_back(put_theta_e);
            st.theta.net.push_back(call_theta_e + put_theta_e);

            if (row.call_oi > max_call_oi) { max_call_oi = row.call_oi; call_wall = row.strike; }
            if (row.put_oi > max_put_oi) { max_put_oi = row.put_oi; put_wall = row.strike; }
        }

        st.skew_0dte_vs_30d.strikes = strikes;
        st.pc_skew_ratio_strikes = strikes;
        for (const auto& row : st.chain) {
            st.skew_0dte_vs_30d.call_iv.push_back(row.call_iv * 100.0);
            st.skew_0dte_vs_30d.put_iv.push_back(row.put_iv * 100.0);
            const double ratio = row.call_iv > 1e-6 ? (row.put_iv / row.call_iv - 1.0) : 0.0;
            st.pc_skew_ratio.push_back(ratio + (rand01() - 0.5) * 0.0002);
        }

        st.put_surface = build_surface(false);
        st.call_surface = build_surface(true);
        st.heatmap = build_heatmap();

        data::GreeksSummary sum;
        sum.timestamp = std::chrono::system_clock::now();
        sum.spot = spot_;
        sum.vix = 17.40 + std::sin(t_sim_ * 0.03) * 0.8;
        sum.atm_iv_0dte = 10.09 + (rand01() - 0.5) * 0.3;
        sum.atm_iv_30d = 13.00 + (rand01() - 0.5) * 0.2;
        sum.net_gex = running_gex;
        sum.net_dex = greeks::net_sum(st.dex.calls, st.dex.puts);
        sum.net_charm_per_day = greeks::net_sum(st.charm.calls, st.charm.puts);
        sum.net_vanna = greeks::net_sum(st.vanna.calls, st.vanna.puts);
        sum.net_theta_per_day = greeks::net_sum(st.theta.calls, st.theta.puts);
        sum.call_wall = call_wall; sum.put_wall = put_wall; sum.gamma_flip = gamma_flip;
        st.summary = sum;

        return st;
    }

private:
    double base_spot_, spot_, t_sim_{0.0};
    uint64_t rng_state_;

    double rand01() noexcept { return static_cast<double>(xorshift64s(rng_state_) >> 11) * (1.0 / 9007199254740992.0); }

    data::IVSurface build_surface(bool is_call) {
        data::IVSurface surf;
        surf.n_moneyness = 30; surf.n_dte = 30;
        surf.moneyness_axis.resize(30); surf.dte_axis.resize(30); surf.iv_grid.resize(30 * 30);
        for (int i = 0; i < 30; ++i) surf.moneyness_axis[i] = 0.85 + i * (0.30 / 29.0);
        for (int j = 0; j < 30; ++j) surf.dte_axis[j] = j * (90.0 / 29.0);

        for (int dte_i = 0; dte_i < 30; ++dte_i) {
            const double dte = surf.dte_axis[dte_i];
            for (int m_i = 0; m_i < 30; ++m_i) {
                const double moneyness = surf.moneyness_axis[m_i];
                const double log_m = std::log(moneyness);
                double base_iv = 0.11 + 0.35 * log_m * log_m;
                base_iv += is_call ? std::max(0.0, log_m) * 0.10 : std::max(0.0, -log_m) * 0.22;
                base_iv *= (1.0 - 0.15 * std::min(dte, 60.0) / 60.0);
                base_iv = std::clamp(base_iv, 0.10, 0.85);
                surf.iv_grid[static_cast<size_t>(dte_i) * 30 + static_cast<size_t>(m_i)] = static_cast<float>(base_iv * 100.0);
            }
        }
        return surf;
    }

    data::IVHeatmap build_heatmap() {
        data::IVHeatmap hm;
        hm.dte_labels = {"0d", "3d", "4d", "5d", "6d", "7d", "10d", "11d", "12d", "13d"};
        constexpr int kHeatStrikes = 58;
        const double hm_top = std::round(spot_) + kHeatStrikes / 2.0;
        hm.strikes.reserve(kHeatStrikes);
        for (int i = 0; i < kHeatStrikes; ++i) hm.strikes.push_back(hm_top - i);

        hm.cells.resize(hm.strikes.size() * hm.dte_labels.size());
        hm.low_iv = 100.0; hm.high_iv = 0.0;
        for (size_t si = 0; si < hm.strikes.size(); ++si) {
            const double dist = std::abs(hm.strikes[si] - spot_);
            for (size_t di = 0; di < hm.dte_labels.size(); ++di) {
                data::IVHeatmapCell cell;
                cell.has_data = !(di == 3 && dist > 10.0 && (static_cast<int>(si) % 3 != 0));
                double iv = std::clamp(11.0 + dist * 1.35 + rand01() * 2.0 - di * 0.15, 8.0, 90.0);
                cell.iv_pct = iv;
                hm.low_iv = std::min(hm.low_iv, iv);
                hm.high_iv = std::max(hm.high_iv, iv);
                hm.cells[si * hm.dte_labels.size() + di] = cell;
            }
        }
        return hm;
    }
};

}  // namespace mockfeed

// ===========================================================================
// 4. IBKR STUB — not wired into the render loop yet. Same MarketState output
//    contract as MockFeed. Every TODO marks exactly where a real TWS API
//    call belongs once you're ready for phase 2 (see README).
// ===========================================================================
namespace ibkr {

struct ConnectionConfig {
    std::string host{"127.0.0.1"};
    int port{7497};  // 7497=TWS paper, 7496=TWS live, 4002=Gateway paper, 4001=Gateway live
    int client_id{1};
    std::string symbol{"QQQ"};
};

class IbkrClient {
public:
    explicit IbkrClient(ConnectionConfig cfg) : cfg_(std::move(cfg)) {}

    [[nodiscard]] data::FeedResult<void> connect_and_subscribe() {
#ifndef TERMINAL_HAS_IBKR
        status_ = data::ConnectionStatus::Disconnected;
        return std::unexpected(data::FeedError::NotConnected);
#else
        status_ = data::ConnectionStatus::Connecting;
        // TODO(live-data): EClientSocket* client = new EClientSocket(&wrapper, &signal);
        // TODO(live-data): client->eConnect(cfg_.host.c_str(), cfg_.port, cfg_.client_id);
        // TODO(live-data): spin up EReader thread; call reqMktData()/reqSecDefOptParams()
        //                  for cfg_.symbol; on tickOptionComputation callbacks, fill a
        //                  MarketState using the SAME greeks::black_scholes_greeks() calls
        //                  MockFeed uses for vanna/charm (IBKR streams delta/gamma/theta/
        //                  vega natively, NOT vanna/charm — those must be derived here).
        status_ = data::ConnectionStatus::Streaming;
        return {};
#endif
    }

    void disconnect() { status_ = data::ConnectionStatus::Disconnected; /* TODO(live-data): eDisconnect() */ }
    [[nodiscard]] data::ConnectionStatus status() const noexcept { return status_; }

    [[nodiscard]] data::FeedResult<data::MarketState> try_get_latest_snapshot() {
        if (status_ != data::ConnectionStatus::Streaming) return std::unexpected(data::FeedError::NotConnected);
        // TODO(live-data): return the completed double-buffered MarketState here.
        return std::unexpected(data::FeedError::NotConnected);
    }

private:
    ConnectionConfig cfg_;
    data::ConnectionStatus status_{data::ConnectionStatus::Disconnected};
};

}  // namespace ibkr

// ===========================================================================
// 5. RENDER: BARCHARTS ROW (Row 1, 8 charts)
// ===========================================================================
namespace render {

enum class BarOrientation { Vertical, Horizontal };
enum class SurfaceDteRange { D30, D60, D90 };

// Shared across barcharts + surface fallback (section 7), so it lives at
// render:: scope rather than inside either section's anonymous namespace.
constexpr ImPlotFlags kPlotFlags = ImPlotFlags_NoTitle | ImPlotFlags_NoLegend | ImPlotFlags_NoMenus;

namespace {

void draw_spot_line(double spot) {
    ImPlotRect limits = ImPlot::GetPlotLimits();
    double xs[2] = {spot, spot};
    double ys[2] = {limits.Y.Min, limits.Y.Max};
    ImPlot::SetNextLineStyle(theme::rgb(theme::kYellow), 1.5f);
    ImPlot::PlotLine("##spotline", xs, ys, 2);
}

void begin_chart_child(const char* id, float w, float h) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::rgb(theme::kBgPanel));
    ImGui::PushStyleColor(ImGuiCol_Border, theme::rgb(theme::kBorder));
    ImGui::BeginChild(id, ImVec2(w, h), ImGuiChildFlags_Border);
}
void end_chart_child() { ImGui::EndChild(); ImGui::PopStyleColor(2); }

void chart_title(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, theme::rgb(theme::kTextPrimary));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

void setup_axes_once(double x_min, double x_max, double y_min, double y_max) {
    ImPlot::SetupAxisLimits(ImAxis_X1, x_min, x_max, ImPlotCond_Once);
    ImPlot::SetupAxisLimits(ImAxis_Y1, y_min, y_max, ImPlotCond_Once);
}

double vec_min(const std::vector<double>& v) { double m = 0; for (double x : v) m = std::min(m, x); return m; }
double vec_max(const std::vector<double>& v) { double m = 0; for (double x : v) m = std::max(m, x); return m; }

void plot_dual_bars(const char* id, const data::ExposureSeries& s, double spot, BarOrientation orient,
                     uint32_t call_color, uint32_t put_color) {
    if (s.strikes.empty()) return;
    const double bar_w = 0.8;
    if (ImPlot::BeginPlot(id, ImVec2(-1, -1), kPlotFlags)) {
        double ymax = std::max(vec_max(s.calls), vec_max(s.puts));
        setup_axes_once(s.strikes.front(), s.strikes.back(), 0.0, ymax * 1.05 + 1e-6);
        int n = static_cast<int>(s.strikes.size());
        if (orient == BarOrientation::Vertical) {
            ImPlot::SetNextFillStyle(theme::rgb(call_color));
            ImPlot::PlotBars("Calls", s.strikes.data(), s.calls.data(), n, bar_w);
            ImPlot::SetNextFillStyle(theme::rgb(put_color));
            ImPlot::PlotBars("Puts", s.strikes.data(), s.puts.data(), n, bar_w);
        } else {
            ImPlot::SetNextFillStyle(theme::rgb(call_color));
            ImPlot::PlotBarsH("Calls", s.strikes.data(), s.calls.data(), n, bar_w);
            ImPlot::SetNextFillStyle(theme::rgb(put_color));
            ImPlot::PlotBarsH("Puts", s.strikes.data(), s.puts.data(), n, bar_w);
        }
        draw_spot_line(spot);
        ImPlot::EndPlot();
    }
}

void plot_signed_bars(const char* id, const std::vector<double>& strikes, const std::vector<double>& values,
                       double spot, BarOrientation orient) {
    if (strikes.empty()) return;
    if (ImPlot::BeginPlot(id, ImVec2(-1, -1), kPlotFlags)) {
        double ymin = vec_min(values), ymax = vec_max(values);
        double pad = (ymax - ymin) * 0.05 + 1e-9;
        setup_axes_once(strikes.front(), strikes.back(), ymin - pad, ymax + pad);

        std::vector<double> pos_x, pos_y, neg_x, neg_y;
        for (size_t i = 0; i < strikes.size(); ++i) {
            if (values[i] >= 0) { pos_x.push_back(strikes[i]); pos_y.push_back(values[i]); }
            else { neg_x.push_back(strikes[i]); neg_y.push_back(values[i]); }
        }
        const double bar_w = 0.8;
        if (orient == BarOrientation::Vertical) {
            if (!pos_x.empty()) { ImPlot::SetNextFillStyle(theme::rgb(theme::kEmerald)); ImPlot::PlotBars("Pos", pos_x.data(), pos_y.data(), (int)pos_x.size(), bar_w); }
            if (!neg_x.empty()) { ImPlot::SetNextFillStyle(theme::rgb(theme::kRed)); ImPlot::PlotBars("Neg", neg_x.data(), neg_y.data(), (int)neg_x.size(), bar_w); }
        } else {
            if (!pos_x.empty()) { ImPlot::SetNextFillStyle(theme::rgb(theme::kEmerald)); ImPlot::PlotBarsH("Pos", pos_x.data(), pos_y.data(), (int)pos_x.size(), bar_w); }
            if (!neg_x.empty()) { ImPlot::SetNextFillStyle(theme::rgb(theme::kRed)); ImPlot::PlotBarsH("Neg", neg_x.data(), neg_y.data(), (int)neg_x.size(), bar_w); }
        }
        draw_spot_line(spot);
        ImPlot::EndPlot();
    }
}

void plot_skew_lines(const char* id, const data::SkewCurve& skew, double spot) {
    if (skew.strikes.empty()) return;
    if (ImPlot::BeginPlot(id, ImVec2(-1, -1), kPlotFlags)) {
        double ymin = std::min(vec_min(skew.call_iv), vec_min(skew.put_iv));
        double ymax = std::max(vec_max(skew.call_iv), vec_max(skew.put_iv));
        setup_axes_once(skew.strikes.front(), skew.strikes.back(), ymin * 0.9, ymax * 1.1 + 1e-6);
        ImPlot::SetNextLineStyle(theme::rgb(theme::kBlue), 1.5f);
        ImPlot::PlotLine("Calls", skew.strikes.data(), skew.call_iv.data(), static_cast<int>(skew.strikes.size()));
        ImPlot::SetNextLineStyle(theme::rgb(theme::kRed), 1.5f);
        ImPlot::PlotLine("Puts", skew.strikes.data(), skew.put_iv.data(), static_cast<int>(skew.strikes.size()));
        draw_spot_line(spot);
        ImPlot::EndPlot();
    }
}

void plot_ratio_line(const char* id, const std::vector<double>& strikes, const std::vector<double>& ratio, double spot) {
    if (strikes.empty()) return;
    if (ImPlot::BeginPlot(id, ImVec2(-1, -1), kPlotFlags)) {
        double ymin = vec_min(ratio), ymax = vec_max(ratio);
        double pad = (ymax - ymin) * 0.1 + 1e-9;
        setup_axes_once(strikes.front(), strikes.back(), ymin - pad, ymax + pad);
        ImPlot::SetNextLineStyle(theme::rgb(theme::kPurple), 1.5f);
        ImPlot::PlotLine("PCRatio", strikes.data(), ratio.data(), static_cast<int>(strikes.size()));
        draw_spot_line(spot);
        ImPlot::EndPlot();
    }
}

}  // namespace

void draw_barcharts_row(const data::MarketState& state, BarOrientation orientation, float cell_w, float cell_h) {
    const double spot = state.summary.spot;
    ImGui::PushID("barcharts_row");

    auto cell = [&](const char* label, const char* plot_id, auto&& draw_fn) {
        begin_chart_child(label, cell_w, cell_h);
        chart_title(label);
        draw_fn(plot_id);
        end_chart_child();
    };

    cell("Open Interest by Strike", "##oi", [&](const char* id) { plot_dual_bars(id, state.oi, spot, orientation, theme::kBlue, theme::kRed); });
    ImGui::SameLine();
    cell("Gamma Exposure (GEX)", "##gex", [&](const char* id) { plot_signed_bars(id, state.gex.strikes, state.gex.net, spot, orientation); });
    ImGui::SameLine();
    cell("Delta Exposure (DEX)", "##dex", [&](const char* id) { plot_signed_bars(id, state.dex.strikes, state.dex.net, spot, orientation); });

    cell("Vanna Exposure", "##vanna", [&](const char* id) { plot_signed_bars(id, state.vanna.strikes, state.vanna.net, spot, orientation); });
    ImGui::SameLine();
    cell("Charm Exposure", "##charm", [&](const char* id) { plot_signed_bars(id, state.charm.strikes, state.charm.net, spot, orientation); });
    ImGui::SameLine();
    cell("Theta Exposure (ThEx)", "##theta", [&](const char* id) { plot_signed_bars(id, state.theta.strikes, state.theta.net, spot, orientation); });

    cell("IV Skew - 0DTE vs ~30d", "##skew", [&](const char* id) { plot_skew_lines(id, state.skew_0dte_vs_30d, spot); });
    ImGui::SameLine();
    cell("P/C IV Skew Ratio", "##ratio", [&](const char* id) { plot_ratio_line(id, state.pc_skew_ratio_strikes, state.pc_skew_ratio, spot); });

    ImGui::PopID();
}

// ===========================================================================
// 6. RENDER: SUMMARY PANEL (Row 1, 9th cell)
// ===========================================================================
namespace {

std::string fmt_magnitude(double v) {
    const double av = std::abs(v);
    const char sign = v >= 0 ? '+' : '-';
    if (av >= 1e9) return std::format("{}{:.2f}B", sign, av / 1e9);
    if (av >= 1e6) return std::format("{}{:.0f}M", sign, av / 1e6);
    if (av >= 1e3) return std::format("{}{:.0f}K", sign, av / 1e3);
    return std::format("{}{:.0f}", sign, av);
}

void summary_row(const char* label, const std::string& value, uint32_t color) {
    ImGui::PushStyleColor(ImGuiCol_Text, theme::rgb(theme::kTextMuted));
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    float avail = ImGui::GetContentRegionAvail().x;
    ImVec2 text_size = ImGui::CalcTextSize(value.c_str());
    ImGui::SameLine(ImGui::GetCursorPosX() + avail - text_size.x);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::rgb(color));
    ImGui::TextUnformatted(value.c_str());
    ImGui::PopStyleColor();
}

}  // namespace

void draw_summary_panel(const data::MarketState& state, float width, float height) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::rgb(theme::kBgPanel));
    ImGui::PushStyleColor(ImGuiCol_Border, theme::rgb(theme::kBorder));
    ImGui::BeginChild("##summary_panel", ImVec2(width, height), ImGuiChildFlags_Border);

    const auto& s = state.summary;
    {
        const char* title = "QQQ GREEKS SUMMARY";
        float avail = ImGui::GetContentRegionAvail().x;
        ImGui::SetCursorPosX((avail - ImGui::CalcTextSize(title).x) * 0.5f);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::rgb(0xffffff));
        ImGui::TextUnformatted(title);
        ImGui::PopStyleColor();

        auto tt = std::chrono::floor<std::chrono::seconds>(s.timestamp);
        std::string ts = std::format("{:%Y-%m-%d %H:%M} ET", tt);
        ImGui::SetCursorPosX((avail - ImGui::CalcTextSize(ts.c_str()).x) * 0.5f);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::rgb(theme::kTextMuted));
        ImGui::TextUnformatted(ts.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::Dummy(ImVec2(0, 10));
    summary_row("SPOT", std::format("{:.2f}", s.spot), theme::kYellow);
    summary_row("VIX", std::format("{:.2f}", s.vix), theme::kGreen);
    summary_row("ATM IV 0DTE", std::format("{:.2f}%", s.atm_iv_0dte), theme::kTextPrimary);
    summary_row("ATM IV 30d", std::format("{:.2f}%", s.atm_iv_30d), theme::kTextPrimary);

    ImGui::Dummy(ImVec2(0, 10));
    summary_row("Net GEX (all exp)", fmt_magnitude(s.net_gex), s.net_gex >= 0 ? theme::kGreen : theme::kRed);
    summary_row("Net DEX (all exp)", fmt_magnitude(s.net_dex), s.net_dex >= 0 ? theme::kGreen : theme::kRed);
    summary_row("Net Charm/day", fmt_magnitude(s.net_charm_per_day), s.net_charm_per_day >= 0 ? theme::kGreen : theme::kRed);
    summary_row("Net Vanna", fmt_magnitude(s.net_vanna), s.net_vanna >= 0 ? theme::kGreen : theme::kRed);
    summary_row("Net Theta/day", fmt_magnitude(s.net_theta_per_day), s.net_theta_per_day >= 0 ? theme::kGreen : theme::kRed);

    ImGui::Dummy(ImVec2(0, 10));
    summary_row("Call Wall (max OI)", std::format("{:.2f}", s.call_wall), theme::kBlue);
    summary_row("Put Wall (max OI)", std::format("{:.2f}", s.put_wall), theme::kRed);
    summary_row("Gamma Flip", std::format("{:.2f}", s.gamma_flip), theme::kOrange);

    ImGui::EndChild();
    ImGui::PopStyleColor(2);
}

// ===========================================================================
// 7. RENDER: 3D IV SURFACES (Row 2)
// ===========================================================================
namespace {

data::IVSurface clip_to_dte(const data::IVSurface& src, double max_dte) {
    data::IVSurface out;
    out.n_moneyness = src.n_moneyness;
    out.moneyness_axis = src.moneyness_axis;
    int cutoff = 0;
    while (cutoff < src.n_dte && src.dte_axis[static_cast<size_t>(cutoff)] <= max_dte) ++cutoff;
    cutoff = std::max(cutoff, 2);
    out.n_dte = cutoff;
    out.dte_axis.assign(src.dte_axis.begin(), src.dte_axis.begin() + cutoff);
    out.iv_grid.assign(src.iv_grid.begin(), src.iv_grid.begin() + static_cast<long>(cutoff) * src.n_moneyness);
    return out;
}

#ifdef TERMINAL_HAS_IMPLOT3D
void draw_surface_3d(const char* id, const data::IVSurface& surf, ImPlot3DColormap cmap) {
    if (ImPlot3D::BeginPlot(id, ImVec2(-1, -1))) {
        ImPlot3D::SetupAxes("Moneyness K/S", "Days to Expiry", "Implied Vol (%)");
        ImPlot3D::PushColormap(cmap);
        ImPlot3D::PlotSurface("iv", surf.moneyness_axis.data(), surf.dte_axis.data(), surf.iv_grid.data(),
                               surf.n_moneyness, surf.n_dte);
        ImPlot3D::PopColormap();
        ImPlot3D::EndPlot();
    }
}
#else
// Fallback when ImPlot3D isn't available: a 2D heat-slice (moneyness x DTE,
// color = IV) using plain ImPlot. Not a literal 3D mesh, but conveys the
// same information and needs zero extra GL plumbing — appropriate for a
// "keep it simple" single-file build. Flip TERMINAL_USE_IMPLOT3D=ON for
// the true 3D surface.
void draw_surface_3d(const char* id, const data::IVSurface& surf, uint32_t /*unused*/) {
    if (surf.n_dte < 2 || surf.n_moneyness < 2) return;
    if (ImPlot::BeginPlot(id, ImVec2(-1, -1), render::kPlotFlags)) {
        ImPlot::SetupAxes("Moneyness K/S", "Days to Expiry");
        ImPlotHeatmapFlags flags = 0;
        double bounds_min[2] = {surf.moneyness_axis.front(), surf.dte_axis.front()};
        double bounds_max[2] = {surf.moneyness_axis.back(), surf.dte_axis.back()};
        ImPlot::PlotHeatmap(id, surf.iv_grid.data(), surf.n_dte, surf.n_moneyness, 0.0, 0.0, nullptr,
                             ImPlotPoint(bounds_min[0], bounds_min[1]), ImPlotPoint(bounds_max[0], bounds_max[1]), flags);
        ImPlot::EndPlot();
    }
}
#endif

}  // namespace

void draw_surfaces_row(const data::MarketState& state, SurfaceDteRange range, float width, float height) {
    double max_dte = range == SurfaceDteRange::D30 ? 30.0 : range == SurfaceDteRange::D60 ? 60.0 : 90.0;
    data::IVSurface puts = clip_to_dte(state.put_surface, max_dte);
    data::IVSurface calls = clip_to_dte(state.call_surface, max_dte);

    float panel_w = (width - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::rgb(theme::kBgPanel));
    ImGui::PushStyleColor(ImGuiCol_Border, theme::rgb(theme::kBorder));

    ImGui::BeginChild("##surf_puts", ImVec2(panel_w, height), ImGuiChildFlags_Border);
    ImGui::TextUnformatted("PUTS Implied Volatility");
#ifdef TERMINAL_HAS_IMPLOT3D
    draw_surface_3d("##putsurf", puts, ImPlot3DColormap_Viridis);
#else
    draw_surface_3d("##putsurf", puts, 0u);
#endif
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("##surf_calls", ImVec2(panel_w, height), ImGuiChildFlags_Border);
    ImGui::TextUnformatted("CALLS Implied Volatility");
#ifdef TERMINAL_HAS_IMPLOT3D
    draw_surface_3d("##callsurf", calls, ImPlot3DColormap_Plasma);
#else
    draw_surface_3d("##callsurf", calls, 0u);
#endif
    ImGui::EndChild();

    ImGui::PopStyleColor(2);
}

// ===========================================================================
// 8. RENDER: IV HEATMAP TABLE (Row 3)
// ===========================================================================
void draw_heatmap_panel(const data::MarketState& state, float width) {
    const auto& hm = state.heatmap;
    if (hm.strikes.empty()) return;

    ImGui::PushStyleColor(ImGuiCol_Text, theme::rgb(theme::kCyan));
    ImGui::TextUnformatted("WHERE IS IMPLIED VOLATILITY CHEAP OR EXPENSIVE - BY STRIKE AND EXPIRY?");
    ImGui::PopStyleColor();

    ImGui::PushStyleColor(ImGuiCol_Text, theme::rgb(theme::kTextMuted));
    ImGui::Text("low %.1f%%  ->  high %.1f%%", hm.low_iv, hm.high_iv);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::rgb(theme::kGreen));
    ImGui::TextUnformatted("  CW=call wall");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::rgb(theme::kRed));
    ImGui::TextUnformatted("  PW=put wall");
    ImGui::PopStyleColor();

    const int n_cols = static_cast<int>(hm.dte_labels.size()) + 2;
    ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::rgb(theme::kBgBody));
    ImGui::BeginChild("##heatmap_scroll", ImVec2(width, 560), ImGuiChildFlags_Border);

    if (ImGui::BeginTable("iv_heatmap_table", n_cols, flags, ImVec2(0, 0))) {
        ImGui::TableSetupColumn("strike", ImGuiTableColumnFlags_WidthFixed, 55.0f);
        for (const auto& label : hm.dte_labels) ImGui::TableSetupColumn(label.c_str(), ImGuiTableColumnFlags_WidthFixed, 55.0f);
        ImGui::TableSetupColumn("wall", ImGuiTableColumnFlags_WidthFixed, 40.0f);
        ImGui::TableSetupScrollFreeze(1, 1);
        ImGui::TableHeadersRow();

        size_t atm_idx = 0;
        double best_dist = 1e18;
        for (size_t i = 0; i < hm.strikes.size(); ++i) {
            double d = std::abs(hm.strikes[i] - state.summary.spot);
            if (d < best_dist) { best_dist = d; atm_idx = i; }
        }

        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(hm.strikes.size()));
        while (clipper.Step()) {
            for (int row_i = clipper.DisplayStart; row_i < clipper.DisplayEnd; ++row_i) {
                const size_t si = static_cast<size_t>(row_i);
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                ImGui::PushStyleColor(ImGuiCol_Text, theme::rgb(theme::kTextPrimary));
                ImGui::Text("%.0f", hm.strikes[si]);
                ImGui::PopStyleColor();

                for (size_t di = 0; di < hm.dte_labels.size(); ++di) {
                    ImGui::TableSetColumnIndex(static_cast<int>(di) + 1);
                    const auto& cell = hm.at(si, di);
                    bool is_atm = (si == atm_idx && di == 0);

                    if (!cell.has_data) {
                        ImGui::PushStyleColor(ImGuiCol_Text, theme::rgb(theme::kHeatNul));
                        ImGui::TextUnformatted("....");
                        ImGui::PopStyleColor();
                        continue;
                    }

                    ImU32 bg = is_atm ? ImGui::ColorConvertFloat4ToU32(theme::rgb(theme::kAtmBg)) : theme::heatmap_color_for_iv(cell.iv_pct);
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, bg);

                    uint32_t fg = is_atm ? theme::kAtmFg : 0xffffff;
                    ImGui::PushStyleColor(ImGuiCol_Text, theme::rgb(fg));
                    std::string txt = std::format("{:.0f}%", cell.iv_pct);
                    float cw = ImGui::GetContentRegionAvail().x;
                    float tw = ImGui::CalcTextSize(txt.c_str()).x;
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (cw - tw) * 0.5f));
                    ImGui::TextUnformatted(txt.c_str());
                    ImGui::PopStyleColor();
                }

                ImGui::TableSetColumnIndex(n_cols - 1);
                double strike = hm.strikes[si];
                if (std::abs(strike - state.summary.call_wall) < 0.5) {
                    ImGui::PushStyleColor(ImGuiCol_Text, theme::rgb(theme::kGreen));
                    ImGui::TextUnformatted("CW");
                    ImGui::PopStyleColor();
                } else if (std::abs(strike - state.summary.put_wall) < 0.5) {
                    ImGui::PushStyleColor(ImGuiCol_Text, theme::rgb(theme::kRed));
                    ImGui::TextUnformatted("PW");
                    ImGui::PopStyleColor();
                }
            }
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

}  // namespace render

// ===========================================================================
// 9. APP / MAIN LOOP
// ===========================================================================
namespace {

void glfw_error_callback(int error, const char* description) {
    std::fprintf(stderr, "GLFW error %d: %s\n", error, description);
}

void apply_dark_style() {
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = theme::rgb(theme::kBgBody);
    colors[ImGuiCol_ChildBg] = theme::rgb(theme::kBgPanel);
    colors[ImGuiCol_PopupBg] = theme::rgb(theme::kBgPanel, 0.98f);
    colors[ImGuiCol_Border] = theme::rgb(theme::kBorder);
    colors[ImGuiCol_FrameBg] = theme::rgb(theme::kBgPanel);
    colors[ImGuiCol_FrameBgHovered] = theme::rgb(theme::kBorder);
    colors[ImGuiCol_FrameBgActive] = theme::rgb(theme::kBorder);
    colors[ImGuiCol_Text] = theme::rgb(theme::kTextPrimary);
    colors[ImGuiCol_TextDisabled] = theme::rgb(theme::kTextDim);
    colors[ImGuiCol_TitleBg] = theme::rgb(theme::kBgPanel);
    colors[ImGuiCol_TitleBgActive] = theme::rgb(theme::kBgPanel);
    colors[ImGuiCol_MenuBarBg] = theme::rgb(theme::kBgPanel);
    colors[ImGuiCol_Header] = theme::rgb(theme::kBorder);
    colors[ImGuiCol_HeaderHovered] = theme::rgb(theme::kBorder, 0.8f);
    colors[ImGuiCol_HeaderActive] = theme::rgb(theme::kBorder);
    colors[ImGuiCol_Button] = theme::rgb(theme::kBgPanel);
    colors[ImGuiCol_ButtonHovered] = theme::rgb(theme::kBorder);
    colors[ImGuiCol_ButtonActive] = theme::rgb(theme::kBorder);
    colors[ImGuiCol_TableHeaderBg] = theme::rgb(theme::kBgPanel);
    colors[ImGuiCol_TableBorderStrong] = theme::rgb(theme::kBorder);
    colors[ImGuiCol_TableBorderLight] = theme::rgb(theme::kGridLine, 0.5f);

    style.WindowRounding = 0.0f;
    style.ChildRounding = 2.0f;
    style.FrameRounding = 2.0f;
    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.FramePadding = ImVec2(6, 4);
    style.CellPadding = ImVec2(4, 3);
    style.ItemSpacing = ImVec2(8, 6);

    ImPlotStyle& pstyle = ImPlot::GetStyle();
    ImVec4* pcolors = pstyle.Colors;
    pcolors[ImPlotCol_FrameBg] = ImVec4(0, 0, 0, 0);
    pcolors[ImPlotCol_PlotBg] = ImVec4(0, 0, 0, 0);
    pcolors[ImPlotCol_PlotBorder] = theme::rgb(theme::kBorder);
    pcolors[ImPlotCol_LegendBg] = theme::rgb(theme::kBgPanel, 0.9f);
    pcolors[ImPlotCol_AxisText] = theme::rgb(theme::kTextDim);
    pcolors[ImPlotCol_AxisGrid] = theme::rgb(theme::kGridLine, 0.5f);
    pstyle.PlotPadding = ImVec2(8, 8);
}

}  // namespace

int main() {
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) { std::fprintf(stderr, "glfwInit failed\n"); return 1; }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif

    GLFWwindow* window = glfwCreateWindow(1800, 1400, "QQQ Options Data Terminal", nullptr, nullptr);
    if (!window) { std::fprintf(stderr, "glfwCreateWindow failed\n"); glfwTerminate(); return 1; }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
#ifdef TERMINAL_HAS_IMPLOT3D
    ImPlot3D::CreateContext();
#endif
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    apply_dark_style();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    mockfeed::MockFeed mock_feed(485.0);
    ibkr::IbkrClient ibkr_client(ibkr::ConnectionConfig{.symbol = "QQQ"});
    bool use_ibkr = ibkr_client.connect_and_subscribe().has_value();

    data::MarketState current_state = mock_feed.next_snapshot();
    double time_since_update = 0.0;
    constexpr double kUpdateInterval = 0.25;  // ~4Hz

    render::BarOrientation bar_orientation = render::BarOrientation::Vertical;
    render::SurfaceDteRange surface_range = render::SurfaceDteRange::D90;

    auto last_time = std::chrono::steady_clock::now();

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - last_time).count();
        last_time = now;

        // --- poll data ---
        time_since_update += dt;
        if (time_since_update >= kUpdateInterval) {
            time_since_update = 0.0;
            bool updated = false;
            if (use_ibkr) {
                auto snap = ibkr_client.try_get_latest_snapshot();
                if (snap.has_value()) { current_state = std::move(snap.value()); updated = true; }
                else if (ibkr_client.status() == data::ConnectionStatus::Disconnected) { use_ibkr = false; }
            }
            if (!use_ibkr && !updated) current_state = mock_feed.next_snapshot();
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // --- draw ---
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 16));
        ImGui::Begin("##root", nullptr,
                      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);
        ImGui::PopStyleVar(2);

        // controls row
        ImGui::PushStyleColor(ImGuiCol_Text, theme::rgb(theme::kTextMuted));
        const char* status_str = current_state.status == data::ConnectionStatus::MockData ? "MOCK DATA"
                                : current_state.status == data::ConnectionStatus::Streaming ? "LIVE (IBKR)" : "DISCONNECTED";
        ImGui::Text("%s  |  %s", current_state.underlying.c_str(), status_str);
        ImGui::PopStyleColor();

        ImGui::SameLine(ImGui::GetWindowWidth() - 260);
        int orient_idx = bar_orientation == render::BarOrientation::Vertical ? 0 : 1;
        const char* orient_items[] = {"View: Vertical Barcharts", "View: Horizontal Barcharts"};
        ImGui::SetNextItemWidth(240);
        if (ImGui::Combo("##orient", &orient_idx, orient_items, 2))
            bar_orientation = orient_idx == 0 ? render::BarOrientation::Vertical : render::BarOrientation::Horizontal;

        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 6));

        const float avail_w = ImGui::GetContentRegionAvail().x;
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const float cell_w = (avail_w - spacing * 2) / 3.0f;
        const float cell_h = 250.0f;

        // Row 1
        render::draw_barcharts_row(current_state, bar_orientation, cell_w, cell_h);
        ImGui::SameLine();
        render::draw_summary_panel(current_state, cell_w, cell_h);

        ImGui::Dummy(ImVec2(0, 20));

        // Row 2
        ImGui::PushStyleColor(ImGuiCol_Text, theme::rgb(theme::kTextMuted));
        int range_idx = surface_range == render::SurfaceDteRange::D30 ? 0 : surface_range == render::SurfaceDteRange::D60 ? 1 : 2;
        const char* range_items[] = {"30 Days Expiry View", "60 Days Expiry View", "90 Days Expiry View"};
        ImGui::SameLine(ImGui::GetWindowWidth() - 260);
        ImGui::SetNextItemWidth(240);
        if (ImGui::Combo("##surfrange", &range_idx, range_items, 3))
            surface_range = range_idx == 0 ? render::SurfaceDteRange::D30 : range_idx == 1 ? render::SurfaceDteRange::D60 : render::SurfaceDteRange::D90;
        ImGui::PopStyleColor();

        render::draw_surfaces_row(current_state, surface_range, avail_w, 450.0f);

        ImGui::Dummy(ImVec2(0, 20));

        // Row 3
        render::draw_heatmap_panel(current_state, avail_w);

        ImGui::End();

        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.043f, 0.051f, 0.070f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

#ifdef TERMINAL_HAS_IMPLOT3D
    ImPlot3D::DestroyContext();
#endif
    ImPlot::DestroyContext();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
