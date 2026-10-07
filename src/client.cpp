#include "client.hpp"

#include <QDesktopServices>
#include <QHostAddress>
#include <QNetworkReply>
#include <QOAuthHttpServerReplyHandler>

#include <chrono>

#include <spdlog/spdlog.h>

namespace {
    const QUrl authorize_url("https://auth.frontierstore.net/auth");
    const QUrl token_url("https://auth.frontierstore.net/token");
    const QString api_host = QStringLiteral("https://companion.orerve.net");
    // Docking again at the same station within this long asks the API nothing new.
    constexpr auto docked_cooldown = std::chrono::seconds(90);

    // What the browser shows once Frontier sends it back to us. Qt wraps it in <body>, so the style
    // comes along inside it. It is the same page whether or not the sign-in went through, and the app
    // says which.
    const QString callback_page = QStringLiteral(R"(
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
  html, body { margin: 0; height: 100%; background: #0d0d0d; color: #e6e6e6;
               font-family: 'IBM Plex Mono', Consolas, 'DejaVu Sans Mono', monospace; }
  body { display: flex; align-items: center; justify-content: center; }
  .card { background: #1a1a1a; border: 1px solid #2a2a2a; border-top: 2px solid #f0b040;
          padding: 28px 36px; max-width: 420px; margin: 16px; }
  h1 { margin: 0 0 12px; font-size: 15px; color: #f0b040; letter-spacing: 0.08em;
       text-transform: uppercase; }
  p { margin: 0; font-size: 13px; line-height: 1.6; color: #b0b0b0; }
  .muted { margin-top: 16px; font-size: 11px; color: #6a6a6a; }
</style>
<div class="card">
  <h1>Plum</h1>
  <p>Frontier has handed your sign-in back to Plum.</p>
  <p class="muted">You can close this tab and return to the app.</p>
</div>
)");
} // namespace

client::client(const QString &client_id, const std::shared_ptr<journal::commander_feed> &feed, QObject *parent)
    : QObject(parent), feed_subscriber(this, feed) {
    flow.setNetworkAccessManager(&network);
    flow.setAuthorizationUrl(authorize_url);
    flow.setTokenUrl(token_url);
    flow.setClientIdentifier(client_id); // a public client: PKCE, no secret
    flow.setRequestedScopeTokens({"auth", "capi"});
    // PKCE with S256, which Frontier requires, is Qt's default.

    // Frontier signs in through Frontier, Steam or Epic accounts, and wants to be told all three
    // are acceptable.
    flow.setModifyParametersFunction([](QAbstractOAuth::Stage stage, QMultiMap<QString, QVariant> *parameters) {
        if (stage == QAbstractOAuth::Stage::RequestingAuthorization)
            parameters->insert(QStringLiteral("audience"), QStringLiteral("frontier,steam,epic"));
    });

    // Access tokens last a few hours; renew them before they run out.
    flow.setAutoRefresh(true);

    connect(&flow, &QAbstractOAuth::authorizeWithBrowser, this, [this](const QUrl &url) {
        spdlog::info("[{}] opening Frontier sign-in in the browser", this->feed()->id());
        if (!QDesktopServices::openUrl(url)) spdlog::warn("[{}] cannot open the browser", this->feed()->id());
    });
    connect(&flow, &QAbstractOAuth::granted, this, [this] {
        spdlog::info("[{}] Frontier account {}", this->feed()->id(), renewing ? "renewed" : "signed in");
        renewing = false;
        stop_listening();
        watch();
        emit authorized();
    });
    connect(&flow, &QAbstractOAuth::requestFailed, this, [this](QAbstractOAuth::Error error) {
        // A saved token that no longer works: sign in properly instead.
        if (renewing) {
            spdlog::info("[{}] saved Frontier token rejected (error {}); signing in again", this->feed()->id(),
                         static_cast<int>(error));
            renewing = false;
            sign_in();
            return;
        }
        spdlog::warn("[{}] Frontier sign-in failed (error {})", this->feed()->id(), static_cast<int>(error));
        stop_listening();
        emit failed(QStringLiteral("Frontier sign-in failed (error %1)").arg(static_cast<int>(error)));
    });
    connect(&flow, &QAbstractOAuth2::refreshTokenChanged, this, [this](const QString &token) {
        spdlog::debug("[{}] refresh token {}", this->feed()->id(), token.isEmpty() ? "cleared" : "changed");
        emit refresh_token_changed(token);
    });
}

void client::authorize(const QString &refresh_token) {
    if (refresh_token.isEmpty()) {
        sign_in();
        return;
    }
    spdlog::info("[{}] renewing saved Frontier token", feed()->id());
    renewing = true;
    flow.setRefreshToken(refresh_token);
    flow.refreshTokens();
}

// The browser comes back to a server on localhost, on whatever port the OS hands out, for just
// this sign-in.
void client::sign_in() {
    stop_listening();
    redirect = new QOAuthHttpServerReplyHandler(QHostAddress::LocalHost, 0, this);
    if (!redirect->isListening()) {
        stop_listening();
        spdlog::warn("[{}] cannot listen for the sign-in redirect", feed()->id());
        emit failed(QStringLiteral("cannot listen for Frontier's sign-in redirect"));
        return;
    }
    spdlog::debug("[{}] listening for the sign-in redirect on {}", feed()->id(), redirect->callback().toStdString());
    redirect->setCallbackText(callback_page);
    flow.setReplyHandler(redirect);
    flow.grant();
}

void client::stop_listening() {
    if (!redirect) return;
    redirect->close();
    redirect->deleteLater();
    redirect = nullptr;
}

void client::get(const QString &path, reply_fn done) {
    send(path, std::move(done), false);
}

void client::send(const QString &path, reply_fn done, bool retried) {
    QNetworkRequest request(QUrl(api_host + path));
    request.setRawHeader("Authorization", "Bearer " + flow.token().toUtf8());
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Plum"));

    spdlog::debug("[{}] companion API GET {}{}", feed()->id(), path.toStdString(), retried ? " (retry)" : "");
    auto *reply = network.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, path, done = std::move(done), retried] {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

        // The token has lapsed or been revoked: renew it once and try again.
        if (status == 401 && !retried) {
            spdlog::info("[{}] companion API {}: token rejected; renewing", feed()->id(), path.toStdString());
            auto retry = std::make_shared<QMetaObject::Connection>();
            auto give_up = std::make_shared<QMetaObject::Connection>();
            const auto disconnect_both = [retry, give_up] {
                QObject::disconnect(*retry);
                QObject::disconnect(*give_up);
            };
            *retry = connect(&flow, &QAbstractOAuth::granted, this, [this, path, done, disconnect_both] {
                disconnect_both();
                send(path, done, true);
            });
            *give_up = connect(&flow, &QAbstractOAuth::requestFailed, this, [done, disconnect_both] {
                disconnect_both();
                done(401, journal::json::value_t::discarded);
            });
            flow.refreshTokens();
            return;
        }

        if (reply->error() != QNetworkReply::NoError && status == 0)
            spdlog::warn("companion API {}: {}", path.toStdString(), reply->errorString().toStdString());
        const QByteArray body = reply->readAll();
        spdlog::debug("[{}] companion API {}: status {}, {} bytes", feed()->id(), path.toStdString(), status,
                      body.size());
        done(status, journal::json::parse(body.begin(), body.end(), nullptr, false));
    });
}

