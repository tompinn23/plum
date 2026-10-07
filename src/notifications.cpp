#include "include/notifications.hpp"

namespace {
    constexpr std::size_t keep = 200;
    constexpr qint64 merge_window_seconds = 10;
} // namespace

void notifications::post(notification n) {
    n.at = QDateTime::currentDateTimeUtc();

    // A burst of the same thing (a bounty spree, say) becomes one entry with a count.
    if (!n.key.isEmpty() && !m_recent.empty()) {
        auto &last = m_recent.front();
        if (last.key == n.key && last.source == n.source && last.at.secsTo(n.at) < merge_window_seconds) {
            ++last.count;
            last.at = n.at;
            last.text = n.text;
            emit merged(last);
            return;
        }
    }

    m_recent.push_front(std::move(n));
    if (m_recent.size() > keep) m_recent.pop_back();
    emit posted(m_recent.front());
}
