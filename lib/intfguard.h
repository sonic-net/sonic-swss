#ifndef SWSS_INTFGUARD_H
#define SWSS_INTFGUARD_H

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>

#define APP_INTF_GUARD_TABLE_NAME "INTF_GUARD_TABLE"
#define STATE_INTF_GUARD_TABLE_NAME "INTERFACE_GUARD_TABLE"

// Track the current request; the caller validates STATE_DB ownership.
class IntfGuard
{
public:
    enum class State { Held, Retired, Released };
    struct Entry
    {
        std::string id;
        State state;
    };

    bool prepareCurrent(const std::string &alias, const std::string &id)
    {
        if (alias.empty() || id.empty() || id.find_first_not_of("0123456789") != std::string::npos)
        {
            return false;
        }
        uint64_t generation;
        try
        {
            generation = std::stoull(id);
        }
        catch (const std::exception &)
        {
            return false;
        }
        if (!generation)
        {
            return false;
        }
        auto it = m_entries.find(alias);
        if (it != m_entries.end())
        {
            if (generation < std::stoull(it->second.id))
            {
                return false;
            }
            if (it->second.id == id)
            {
                return true;
            }
        }
        // Supersession transfers the fence without reopening admission.
        m_entries[alias] = {id, State::Held};
        return true;
    }

    bool retire(const std::string &alias)
    {
        auto it = m_entries.find(alias);
        if (it == m_entries.end() || it->second.state == State::Released)
        {
            return false;
        }
        it->second.state = State::Retired;
        return true;
    }

    bool release(const std::string &alias, const std::string &id)
    {
        auto it = m_entries.find(alias);
        if (it == m_entries.end() || it->second.id != id || it->second.state == State::Held)
        {
            return false;
        }
        it->second.state = State::Released;
        return true;
    }

    bool cancel(const std::string &alias, const std::string &id)
    {
        auto it = m_entries.find(alias);
        if (it == m_entries.end() || it->second.id != id)
        {
            return false;
        }
        it->second.state = State::Released;
        return true;
    }

    bool isHeld(const std::string &alias) const
    {
        auto it = m_entries.find(alias);
        return it != m_entries.end() && it->second.state != State::Released;
    }

    const Entry *get(const std::string &alias) const
    {
        auto it = m_entries.find(alias);
        return it == m_entries.end() ? nullptr : &it->second;
    }

private:
    std::map<std::string, Entry> m_entries;
};

#endif /* SWSS_INTFGUARD_H */
