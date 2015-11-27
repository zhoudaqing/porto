#pragma once

#include <string>
#include <functional>
#include <memory>

#include "common.hpp"
extern "C" {
#include <arpa/inet.h>
#include <linux/netlink.h>
}

const uint64_t NET_MAX_LIMIT = 0xFFFFFFFF;
const uint64_t NET_MAX_GUARANTEE = 0xFFFFFFFF;
const uint64_t NET_MAP_WHITEOUT = 0xFFFFFFFFFFFFFFFF;

struct nl_sock;
struct rtnl_link;
struct nl_cache;
struct nl_addr;
class TNlLink;

class TNlAddr {
    struct nl_addr *Addr = nullptr;
public:
    TNlAddr() {}
    TNlAddr(const TNlAddr &other);
    TNlAddr &operator=(const TNlAddr &other);
    ~TNlAddr();
    TError Parse(const std::string &s);
    struct nl_addr *GetAddr() const { return Addr; }
    bool IsEmpty() const;
};

enum class ETclassStat {
    Packets,
    Bytes,
    Drops,
    Overlimits,
    BPS,
    PPS,
};

uint32_t TcHandle(uint16_t maj, uint16_t min);

class TNl : public std::enable_shared_from_this<TNl>,
            public TNonCopyable {
    struct nl_sock *Sock = nullptr;

public:

    TNl() {}
    ~TNl() { Disconnect(); }

    TError Connect();
    void Disconnect();

    static void EnableDebug(bool enable);

    struct nl_sock *GetSock() { return Sock; }

    int GetFd();
    TError OpenLinks(std::vector<std::shared_ptr<TNlLink>> &links, bool all);

    static TError Error(int nl_err, const std::string &desc);
    void Dump(const std::string &prefix, void *obj);
};

class TNlLink : public TNonCopyable {
    std::shared_ptr<TNl> Nl;
    struct rtnl_link *Link = nullptr;

    TError AddXVlan(const std::string &vlantype,
                    const std::string &master,
                    uint32_t type,
                    const std::string &hw,
                    int mtu);

public:

    TNlLink(std::shared_ptr<TNl> sock, const std::string &name);
    TNlLink(std::shared_ptr<TNl> sock, struct rtnl_link *link);
    ~TNlLink();
    TError Load();

    int GetIndex() const;
    std::string GetName() const;
    std::string GetDesc() const;
    bool IsLoopback() const;
    bool IsRunning() const;
    TError Error(int nl_err, const std::string &desc) const;
    void Dump(const std::string &prefix, void *obj = nullptr) const;

    TError Remove();
    TError Up();
    TError ChangeNs(const std::string &newName, int nsFd);
    TError AddIpVlan(const std::string &master,
                     const std::string &mode, int mtu);
    TError AddMacVlan(const std::string &master,
                      const std::string &type, const std::string &hw,
                      int mtu);
    TError AddVeth(const std::string &name, const std::string &peerName, const std::string &hw, int mtu, int nsFd);

    static bool ValidIpVlanMode(const std::string &mode);
    static bool ValidMacVlanType(const std::string &type);
    static bool ValidMacAddr(const std::string &hw);

    TError SetDefaultGw(const TNlAddr &addr);
    TError SetIpAddr(const TNlAddr &addr, const int prefix);

    struct rtnl_link *GetLink() const { return Link; }
    struct nl_sock *GetSock() const { return Nl->GetSock(); }
    std::shared_ptr<TNl> GetNl() { return Nl; };

    void LogCache(struct nl_cache *cache) const;
};

class TNlClass : public TNonCopyable {
    const uint32_t Parent, Handle;

public:
    TNlClass(uint32_t parent, uint32_t handle) : Parent(parent), Handle(handle) {}

    TError GetProperties(const TNlLink &link, uint32_t &prio, uint32_t &rate, uint32_t &ceil);
    bool Exists(const TNlLink &link);
};

class TNlHtb : public TNonCopyable {
    const uint32_t Parent, Handle;

public:
    TNlHtb(uint32_t parent, uint32_t handle) : Parent(parent), Handle(handle) {}
    TError Create(const TNlLink &link, uint32_t defaultClass);
    TError Remove(const TNlLink &link);
    bool Exists(const TNlLink &link);
    bool Valid(const TNlLink &link, uint32_t defaultClass);
};

class TNlCgFilter : public TNonCopyable {
    const int FilterPrio = 10;
    const char *FilterType = "cgroup";
    const uint32_t Parent, Handle;

public:
    TNlCgFilter(uint32_t parent, uint32_t handle) : Parent(parent), Handle(handle) {}
    TError Create(const TNlLink &link);
    bool Exists(const TNlLink &link);
    TError Remove(const TNlLink &link);
};

TError ParseIpPrefix(const std::string &s, TNlAddr &addr, int &prefix);
