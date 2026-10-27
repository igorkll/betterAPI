#pragma once

#include <dsbox_scheduler/executor.hpp>
#include <dsbox_scheduler/types.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dsbox_scheduler
{
    using TimerId = std::uint64_t;

    struct TimerEntry
    {
        TimerId id = 0; // assigned by TimerQueue::add()
        TimePoint deadline;
        Callback task;

        // Optional. Invoked instead of `task` when the scheduler discards
        // the entry without running it (scheduler stopped, executor
        // rejected the task). NOT invoked when the entry is cancelled
        // through its handle -- the canceller already knows.
        Callback onDrop;

        std::shared_ptr<IExecutor> executor;
    };

    // Pure ordered collection of pending timers. No threads, no locking,
    // no clock: the caller supplies "now" and does its own
    // synchronization (Scheduler guards it with its mutex). That is what
    // makes it trivial to test exhaustively.
    //
    // Ordering: by deadline, then by insertion order (FIFO) for equal
    // deadlines. Cancellation removes the entry immediately, O(log n).
    class TimerQueue
    {
    public:
        // Inserts `entry`, assigning and returning a fresh id.
        TimerId add(TimerEntry entry)
        {
            entry.id = m_nextId++;
            const TimerId id = entry.id;
            insert(std::move(entry));
            return id;
        }

        // Removes a pending entry. False if it was not pending (already
        // popped, cancelled, or never existed).
        bool cancel(TimerId id)
        {
            const auto it = m_index.find(id);
            if (it == m_index.end())
                return false;

            m_entries.erase(it->second);
            m_index.erase(it);
            return true;
        }

        bool empty() const noexcept
        {
            return m_entries.empty();
        }

        std::size_t size() const noexcept
        {
            return m_entries.size();
        }

        std::optional<TimePoint> nextDeadline() const
        {
            if (m_entries.empty())
                return std::nullopt;

            return m_entries.begin()->first.first;
        }

        // Removes and returns every entry with deadline <= now, in
        // firing order.
        std::vector<TimerEntry> popDue(TimePoint now)
        {
            std::vector<TimerEntry> due;

            while (!m_entries.empty() && m_entries.begin()->first.first <= now)
            {
                const auto it = m_entries.begin();
                m_index.erase(it->second.id);
                due.push_back(std::move(it->second));
                m_entries.erase(it);
            }

            return due;
        }

        // Removes and returns everything, in firing order.
        std::vector<TimerEntry> drain()
        {
            std::vector<TimerEntry> all;
            all.reserve(m_entries.size());

            for (auto& [key, entry] : m_entries)
                all.push_back(std::move(entry));

            m_entries.clear();
            m_index.clear();
            return all;
        }

    private:
        using Key = std::pair<TimePoint, std::uint64_t>; // (deadline, sequence)

        void insert(TimerEntry&& entry)
        {
            const Key key{ entry.deadline, m_nextSeq++ };
            m_index[entry.id] = key;
            m_entries.emplace(key, std::move(entry));
        }

        std::map<Key, TimerEntry> m_entries;
        std::unordered_map<TimerId, Key> m_index;
        std::uint64_t m_nextSeq = 1;
        TimerId m_nextId = 1;
    };
}
