#pragma once

#include <QFrame>
#include <QWidget>

#include <journal/game_state.hpp>
#include <journal/history.hpp>

class QLabel;
class QProgressBar;
class QSplitter;
class QVBoxLayout;

// A pane with an amber title bar and a list of key/value rows, styled after trifles.
class Panel : public QFrame {
    Q_OBJECT

public:
    explicit Panel(const QString &title, QWidget *parent = nullptr);

    void setTitle(const QString &title);
    QLabel *addField(const QString &name, const QString &value = QStringLiteral("-"), bool accent = false);
    QProgressBar *addBar(const QString &name, int value = 0);
    void addSection(const QString &text);

private:
    QLabel *m_title;
    QVBoxLayout *m_body;
};

// Base class for every dashboard page. Panels are laid out in resizable columns.
class Dashboard : public QWidget {
    Q_OBJECT

public:
    Dashboard(const QString &title, QWidget *parent = nullptr);

    [[nodiscard]] QString title() const { return m_title; }

    // Called on the UI thread with the latest snapshot, and when history may have changed.
    virtual void showState(const journal::game_state &) {}
    virtual void showHistory(const journal::history &) {}

protected:
    Panel *addPanel(const QString &title, int column);

private:
    QString m_title;
    QSplitter *m_columns;
};
