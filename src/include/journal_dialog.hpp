#pragma once

#include <vector>

#include <QDialog>

#include "journal_sources.hpp"

class QPushButton;
class QTableWidget;

// Edits the list of watched journal directories. Each row is a name and a directory.
class journal_dialog : public QDialog {
    Q_OBJECT

public:
    explicit journal_dialog(const std::vector<journal_source> &sources, QWidget *parent = nullptr);

    [[nodiscard]] std::vector<journal_source> sources() const;

    void accept() override;

    // Shows a row's account as linked, or offers to link it.
    void set_linked(const QString &id, bool linked) const;

    signals:
    // The user asked to link the Frontier account behind a row's journals, for the Companion API.
    // The dialog stays open.




    void link_requested(const journal_source &source);

private:
    void add_row(const journal_source &source);

    [[nodiscard]] int row_of(const QString &id) const;

    void add_directory();

    void browse_selected();

    void remove_selected() const;

    void update_buttons() const;

    void refresh_row(int row) const;

    QTableWidget *m_table;
    QPushButton *m_browse;
    QPushButton *m_remove;
};
