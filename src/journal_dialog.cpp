#include "journal_dialog.hpp"

#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QStyle>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {
    enum column { directory_column, commander_column, status_column, link_column };

    // The source id rides along on the commander cell, so rows can be reordered freely.
    constexpr int id_role = Qt::UserRole;
    constexpr int row_height = 32;

    const QColor text_colour(0xd0, 0xd0, 0xd0);
    const QColor muted_colour(0x6a, 0x6a, 0x6a);
    const QColor linked_colour(0x44, 0xcc, 0x88);
    const QColor missing_colour(0xe0, 0x60, 0x50);

    QString cell_text(const QTableWidget *table, const int row, const int column) {
        const auto *item = table->item(row, column);
        return item ? item->text().trimmed() : QString();
    }

    // Cell widgets would otherwise take the theme's opaque widget background.
    QWidget *centred(QWidget *widget) {
        auto *host = new QWidget;
        host->setObjectName("cell_host");
        auto *layout = new QHBoxLayout(host);
        layout->setContentsMargins(4, 2, 4, 2);
        layout->setAlignment(Qt::AlignCenter);
        layout->addWidget(widget);
        return host;
    }
} // namespace

journal_dialog::journal_dialog(const std::vector<journal_source> &sources, QWidget *parent)
    : QDialog(parent), table(new QTableWidget(0, 4)), browse(new QPushButton("Browse...")),
      remove(new QPushButton("Remove")) {
    setWindowTitle("Configuration - Journal directories");
    setMinimumSize(820, 400);
    resize(900, 460);

    auto *heading = new QLabel("Journal directories");
    heading->setObjectName("dialog_heading");

    table->setObjectName("journal_table");
    table->setHorizontalHeaderLabels({"Directory", "CMDR", "CAPI", ""});
    table->horizontalHeaderItem(commander_column)->
            setToolTip("The commander named in the directory's newest journal");
    table->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    table->horizontalHeader()->setSectionResizeMode(directory_column, QHeaderView::Stretch);
    table->setColumnWidth(commander_column, 160);
    table->setColumnWidth(status_column, 100);
    table->setColumnWidth(link_column, 110);
    table->verticalHeader()->hide();
    table->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    table->verticalHeader()->setDefaultSectionSize(row_height);
    table->setShowGrid(false);
    table->setFocusPolicy(Qt::NoFocus);
    // Directories change through Add and Browse; the commander comes from the journals.
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    for (const auto &source: sources) add_row(source);

    auto *add = new QPushButton(QStringLiteral(u"＋  Add directory..."));
    add->setObjectName("add_button");
    auto *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    for (auto *button: {add, browse, remove}) button->setCursor(Qt::PointingHandCursor);
    for (auto *button: box->buttons()) button->setCursor(Qt::PointingHandCursor);

    auto *bar = new QHBoxLayout;
    bar->setSpacing(8);
    bar->addWidget(add);
    bar->addWidget(browse);
    bar->addWidget(remove);
    bar->addStretch();
    bar->addWidget(box);

    auto *layout = new QVBoxLayout(this);
    layout->setSpacing(12);
    layout->addWidget(heading);
    layout->addWidget(table, 1);
    layout->addLayout(bar);

    connect(add, &QPushButton::clicked, this, &journal_dialog::add_directory);
    connect(browse, &QPushButton::clicked, this, &journal_dialog::browse_selected);
    connect(remove, &QPushButton::clicked, this, &journal_dialog::remove_selected);
    connect(table, &QTableWidget::itemSelectionChanged, this, &journal_dialog::update_buttons);
    connect(table, &QTableWidget::itemChanged, this, [this](const QTableWidgetItem *item) {
        if (item->column() == directory_column) refresh_row(item->row());
    });
    connect(box, &QDialogButtonBox::accepted, this, &journal_dialog::accept);
    connect(box, &QDialogButtonBox::rejected, this, &journal_dialog::reject);

    update_buttons();
}

std::vector<journal_source> journal_dialog::sources() const {
    std::vector<journal_source> result;
    for (int row = 0; row < table->rowCount(); ++row) {
        result.push_back({
            .id = table->item(row, commander_column)->data(id_role).toString(),
            .directory = QDir::fromNativeSeparators(cell_text(table, row, directory_column))
        });
    }
    return result;
}

