#pragma once
// Included after the gateway's policy/cache helpers. The UDP worker never waits
// for upstream TCP: clients use the separate bounded TCP worker after a TC reply.

static int dashDnsNonblockingSocket(int type)
{
    int fd = lwip_socket(AF_INET, type, 0);
    if (fd >= 0 && lwip_fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
        lwip_close(fd); fd = -1;
    }
    return fd;
}

static int dashDnsServerSocket(int type)
{
    int fd = dashDnsNonblockingSocket(type);
    if (fd < 0) return -1;
    sockaddr_in addr = {};
    addr.sin_family = AF_INET; addr.sin_port = htons(53);
    addr.sin_addr.s_addr = static_cast<uint32_t>(WiFi.softAPIP());
    if (!addr.sin_addr.s_addr || lwip_bind(fd,reinterpret_cast<sockaddr *>(&addr),sizeof(addr)) ||
        (type == SOCK_STREAM && lwip_listen(fd,2))) {
        lwip_close(fd); return -1;
    }
    return fd;
}

static bool dashDnsApClient(const sockaddr_in &client)
{
    return (ntohl(client.sin_addr.s_addr) & 0xffffff00u) == 0x64640100u;
}

static size_t dashDnsLocalReply(const uint8_t *query, size_t n, const dns_wire::Question &q,
                               bool cacheable, uint8_t *out)
{
    if (q.type == kDashGatewayDnsTypeAAAA) {
        ++gatewayDnsLocal;
        return dns_wire::reply(query,n,out,dns_wire::MaxMessage,0);
    }
    if (std::strcmp(q.name,"t.sl") == 0) {
        ++gatewayDnsLocal;
        return q.type == kDashGatewayDnsTypeA
            ? dashGatewayMakeDnsFakeReply(query,n,out,dns_wire::MaxMessage,inet_addr("100.100.1.1"))
            : dns_wire::reply(query,n,out,dns_wire::MaxMessage,0);
    }
    if (gatewayEnabled && !dashGatewayDnsAllowed(q.name)) {
        ++gatewayDnsLocal;
        dashGatewayTrackBlocked(q.name);
        return dashGatewayMakeDnsBlockedReply(query,n,out,dns_wire::MaxMessage);
    }
    size_t size = cacheable ? dashGatewayDnsCacheLookup(q.name,q.type,millis()/1000,out,dns_wire::MaxMessage) : 0;
    if (size) {
        out[0]=query[0]; out[1]=query[1];
        // Preserve this client's question case (including DNS 0x20 checks).
        std::memcpy(out+12,query+12,q.end-12);
    }
    return size;
}

static bool dashDnsValidResponse(uint8_t *p, size_t n, uint16_t id,
                                 const char *name, uint16_t type)
{
    dns_wire::Question q;
    if (n < 12 || !(p[2]&0x80) || dns_wire::u16(p) != id ||
        !dns_wire::question(p,n,q) || q.type != type || std::strcmp(q.name,name)) return false;
    if (p[2]&2) return true; // TC packets may end inside an RR; never cache them.
    uint32_t ttl = 0;
    return dns_wire::records(p,n,q.end,0,ttl);
}

static void dashDnsSendClients(DashGatewayPendingQuery &q, uint8_t *response, size_t n)
{
    if (n > q.udpCap || (response[2]&2))
        n = dns_wire::reply(q.query,q.queryLen,response,dns_wire::MaxMessage,0,true);
    for (uint8_t i=0; i<q.clientCount && n; ++i) {
        dns_wire::put16(response,q.clientIds[i]);
        if (lwip_sendto(gatewayDnsSock,response,n,0,reinterpret_cast<sockaddr *>(&q.clients[i]),sizeof(sockaddr_in)) < 0)
            ++gatewayDnsSocketErrors;
    }
    q.inUse = false;
}

static void dashDnsFailPending(DashGatewayPendingQuery &q, uint8_t *out)
{
    size_t n = dns_wire::reply(q.query,q.queryLen,out,dns_wire::MaxMessage,2);
    dashDnsSendClients(q,out,n);
}

