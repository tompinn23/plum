#include "Dashboard.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QSplitter>
#include <QVBoxLayout>

namespace {

QWidget *keyValueRow(QLabel *key, QWidget *value) {
    auto *row = new QWidget;
    row->setObjectName("fieldRow");
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(key);
    layout->addWidget(value, 1);
    return row;
}

} // namespace

Panel::Panel(const QString &title, QWidget *parent)
    : QFrame(parent), m_title(new QLabel(title)), m_body(new QVBoxLayout) {
    setObjectName("panel");

    auto *header = new QWidget;
    header->setObjectName("panelHeader");
    header->setAttribute(Qt::WA_StyledBackground);
    header->setFixedHeight(20);
    m_title->setObjectName("panelTitle");
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(6, 0, 4, 0);
    headerLayout->addWidget(m_title);

    auto *body = new QWidget;
    body->setObjectName("panelBody");
    body->setAttribute(Qt::WA_StyledBackground);
    body->setLayout(m_body);
    m_body->setContentsMargins(8, 6, 8, 8);
    m_body->setSpacing(4);
    m_body->addStretch();

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(3, 3, 3, 3);
    layout->setSpacing(0);
    layout->addWidget(header);
    layout->addWidget(body, 1);
}

void Panel::setTitle(const QString &title) {
    m_title->setText(title);
}

QLabel *Panel::addField(const QString &name, const QString &value, bool accent) {
    auto *key = new QLabel(name);
    key->setObjectName("fieldName");

    auto *val = new QLabel(value);
    val->setObjectName("fieldValue");
    val->setProperty("accent", accent);
    val->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    val->setTextInteractionFlags(Qt::TextSelectableByMouse);

    // Insert before the trailing stretch so rows stay packed at the top.
    m_body->insertWidget(m_body->count() - 1, keyValueRow(key, val));
    return val;
}

QProgressBar *Panel::addBar(const QString &name, int value) {
    auto *key = new QLabel(name);
    key->setObjectName("fieldName");

    auto *bar = new QProgressBar;
    bar->setRange(0, 100);
    bar->setValue(value);
    bar->setTextVisible(false);
    bar->setFixedHeight(4);

    m_body->insertWidget(m_body->count() - 1, keyValueRow(key, bar));
    return bar;
}

void Panel::addSection(const QString &text) {
    auto *label = new QLabel(text.toUpper());
    label->setObjectName("sectionLabel");
    m_body->insertWidget(m_body->count() - 1, label);
}

Dashboard::Dashboard(const QString &title, QWidget *parent)
    : QWidget(parent), m_title(title), m_columns(new QSplitter(Qt::Horizontal)) {
    m_columns->setHandleWidth(4);
    m_columns->setChildrenCollapsible(false);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(0);
    layout->addWidget(m_columns);
}

Panel *Dashboard::addPanel(const QString &title, int column) {
    while (m_columns->count() <= column) {
        auto *col = new QSplitter(Qt::Vertical);
        col->setHandleWidth(4);
        col->setChildrenCollapsible(false);
        m_columns->addWidget(col);
        m_columns->setStretchFactor(m_columns->count() - 1, 1);
    }

    auto *col = static_cast<QSplitter *>(m_columns->widget(column));
    auto *panel = new Panel(title);
    col->addWidget(panel);
    col->setStretchFactor(col->count() - 1, 1);
    return panel;
}
