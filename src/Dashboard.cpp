#include "dashboard.hpp"

#include <QCoreApplication>
#include <QLabel>
#include <QPointer>
#include <QHeaderView>
#include <QProgressBar>
#include <QSplitter>
#include <QTableWidget>
#include <QVBoxLayout>
#include <utility>

namespace {
    QWidget *kv_row(QLabel *key, QWidget *value) {
        auto *row = new QWidget;

        row->setObjectName("field_row");
        auto *layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(key);
        layout->addWidget(value, 1);
        return row;
    }
}

panel::panel(const QString &title, const dashboard *owner, QWidget *parent)
    : QFrame(parent), _title(new QLabel(title)), body(new QVBoxLayout), owner(owner) {
    setObjectName("panel");

    auto *header = new QWidget;
    header->setObjectName("panel_header");
    header->setAttribute(Qt::WA_StyledBackground);
    header->setFixedHeight(20);
    _title->setObjectName("panel_title");
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(6, 0, 4, 0);
    headerLayout->addWidget(_title);

    auto *content = new QWidget;
    content->setObjectName("panel_body");
    content->setAttribute(Qt::WA_StyledBackground);
    content->setLayout(body);

    body->setContentsMargins(8, 6, 8, 8);
    body->setSpacing(4);
    body->addStretch();

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(3, 3, 3, 3);
    layout->setSpacing(0);
    layout->addWidget(header);
    layout->addWidget(content, 1);
}

void panel::set_title(const QString &title) const {
    _title->setText(title);
}

void panel::notify(notification n) const {
    if (owner) owner->notify(std::move(n));
}

QLabel *panel::add_field(const QString &name, const QString &value, const bool accent) const {
    auto *key = new QLabel(name);
    key->setObjectName("field_name");

    auto *val = new QLabel(value);
    val->setObjectName("field_value");
    val->setProperty("accent", accent);
    val->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    val->setTextInteractionFlags(Qt::TextSelectableByMouse);

    // Insert before the trailing stretch so rows stay packed at the top.
    body->insertWidget(body->count() - 1, kv_row(key, val));
    return val;
}

QProgressBar *panel::add_bar(const QString &name, const int value) const {
    auto *key = new QLabel(name);
    key->setObjectName("field_name");

    auto *bar = new QProgressBar;
    bar->setRange(0, 100);
    bar->setValue(value);
    bar->setTextVisible(false);
    bar->setFixedHeight(4);

    body->insertWidget(body->count() - 1, kv_row(key, bar));
    return bar;
}

void panel::add_widget(QWidget *widget) const {
    body->insertWidget(body->count() - 1, widget, 1);
}

QTableWidget *panel::add_table(const QStringList &columns) const {
    auto *table = new QTableWidget(0, static_cast<int>(columns.size()));
    table->setHorizontalHeaderLabels(columns);
    table->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->verticalHeader()->hide();
    table->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    table->verticalHeader()->setDefaultSectionSize(22);
    table->setShowGrid(false);
    table->setFocusPolicy(Qt::NoFocus);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionMode(QAbstractItemView::NoSelection);
    add_widget(table);
    return table;
}

QLabel *panel::add_section(const QString &text) const {
    auto *label = new QLabel(text.toUpper());
    label->setObjectName("section_label");
    body->insertWidget(body->count() - 1, label);
    return label;
}

dashboard::dashboard(QString title, const std::shared_ptr<journal::commander_feed> &feed, overlays &notes,
                     QWidget *parent)
    : QWidget(parent), feed_subscriber(this, feed), _title(std::move(title)), columns(new QSplitter(Qt::Horizontal)),
      notes(notes) {
    columns->setHandleWidth(4);
    columns->setChildrenCollapsible(false);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(0);
    layout->addWidget(columns);
}

panel *dashboard::add_panel(const QString &title, const int colidx) const {
    while (columns->count() <= colidx) {
        auto *col = new QSplitter(Qt::Vertical);
        col->setHandleWidth(4);
        col->setChildrenCollapsible(false);
        columns->addWidget(col);
        columns->setStretchFactor(columns->count() - 1, 1);
    }

    auto *col = dynamic_cast<QSplitter *>(columns->widget(colidx));
    auto *pane = new panel(title, this);
    col->addWidget(pane);
    col->setStretchFactor(col->count() - 1, 1);
    return pane;
}


void dashboard::notify(notification n) const {
    if (n.feed.empty()) n.feed = feed()->id();
    if (n.source.isEmpty()) {
        const auto state = feed()->state();
        n.source = state && state->name ? QString::fromStdString(*state->name) : QString();
    }
    notes.post(std::move(n));
}