static void dashDnsReceiveClient(uint8_t *rx, uint8_t *tx)
{
    sockaddr_in client = {}; socklen_t size = sizeof(client);
    int n = lwip_recvfrom(gatewayDnsSock,rx,dns_wire::MaxMessage+1,0,reinterpret_cast<sockaddr *>(&client),&size);
    if (n < 0) return;
    ++gatewayDnsRx;
    dns_wire::Question parsed; uint16_t cap = 512; bool cacheable = false;
    if (!dashDnsApClient(client) || n > int(dns_wire::MaxQuery) ||
        !dns_wire::query(rx,n,parsed,cap,cacheable)) { ++gatewayDnsInvalid; return; }
    size_t local = dashDnsLocalReply(rx,n,parsed,cacheable,tx);
    if (local) {
        if (local > cap) local = dns_wire::reply(rx,n,tx,dns_wire::MaxMessage,0,true);
        if (lwip_sendto(gatewayDnsSock,tx,local,0,reinterpret_cast<sockaddr *>(&client),size) < 0)
            ++gatewayDnsSocketErrors;
        return;
    }
    const uint16_t originalId = dns_wire::u16(rx);
    DashGatewayPendingQuery *slot = nullptr;
    DASH_GATEWAY_FOR_PENDING(q) {
        if (!q.inUse) { if (!slot) slot=&q; continue; }
        // Byte-identical question/flags/EDNS only, preserving each client's ID.
        if (q.rulesVersion == gatewayDnsRulesVersion && q.queryLen == n &&
            std::memcmp(q.query+2,rx+2,n-2) == 0 && q.clientCount < kDashGatewayMaxPendingClients) {
            dashGatewayPendingAddClient(q,originalId,client); return;
        }
    }
    if (!slot || gatewayUpstreamDns == IPADDR_NONE || !gatewayUpstreamDns) {
        if (!slot) ++gatewayDnsPendingFull;
        size_t fail = dns_wire::reply(rx,n,tx,dns_wire::MaxMessage,2);
        lwip_sendto(gatewayDnsSock,tx,fail,0,reinterpret_cast<sockaddr *>(&client),size);
        return;
    }
    uint16_t id;
    bool collision;
    do {
        id = ++gatewayNextProxyId; collision = false;
        DASH_GATEWAY_FOR_PENDING(q) if (q.inUse && q.proxyId == id) collision=true;
    } while (collision);
    dashGatewayInitPending(*slot,originalId,id,parsed.type,&client,parsed.name);
    slot->upstream = gatewayUpstreamDns; slot->queryLen = n;
    slot->udpCap = cap; slot->cacheable = cacheable;
    std::memcpy(slot->query,rx,n); dns_wire::put16(slot->query,id);
    dashGatewayUpdatePendingMax();
    sockaddr_in dst = {}; dst.sin_family=AF_INET; dst.sin_port=htons(53); dst.sin_addr.s_addr=slot->upstream;
    if (lwip_sendto(gatewayUpstreamSock,slot->query,n,0,reinterpret_cast<sockaddr *>(&dst),sizeof(dst)) != n) {
        ++gatewayDnsUpstreamFails; dashDnsFailPending(*slot,tx);
    }
}

static void dashDnsReceiveUpstream(uint8_t *rx, uint8_t *tx)
{
    sockaddr_in from = {}; socklen_t size=sizeof(from);
    int n = lwip_recvfrom(gatewayUpstreamSock,rx,dns_wire::MaxMessage+1,0,reinterpret_cast<sockaddr *>(&from),&size);
    if (n < 12) return;
    DASH_GATEWAY_FOR_PENDING(q) {
        if (!q.inUse || q.proxyId != dns_wire::u16(rx) || q.upstream != from.sin_addr.s_addr || from.sin_port != htons(53)) continue;
        if (q.rulesVersion != gatewayDnsRulesVersion) { q.inUse=false; return; }
        if (n > int(dns_wire::MaxMessage)) {
            size_t len = dns_wire::reply(q.query,q.queryLen,tx,dns_wire::MaxMessage,0,true);
            dashDnsSendClients(q,tx,len); return;
        }
        if (!dashDnsValidResponse(rx,n,q.proxyId,q.domain,q.qtype)) { ++gatewayDnsInvalid; return; }
        dashGatewayTrackLatency(q,xTaskGetTickCount());
        if (q.cacheable) dashGatewayDnsCachePut(q.domain,q.qtype,millis()/1000,rx,n);
        dashDnsSendClients(q,rx,n); return;
    }
}

