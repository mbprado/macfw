//
//  FW410AudioDriver.cpp
//  FW410AudioDriver
//
//  Created by Murilo Borghi Prado on 17.08.2026.
//

#include <os/log.h>

#include <DriverKit/IOUserServer.h>
#include <DriverKit/IOLib.h>

#include "FW410AudioDriver.h"

kern_return_t
IMPL(FW410AudioDriver, Start)
{
    kern_return_t ret;
    ret = Start(provider, SUPERDISPATCH);
    os_log(OS_LOG_DEFAULT, "Hello World");
    return ret;
}
