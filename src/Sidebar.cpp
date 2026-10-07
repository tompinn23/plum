#include "include/sidebar.hpp"

#include <QPushButton>
#include <QVBoxLayout>

sidebar::sidebar(QWidget *parent) : QWidget(parent), layout(new QVBoxLayout(this)) {
    setObjectName("sidebar");
    setAttribute(Qt::WA_StyledBackground);
    setFixedWidth(110);

    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addStretch();

    group.setExclusive(true);
    connect(&group, &QButtonGroup::idClicked, this, &sidebar::current_changed);
}

int sidebar::add_entry(const QString &label) {
    const int index = static_cast<int>(group.buttons().size());

    auto *button = new QPushButton(label.toUpper(), this);
    button->setObjectName("nav_button");
    button->setCheckable(true);
    button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    if (index < 9)
        button->setShortcut(QKeySequence(Qt::CTRL | static_cast<Qt::Key>(Qt::Key_1 + index)));

    group.addButton(button, index);
    // Insert before the trailing stretch so buttons stack from the top.
    layout->insertWidget(layout->count() - 1, button);
    return index;
}

void sidebar::set_current(const int index) const {
    if (auto *button = group.button(index))
        button->setChecked(true);
}
