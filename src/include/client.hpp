#pragma once

#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <QNetworkAccessManager>
#include <QOAuth2AuthorizationCodeFlow>
#include <QObject>
#include <QString>
#include <QTimer>

#include <journal/game_event.hpp>
#include <journal/journal_service.hpp>

class QOAuthHttpServerReplyHandler;

// Frontier's Companion API: signing in to an account (OAuth 2 with PKCE, through the browser) and
// requests against it.
class client : public QObject {
    Q_OBJECT

public:
    client(const QString &client_id, const std::shared_ptr<journal::commander_feed> &feed, QObject *parent = nullptr);

    // With a saved refresh token, renews it without the browser; without one, or if it no longer
    // works, opens Frontier's sign-in page. Emits authorized() or failed().
    void authorize(const QString &refresh_token = {});

    [[nodiscard]] bool is_authorized() const { return !flow.token().isEmpty(); }
    [[nodiscard]] QString refresh_token() const { return flow.refreshToken(); }

    // GETs an endpoint such as "/profile", "/market", "/shipyard" or "/fleetcarrier". `done` gets
    // the HTTP status and the parsed body: discarded JSON if there was none or it did not parse. A
    // rejected token is renewed and the request tried once more before `done` hears of it.
    using reply_fn = std::function<void(int status, const journal::json & body)>;

    void get(const QString &path, reply_fn done);

    // Live events from this client's feed, handed to `handler` on the UI thread with the state
    // as it stood after them. Lasts as long as the client.
    void subscribe(const std::set<std::string> &events,
                   std::function<void(const journal::game_state &, const journal::game_event &)> handler);

    signals:


    void authorized();

    void failed(const QString &why);

    void refresh_token_changed(const QString &token); // for whoever keeps it between runs

private:
    void sign_in();

    void send(const QString &path, reply_fn done, bool retried);

    void stop_listening();

    void watch();

    void fetch_carrier();

    void fetch(const QString &path, std::function<void(int status)> then = {});

    QNetworkAccessManager network;
    QOAuth2AuthorizationCodeFlow flow;
    QOAuthHttpServerReplyHandler *redirect = nullptr; // only while signing in through the browser
    bool renewing = false; // a refresh that falls back to the browser

    std::shared_ptr<journal::commander_feed> data;
    std::vector<journal::subscription> subs;
    QTimer carrier_timer; // /fleetcarrier, polled while the commander has a carrier
    bool watching = false;
};
