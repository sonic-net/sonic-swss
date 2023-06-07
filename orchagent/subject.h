#pragma once

#include <vector>
#include <algorithm>
#include <string>
#include "observer.h"
#include <list>

using namespace std;

class Subject
{
public:
    virtual void attach(Observer *observer)
    {
        m_observers.push_back(observer);
    }

    virtual void detach(Observer *observer)
    {
        m_observers.remove(observer);
    }

    virtual ~Subject() {}

    virtual void notify(SubjectType type, void *cntx)
    {
        for (auto iter: m_observers)
        {
            iter->update(type, cntx);
        }
    }
    
protected:
    list<Observer *> m_observers;

};