// Once signed in: the station's market and shipyard on docking, and the fleet carrier every 15
// minutes while there is one. Tokens renewing later sign in again, so only the first time sets this
// up.
void client::watch() {
    if (watching) return;
    watching = true;
    spdlog::info("[{}] watching for docking and carrier events", feed()->id());

    subscribe({"Docked"}, [this](const journal::game_state &, const journal::game_event &e) {
        const auto market = e.opt_long("MarketID");
        const auto now = std::chrono::steady_clock::now();
        if (market && market == last_docked_market && now - last_docked_at < docked_cooldown) {
            spdlog::debug("[{}] docked at {} again within {}s; companion API not asked", feed()->id(), *market,
                          docked_cooldown.count());
            return;
        }
        last_docked_market = market;
        last_docked_at = now;
        fetch(QStringLiteral("/market"));
        fetch(QStringLiteral("/shipyard"));
    });

    // A commander without a carrier is not polled until the journals show they have one. While
    // polling, CarrierStats starts the 15 minutes over.
    subscribe({"CarrierBuy", "CarrierStats", "CarrierLocation"},
              [this](const journal::game_state &, const journal::game_event &e) {
                  if (!carrier_timer.isActive())
                      fetch_carrier();
                  else if (e.name() == "CarrierStats")
                      carrier_timer.start();
              });

    connect(&carrier_timer, &QTimer::timeout, this, &client::fetch_carrier);
    fetch_carrier();
}

// 204 means no carrier: stop polling. Anything else keeps the timer running.
void client::fetch_carrier() {
    fetch(QStringLiteral("/fleetcarrier"), [this](const int status) {
        if (status == 204) {
            if (carrier_timer.isActive()) spdlog::info("[{}] no fleet carrier; polling stopped", feed()->id());
            carrier_timer.stop();
        } else if (!carrier_timer.isActive()) {
            spdlog::info("[{}] polling the fleet carrier every 15 minutes", feed()->id());
            carrier_timer.start(std::chrono::minutes(15));
        }
    });
}

// Hands a successful response to the feed's live subscribers as an event named for its endpoint:
// "/market" arrives as CAPIMarket, "/fleetcarrier" as CAPIFleetcarrier. Stamped now, since the
// response carries no time of its own. `then`, if given, hears the status either way.
void client::fetch(const QString &path, std::function<void(int status)> then) {
    get(path, [feed = std::weak_ptr(this->feed()), path, then = std::move(then)](const int status, const journal::json &body) {
        if (then) then(status);
        if (status != 200 || !body.is_object()) {
            if (status != 204) spdlog::warn("companion API {}: status {}", path.toStdString(), status);
            return;
        }
        const auto f = feed.lock();
        if (!f) return;

        QString name = path.mid(1);
        name[0] = name[0].toUpper();
        auto payload = body;
        payload["event"] = "CAPI" + name.toStdString();
        payload["timestamp"] =
                journal::format_timestamp(std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
        spdlog::debug("[{}] injecting {}", f->id(), payload["event"].get<std::string>());
        f->inject(journal::game_event(std::move(payload)));
    });
}