static void dashGatewayDnsTask(void *)
{
    uint8_t rx[dns_wire::MaxMessage+1], tx[dns_wire::MaxMessage];
    gatewayNextProxyId = static_cast<uint16_t>(esp_random());
    for (;;) {
        if (gatewayDnsSock < 0 || gatewayUpstreamSock < 0) {
            int serverFd = dashDnsServerSocket(SOCK_DGRAM);
            int upstreamFd = dashDnsNonblockingSocket(SOCK_DGRAM);
            if (serverFd < 0 || upstreamFd < 0) {
                if (serverFd >= 0) lwip_close(serverFd);
                if (upstreamFd >= 0) lwip_close(upstreamFd);
                { DashGatewayGuard guard; ++gatewayDnsSocketErrors; }
                vTaskDelay(pdMS_TO_TICKS(1000)); continue;
            }
            DashGatewayGuard guard;
            gatewayDnsSock=serverFd; gatewayUpstreamSock=upstreamFd; gatewayDnsBindOk=true;
        }
        gatewayDnsHeartbeatMs = millis();
        // Use lwIP's poll directly: no VFS fd_set layout/offset dependency.
        pollfd ready[2] = {{gatewayDnsSock,POLLIN,0},{gatewayUpstreamSock,POLLIN,0}};
        int result = lwip_poll(ready,2,20);
        DashGatewayGuard guard;
        if (result < 0 || ((ready[0].revents|ready[1].revents)&(POLLERR|POLLNVAL|POLLHUP))) {
            ++gatewayDnsSocketErrors;
            DASH_GATEWAY_FOR_PENDING(q) if (q.inUse) dashDnsFailPending(q,tx);
            lwip_close(gatewayDnsSock); lwip_close(gatewayUpstreamSock);
            gatewayDnsSock=gatewayUpstreamSock=-1; gatewayDnsBindOk=false;
            continue;
        }
        // Drain bounded batches; always return to the scheduler between batches.
        for (int i=0; i<8; ++i) {
            if (ready[1].revents&POLLIN) dashDnsReceiveUpstream(rx,tx);
            if (ready[0].revents&POLLIN) dashDnsReceiveClient(rx,tx);
        }
        const TickType_t now=xTaskGetTickCount();
        DASH_GATEWAY_FOR_PENDING(q) {
            if (!q.inUse) continue;
            if (q.rulesVersion != gatewayDnsRulesVersion) { q.inUse=false; continue; }
            const TickType_t age=now-q.startTime;
            if (age >= pdMS_TO_TICKS(3000)) {
                ++gatewayDnsTimeouts; dashDnsFailPending(q,tx);
            } else if (!q.retried && age >= pdMS_TO_TICKS(1200)) {
                q.retried=true; ++gatewayDnsRetries;
                sockaddr_in dst={}; dst.sin_family=AF_INET; dst.sin_port=htons(53); dst.sin_addr.s_addr=q.upstream;
                if (lwip_sendto(gatewayUpstreamSock,q.query,q.queryLen,0,reinterpret_cast<sockaddr *>(&dst),sizeof(dst)) != q.queryLen)
                    ++gatewayDnsUpstreamFails;
            }
        }
        guard.unlock();
        vTaskDelay(1);
    }
}

static bool dashDnsTcpWait(int fd, short events, unsigned long deadline)
{
    while (static_cast<int32_t>(deadline-millis()) > 0) {
        pollfd p={fd,events,0};
        int ret=lwip_poll(&p,1,std::min<uint32_t>(50,deadline-millis()));
        if (ret < 0 || (p.revents&(POLLERR|POLLHUP|POLLNVAL))) return false;
        if (ret > 0 && (p.revents&events)) return true;
    }
    return false;
}

static bool dashDnsTcpBytes(int fd, uint8_t *data, size_t size, bool write, unsigned long deadline)
{
    size_t done=0;
    while (done < size && dashDnsTcpWait(fd,write ? POLLOUT : POLLIN,deadline)) {
        int n=write ? lwip_send(fd,data+done,size-done,0) : lwip_recv(fd,data+done,size-done,0);
        if (n > 0) done+=n;
        else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) return false;
    }
    return done == size;
}

