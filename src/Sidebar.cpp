#include "Sidebar.h"

#include <QPushButton>
#include <QVBoxLayout>

Sidebar::Sidebar(QWidget *parent) : QWidget(parent), m_layout(new QVBoxLayout(this)) {
    setObjectName("sidebar");
    setAttribute(Qt::WA_StyledBackground);
    setFixedWidth(110);

    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(0);
    m_layout->addStretch();

    m_group.setExclusive(true);
    connect(&m_group, &QButtonGroup::idClicked, this, &Sidebar::currentIndexChanged);
}

int Sidebar::addEntry(const QString &label) {
    const int index = static_cast<int>(m_group.buttons().size());

    auto *button = new QPushButton(label.toUpper(), this);
    button->setObjectName("navButton");
    button->setCheckable(true);
    button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    if (index < 9)
        button->setShortcut(QKeySequence(Qt::CTRL | static_cast<Qt::Key>(Qt::Key_1 + index)));

    m_group.addButton(button, index);
    // Insert before the trailing stretch so buttons stack from the top.
    m_layout->insertWidget(m_layout->count() - 1, button);
    return index;
}

void Sidebar::setCurrentIndex(int index) {
    if (auto *button = m_group.button(index))
        button->setChecked(true);
}
