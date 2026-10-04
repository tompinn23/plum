#pragma once

#include <QButtonGroup>
#include <QWidget>

class QVBoxLayout;

// Narrow text navigation bar. Each entry maps to an index in the dashboard stack.
class Sidebar : public QWidget {
    Q_OBJECT

public:
    explicit Sidebar(QWidget *parent = nullptr);

    int addEntry(const QString &label);
    void setCurrentIndex(int index);

signals:
    void currentIndexChanged(int index);

private:
    QVBoxLayout *m_layout;
    QButtonGroup m_group;
};
