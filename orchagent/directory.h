#pragma once

// The Directory class below allows us to store objects inside.
// The objects are supposed to be derived from a common base class B.
// All set/get operations are addressed by the type of the storing/stored object.

#include <typeinfo>
#include <string>
#include <stdexcept>
#include <unordered_map>
#include <algorithm>

template <typename B>
class Directory
{
public:

    template <typename U>
    void set(const U& value)
    {
        std::string type_name { typeid(U).name() };

        if (m_values.find(type_name) != m_values.end())
        {
            throw std::logic_error(std::string("Type ") + type_name + " already registered");
        }

        m_values[type_name] = value;
    }

    template <typename U>
    const U get() const
    {
        std::string type_name { typeid(U).name() };

        if (m_values.find(type_name) == m_values.end())
        {
            return nullptr;
        }

        return static_cast<U>(m_values.at(type_name));
    }

    class iterator : public std::iterator<std::input_iterator_tag, B>
    {
    public:
        explicit iterator(const typename std::unordered_map<std::string, B>::iterator& it) : it(it) {}

        B& operator*() const
        {
            return it->second;
        }

        bool operator!=(iterator other) const
        {
            return it != other.it;
        }

        iterator& operator++()
        {
            ++it;
            return *this;
        }

    private:
        typename std::unordered_map<std::string, B>::iterator it;
    };

    iterator begin()
    {
        return iterator(m_values.begin());
    }

    iterator end()
    {
        return iterator(m_values.end());
    }

private:
    std::unordered_map<std::string, B> m_values;
};
