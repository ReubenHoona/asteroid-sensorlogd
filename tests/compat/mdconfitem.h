// Test stand-in for mlite's MDConfItem (the dconf wrapper that replaces the deprecated MGConfItem): the same
// stand-in class, so that MGConfItem::setTestValue() and clearTestValues() set the keys the code under test reads.
#ifndef COMPAT_MDCONFITEM_H
#define COMPAT_MDCONFITEM_H

#include "mgconfitem.h"

typedef MGConfItem MDConfItem;

#endif
