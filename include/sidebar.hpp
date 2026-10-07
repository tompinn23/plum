#pragma once


#include <QButtonGroup>
#include <QWidget>

class QVBoxLayout;


class sidebar : public QWidget {
    Q_OBJECT

public:
    explicit sidebar(QWidget *parent = nullptr);

    int add_entry(const QString &label);

    void set_current(int index) const;

    signals:




    void current_changed(int index);

private:
    QVBoxLayout *layout;
    QButtonGroup group;
};