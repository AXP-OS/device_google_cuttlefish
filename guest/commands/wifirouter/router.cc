/*
 * Copyright (C) 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "common/libs/wifi/router.h"

#include <cerrno>
#include <cstddef>
#include <map>
#include <memory>
#include <set>

#include <gflags/gflags.h>
#include <glog/logging.h>
#include <netlink/genl/ctrl.h>
#include <netlink/genl/family.h>
#include <netlink/genl/genl.h>
#include <netlink/netlink.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

DEFINE_string(socket_name, "cvd-wifirouter",
              "Name of the unix-domain socket providing access for routing. "
              "Socket will be created in abstract namespace.");

namespace cvd {
namespace {
using MacHash = uint64_t;
using MacToClientsTable = std::multimap<MacHash, int>;
using ClientsTable = std::set<int>;

// Copied out of mac80211_hwsim.h header.
constexpr int HWSIM_CMD_REGISTER = 1;
constexpr int HWSIM_ATTR_ADDR_TRANSMITTER = 2;
constexpr int HWSIM_ATTR_MAX = 19;

// Name of the WIFI SIM Netlink Family.
constexpr char kWifiSimFamilyName[] = "MAC80211_HWSIM";
const int kMaxSupportedPacketSize = getpagesize();

// Get hash for mac address serialized to 6 bytes of data starting at specified
// location.
// We don't care about byte ordering as much as we do about having all bytes
// there. Byte order does not matter, we want to use it as a key in our map.
uint64_t GetMacHash(const void* macaddr) {
  auto typed = reinterpret_cast<const uint16_t*>(macaddr);
  return (1ull * typed[0] << 32) | (typed[1] << 16) | typed[2];
}

// Enable asynchronous notifications from MAC80211_HWSIM.
// - `sock` is a valid netlink socket connected to NETLINK_GENERIC,
// - `family` is MAC80211_HWSIM genl family number.
//
// Upon failure, this function will terminate execution of the program.
void RegisterForHWSimNotifications(nl_sock* sock, int family) {
  std::unique_ptr<nl_msg, void (*)(nl_msg*)> msg(
      nlmsg_alloc(), [](nl_msg* m) { nlmsg_free(m); });
  genlmsg_put(msg.get(), NL_AUTO_PID, NL_AUTO_SEQ, family, 0, NLM_F_REQUEST,
              HWSIM_CMD_REGISTER, 0);
  nl_send_auto(sock, msg.get());
  auto res = nl_wait_for_ack(sock);
  if (res < 0) {
    LOG(ERROR) << "Could not register for notifications: " << nl_geterror(res);
    exit(1);
  }
}

// Create and configure WIFI Router server socket.
// This function is guaranteed to success. If at any point an error is detected,
// the function will terminate execution of the program.
int CreateWifiRouterServerSocket() {
  auto fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
  if (fd <= 0) {
    LOG(ERROR) << "Could not create unix socket: " << strerror(-fd);
    exit(1);
  }

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  auto len = std::min(sizeof(addr.sun_path) - 2, FLAGS_socket_name.size());
  strncpy(&addr.sun_path[1], FLAGS_socket_name.c_str(), len);
  len += offsetof(sockaddr_un, sun_path) + 1;  // include heading \0 byte.
  auto res = bind(fd, reinterpret_cast<sockaddr*>(&addr), len);

  if (res < 0) {
    LOG(ERROR) << "Could not bind unix socket: " << strerror(-res);
    exit(1);
  }

  listen(fd, 4);
  return fd;
}

// Accept new WIFI Router client. When successful, client will be placed in
// clients table.
void AcceptNewClient(int server_fd, ClientsTable* clients) {
  auto client = accept(server_fd, nullptr, nullptr);
  if (client < 0) {
    LOG(ERROR) << "Could not accept client: " << strerror(errno);
    return;
  }

  clients->insert(client);
  LOG(INFO) << "Client " << client << " added.";
}

// Disconnect and remove client from list of registered clients and recipients
// of WLAN traffic.
void RemoveClient(int client, ClientsTable* clients,
                  MacToClientsTable* targets) {
  close(client);
  clients->erase(client);
  for (auto iter = targets->begin(); iter != targets->end();) {
    if (iter->second == client) {
      iter = targets->erase(iter);
    } else {
      ++iter;
    }
  }
  LOG(INFO) << "Client " << client << " removed.";
}

// Read MAC80211HWSIM packet, find the originating MAC address and redirect it
// to proper sink.
void RouteWIFIPacket(nl_sock* nl, int simfamily, ClientsTable* clients,
                     MacToClientsTable* targets) {
  sockaddr_nl tmp;
  uint8_t* buf;

  const auto len = nl_recv(nl, &tmp, &buf, nullptr);
  if (len < 0) {
    LOG(ERROR) << "Could not read from netlink: " << nl_geterror(len);
    return;
  }

  std::unique_ptr<nlmsghdr, void (*)(nlmsghdr*)> msg(
      reinterpret_cast<nlmsghdr*>(buf), [](nlmsghdr* m) { free(m); });

  // Discard messages that originate from anything else than MAC80211_HWSIM.
  if (msg->nlmsg_type != simfamily) return;

  std::unique_ptr<nl_msg, void (*)(nl_msg*)> rep(
      nlmsg_alloc(), [](nl_msg* m) { nlmsg_free(m); });
  genlmsg_put(rep.get(), 0, 0, 0, 0, 0, WIFIROUTER_CMD_NOTIFY, 0);

  // Note, this is generic netlink message, and uses different parsing
  // technique.
  nlattr* attrs[HWSIM_ATTR_MAX + 1];
  if (genlmsg_parse(msg.get(), 0, attrs, HWSIM_ATTR_MAX, nullptr)) return;

  std::set<int> pending_removals;
  auto addr = attrs[HWSIM_ATTR_ADDR_TRANSMITTER];
  if (addr != nullptr) {
    nla_put(rep.get(), WIFIROUTER_ATTR_MAC, nla_len(addr), nla_data(addr));
    nla_put(rep.get(), WIFIROUTER_ATTR_PACKET, len, buf);
    auto hdr = nlmsg_hdr(rep.get());

    auto key = GetMacHash(nla_data(attrs[HWSIM_ATTR_ADDR_TRANSMITTER]));
    LOG(INFO) << "Received netlink packet from " << std::hex << key;
    for (auto it = targets->find(key); it != targets->end() && it->first == key;
         ++it) {
      auto num_written = send(it->second, hdr, hdr->nlmsg_len, MSG_NOSIGNAL);
      if (num_written != static_cast<int64_t>(hdr->nlmsg_len)) {
        pending_removals.insert(it->second);
      }
    }

    for (auto client : pending_removals) {
      RemoveClient(client, clients, targets);
    }
  }
}

bool HandleClientMessage(int client, MacToClientsTable* targets) {
  std::unique_ptr<nlmsghdr, void (*)(nlmsghdr*)> msg(
      reinterpret_cast<nlmsghdr*>(malloc(kMaxSupportedPacketSize)),
      [](nlmsghdr* h) { free(h); });
  ssize_t size = recv(client, msg.get(), kMaxSupportedPacketSize, 0);

  // Invalid message or no data -> client invalid or disconnected.
  if (size == 0 || size != static_cast<ssize_t>(msg->nlmsg_len) ||
      size < static_cast<ssize_t>(sizeof(nlmsghdr))) {
    return false;
  }

  int result = -EINVAL;
  genlmsghdr* ghdr = reinterpret_cast<genlmsghdr*>(nlmsg_data(msg.get()));

  switch (ghdr->cmd) {
    case WIFIROUTER_CMD_REGISTER:
      // Register client to receive notifications for specified MAC address.
      nlattr* attrs[WIFIROUTER_ATTR_MAX];
      if (!nlmsg_parse(msg.get(), sizeof(genlmsghdr), attrs,
                       WIFIROUTER_ATTR_MAX - 1, nullptr)) {
        if (attrs[WIFIROUTER_ATTR_MAC] != nullptr) {
          targets->emplace(GetMacHash(nla_data(attrs[WIFIROUTER_ATTR_MAC])),
                           client);
          result = 0;
        }
      }
      break;

    default:
      break;
  }

  nlmsgerr err{.error = result};
  std::unique_ptr<nl_msg, void (*)(nl_msg*)> rsp(nlmsg_alloc(), nlmsg_free);
  nlmsg_put(rsp.get(), msg->nlmsg_pid, msg->nlmsg_seq, NLMSG_ERROR, 0, 0);
  nlmsg_append(rsp.get(), &err, sizeof(err), 0);
  auto hdr = nlmsg_hdr(rsp.get());
  if (send(client, hdr, hdr->nlmsg_len, MSG_NOSIGNAL) !=
      static_cast<int64_t>(hdr->nlmsg_len)) {
    return false;
  }
  return true;
}

// Process incoming requests from netlink, server or clients.
void ServerLoop(int server_fd, nl_sock* netlink_sock, int family) {
  ClientsTable clients;
  MacToClientsTable targets;
  int netlink_fd = nl_socket_get_fd(netlink_sock);

  while (true) {
    auto max_fd = server_fd;
    fd_set reads{};

    auto fdset = [&max_fd, &reads](int fd) {
      FD_SET(fd, &reads);
      max_fd = std::max(max_fd, fd);
    };

    fdset(server_fd);
    fdset(netlink_fd);
    for (int client : clients) fdset(client);

    if (select(max_fd + 1, &reads, nullptr, nullptr, nullptr) <= 0) continue;

    if (FD_ISSET(server_fd, &reads)) AcceptNewClient(server_fd, &clients);
    if (FD_ISSET(netlink_fd, &reads))
      RouteWIFIPacket(netlink_sock, family, &clients, &targets);

    // Process any client messages left. Drop any client that is no longer
    // talking with us.
    for (auto client = clients.begin(); client != clients.end();) {
      auto cfd = *client++;
      // Is our client sending us data?
      if (FD_ISSET(cfd, &reads)) {
        if (!HandleClientMessage(cfd, &targets)) {
          // Client should be disconnected.
          RemoveClient(cfd, &clients, &targets);
        }
      }
    }
  }
}

}  // namespace
}  // namespace cvd

int main(int argc, char* argv[]) {
  using namespace cvd;
  google::ParseCommandLineFlags(&argc, &argv, true);
#if !defined(ANDROID)
  // We should check for legitimate google logging here.
  google::InitGoogleLogging(argv[0]);
  google::InstallFailureSignalHandler();
#endif

  std::unique_ptr<nl_sock, void (*)(nl_sock*)> sock(nl_socket_alloc(),
                                                    nl_socket_free);

  auto res = nl_connect(sock.get(), NETLINK_GENERIC);
  if (res < 0) {
    LOG(ERROR) << "Could not connect to netlink generic: " << nl_geterror(res);
    exit(1);
  }

  auto mac80211_family = genl_ctrl_resolve(sock.get(), kWifiSimFamilyName);
  if (mac80211_family <= 0) {
    LOG(ERROR) << "Could not find MAC80211 HWSIM. Please make sure module "
               << "'mac80211_hwsim' is loaded on your system.";
    exit(1);
  }

  RegisterForHWSimNotifications(sock.get(), mac80211_family);
  auto server_fd = CreateWifiRouterServerSocket();
  ServerLoop(server_fd, sock.get(), mac80211_family);
}
