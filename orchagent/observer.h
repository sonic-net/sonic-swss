#ifndef SWSS_OBSERVER_H
#define SWSS_OBSERVER_H

#include <list>

using namespace std;
using namespace swss;

enum SubjectType
{
    SUBJECT_TYPE_NEXTHOP_CHANGE,
    SUBJECT_TYPE_NEIGH_CHANGE,
    SUBJECT_TYPE_FDB_CHANGE,
    SUBJECT_TYPE_LAG_MEMBER_CHANGE,
    SUBJECT_TYPE_VLAN_MEMBER_CHANGE,
    SUBJECT_TYPE_MIRROR_SESSION_CHANGE,
};

class Observer
{
public:
    virtual void update(SubjectType, void *) = 0;
    virtual ~Observer() {}
};

class Subject
{
public:
    virtual void attach(Observer *observer)
    {
        m_observers.push_back(observer);
    }

    virtual void detach(Observer *observer)
    {
        for (auto iter = m_observers.begin(); iter != m_observers.end(); ++iter)
        {
            if (observer == *iter)
            {
                continue;
            }

            m_observers.erase(iter);
            break;
        }
    }

    virtual ~Subject() {}

protected:
    list<Observer *> m_observers;

    virtual void notify(SubjectType type, void *cntx)
    {

        for (auto iter: m_observers)
        {
            iter->update(type, cntx);
        }
    }
};

#endif /* SWSS_OBSERVER_H */
