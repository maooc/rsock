//
// Created by System Administrator on 6/9/18.
//

#include "NetService.h"
#include "ServiceUtil.h"
#include "INetObserver.h"

void NetService::NotifyTcpFinOrRst(const TcpInfo &info) {
    if (!Blocked()) {
        auto fn = [&](INetObserver *o) {
            o->OnTcpFinOrRst(info);
        };
        ServiceUtil::ForEach<INetObserver>(this, fn);
    }
}