void journal_dialog::accept() {
    QStringList missing;
    for (int row = 0; row < table->rowCount(); ++row) {
        const QString directory = cell_text(table, row, directory_column);
        if (directory.isEmpty()) {
            table->selectRow(row);
            QMessageBox::warning(this, windowTitle(), "Every entry needs a directory.");
            return;
        }
        if (!QFileInfo(directory).isDir()) missing << directory;
    }

    if (!missing.isEmpty()) {
        const auto answer = QMessageBox::question(
            this, windowTitle(),
            QStringLiteral("These directories do not exist and will not be watched:\n\n%1\n\nSave anyway?")
            .arg(missing.join('\n')));
        if (answer != QMessageBox::Yes) return;
    }
    QDialog::accept();
}

void journal_dialog::add_row(const journal_source &source) {
    const QSignalBlocker blocker(table);
    const int row = table->rowCount();
    table->insertRow(row);

    table->setItem(row, directory_column, new QTableWidgetItem(QDir::toNativeSeparators(source.directory)));
    auto *commander = new QTableWidgetItem;
    commander->setData(id_role, source.id);
    table->setItem(row, commander_column, commander);
    table->setItem(row, status_column, new QTableWidgetItem);

    // Looked up by id when clicked: rows above it may have been removed since, and the directory
    // edited.
    auto *link = new QPushButton;
    link->setObjectName("link_button");
    link->setFixedHeight(22);
    link->setCursor(Qt::PointingHandCursor);
    link->setToolTip("Sign in to this commander's Frontier account in your browser, for the Companion API");
    connect(link, &QPushButton::clicked, this, [this, id = source.id] {
        if (const int at = row_of(id); at >= 0)
            emit link_requested({
                .id = id, .directory = QDir::fromNativeSeparators(cell_text(table, at, directory_column))
            });
    });
    table->setCellWidget(row, link_column, centred(link));

    set_linked(source.id, false);
    refresh_row(row);
}

void journal_dialog::set_linked(const QString &id, const bool linked) const {
    const int row = row_of(id);
    if (row < 0) return;

    const QSignalBlocker blocker(table);
    auto *status = table->item(row, status_column);
    status->setText(linked ? QStringLiteral("Linked") : QStringLiteral("Not linked"));
    status->setForeground(linked ? linked_colour : muted_colour);

    if (auto *link = table->cellWidget(row, link_column)->findChild<QPushButton *>()) {
        link->setText(linked ? QStringLiteral("Re-link") : QStringLiteral("Link CAPI"));
        // The theme styles a linked account's button quietly; restyle it now the property changed.
        link->setProperty("linked", linked);
        link->style()->unpolish(link);
        link->style()->polish(link);
    }
}

int journal_dialog::row_of(const QString &id) const {
    for (int row = 0; row < table->rowCount(); ++row) {
        if (const auto *item = table->item(row, commander_column); item && item->data(id_role).toString() == id)
            return row;
    }
    return -1;
}

void journal_dialog::add_directory() {
    const QString directory =
            QFileDialog::getExistingDirectory(this, "Add journal directory", default_journal_directory());
    if (directory.isEmpty()) return;

    add_row({.id = new_journal_source_id(), .directory = directory});
    table->selectRow(table->rowCount() - 1);
}

void journal_dialog::browse_selected() {
    const int row = table->currentRow();
    if (row < 0) return;

    const QString directory =
            QFileDialog::getExistingDirectory(this, "Choose journal directory",
                                              cell_text(table, row, directory_column));
    if (!directory.isEmpty()) table->item(row, directory_column)->setText(QDir::toNativeSeparators(directory));
}

void journal_dialog::remove_selected() const {
    if (const int row = table->currentRow(); row >= 0) table->removeRow(row);
}

void journal_dialog::update_buttons() const {
    const bool selected = !table->selectedItems().isEmpty();
    browse->setEnabled(selected);
    remove->setEnabled(selected);
}

// Re-reads who the directory's journals name, and flags a directory that does not exist.
void journal_dialog::refresh_row(const int row) const {
    auto *directory = table->item(row, directory_column);
    auto *commander = table->item(row, commander_column);
    if (!directory || !commander) return;

    const QSignalBlocker blocker(table);
    const QString path = directory->text().trimmed();
    const bool exists = QFileInfo(path).isDir();
    directory->setForeground(exists ? text_colour : missing_colour);
    directory->setToolTip(exists ? path : QStringLiteral("Directory not found"));

    const QString name = exists ? journal_commander(path) : QStringLiteral("<unknown>");
    const bool known = name != QStringLiteral("<unknown>");
    commander->setText(known ? name : QStringLiteral("unknown"));
    commander->setForeground(known ? text_colour : muted_colour);
}