static size_t dashDnsTcpUpstream(uint32_t upstream, uint8_t *query, size_t n, uint8_t *out,
                                 const dns_wire::Question &question, unsigned long deadline)
{
    int fd=dashDnsNonblockingSocket(SOCK_STREAM);
    if (fd < 0) return 0;
    struct Close { int fd; ~Close() { lwip_close(fd); } } close{fd};
    sockaddr_in dst={}; dst.sin_family=AF_INET; dst.sin_port=htons(53); dst.sin_addr.s_addr=upstream;
    int result=lwip_connect(fd,reinterpret_cast<sockaddr *>(&dst),sizeof(dst));
    if (result && errno != EINPROGRESS) return 0;
    if (result) {
        if (!dashDnsTcpWait(fd,POLLOUT,deadline)) return 0;
        int error=0; socklen_t len=sizeof(error);
        if (lwip_getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&len) || error) return 0;
    }
    uint8_t prefix[2]; dns_wire::put16(prefix,n);
    if (!dashDnsTcpBytes(fd,prefix,2,true,deadline) || !dashDnsTcpBytes(fd,query,n,true,deadline) ||
        !dashDnsTcpBytes(fd,prefix,2,false,deadline)) return 0;
    size_t length=dns_wire::u16(prefix);
    if (length > dns_wire::MaxMessage || length < 12 || !dashDnsTcpBytes(fd,out,length,false,deadline)) return 0;
    if (!dashDnsValidResponse(out,length,dns_wire::u16(query),question.name,question.type) || (out[2]&2)) return 0;
    return length;
}

static void dashGatewayDnsTcpTask(void *)
{
    uint8_t query[dns_wire::MaxQuery], response[dns_wire::MaxMessage];
    int listener=-1;
    for (;;) {
        if (listener < 0) {
            listener=dashDnsServerSocket(SOCK_STREAM);
            gatewayDnsTcpBindOk=listener >= 0;
            if (listener < 0) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
        }
        sockaddr_in client={}; socklen_t size=sizeof(client);
        int fd=lwip_accept(listener,reinterpret_cast<sockaddr *>(&client),&size);
        if (fd < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                lwip_close(listener); listener=-1; gatewayDnsTcpBindOk=false;
            }
            vTaskDelay(pdMS_TO_TICKS(20)); continue;
        }
        struct Close { int fd; ~Close() { lwip_close(fd); } } close{fd};
        if (!dashDnsApClient(client) || lwip_fcntl(fd,F_SETFL,O_NONBLOCK) < 0) continue;
        const unsigned long deadline=millis()+3500;
        uint8_t prefix[2];
        if (!dashDnsTcpBytes(fd,prefix,2,false,deadline)) continue;
        size_t n=dns_wire::u16(prefix);
        if (n > sizeof(query) || !dashDnsTcpBytes(fd,query,n,false,deadline)) continue;
        dns_wire::Question parsed; uint16_t udpCap; bool cacheable;
        if (!dns_wire::query(query,n,parsed,udpCap,cacheable)) continue;
        ++gatewayDnsTcpQueries;
        size_t length=0; uint32_t epoch=0, upstream=IPADDR_NONE;
        {
            DashGatewayGuard guard;
            length=dashDnsLocalReply(query,n,parsed,cacheable,response);
            epoch=gatewayDnsRulesVersion; upstream=gatewayUpstreamDns;
        }
        if (!length) {
            if (upstream && upstream != IPADDR_NONE)
                length=dashDnsTcpUpstream(upstream,query,n,response,parsed,deadline-300);
            DashGatewayGuard guard;
            if (epoch != gatewayDnsRulesVersion) length=0;
            if (length && cacheable) dashGatewayDnsCachePut(parsed.name,parsed.type,millis()/1000,response,length);
            if (!length) {
                ++gatewayDnsUpstreamFails;
                length=dns_wire::reply(query,n,response,sizeof(response),2);
            }
        }
        dns_wire::put16(prefix,length);
        if (dashDnsTcpBytes(fd,prefix,2,true,deadline)) dashDnsTcpBytes(fd,response,length,true,deadline);
        // One query per connection bounds resource use; UDP remains independent.
    }
}
