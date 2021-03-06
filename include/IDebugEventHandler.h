/*
Copyright (C) 2006 - 2013 Evan Teran
                          eteran@alum.rit.edu

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef IDEBUG_EVENT_HANDLER_20061101_H_
#define IDEBUG_EVENT_HANDLER_20061101_H_

#include "Types.h"
#include "IDebugEvent.h"

class IDebugEventHandler {
public:
	virtual ~IDebugEventHandler() {}

public:
	virtual edb::EVENT_STATUS handle_event(const IDebugEvent::const_pointer &event) = 0;
};

#endif
