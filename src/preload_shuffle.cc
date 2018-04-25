/*
 * Copyright (c) 2017, Carnegie Mellon University.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT
 * HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY
 * WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <arpa/inet.h>
#include <assert.h>
#include <ifaddrs.h>

#include "preload_internal.h"
#include "preload_mon.h"
#include "preload_shuffle.h"

#include "nn_shuffler.h"
#include "nn_shuffler_internal.h"
#include "xn_shuffler.h"

#include <ch-placement.h>
#include <mercury_config.h>
#include <pdlfs-common/xxhash.h>

#include "common.h"

#if defined(__GNUC__)
static int bits_count(unsigned int v) { return __builtin_popcount(v); }
#else
static int bits_count(unsigned int v) {
  v = v - ((v >> 1) & 0x55555555);
  v = (v & 0x33333333) + ((v >> 2) & 0x33333333);
  int rv = ((v + (v >> 4) & 0xF0F0F0F) * 0x1010101) >> 24;
  return rv;
}
#endif

static const char* shuffle_prepare_sm_uri(char* buf, const char* proto) {
  int min_port;
  int max_port;
  const char* env;
  char msg[100];

  assert(strstr(proto, "sm") != NULL);

  if (pctx.my_rank == 0) {
    snprintf(msg, sizeof(msg),
             "using %s\n>>> may only be used in single-node tests!!!", proto);
    WARN(msg);
  }

  env = maybe_getenv("SHUFFLE_Min_port");
  if (env == NULL) {
    min_port = DEFAULT_MIN_PORT;
  } else {
    min_port = atoi(env);
  }

  env = maybe_getenv("SHUFFLE_Max_port");
  if (env == NULL) {
    max_port = DEFAULT_MAX_PORT;
  } else {
    max_port = atoi(env);
  }

  /* sanity check on port range */
  if (max_port - min_port < 0) ABORT("bad min-max port");
  if (min_port < 1) ABORT("bad min port");
  if (max_port > 65535) ABORT("bad max port");

  if (pctx.my_rank == 0) {
    snprintf(msg, sizeof(msg), "using port range [%d,%d]", min_port, max_port);
    INFO(msg);
  }

  /* finalize uri */
  sprintf(buf, "%s://%d:%d", proto, int(getpid()), min_port);
#ifndef NDEBUG
  if (pctx.verr || pctx.my_rank == 0) {
    snprintf(msg, sizeof(msg), "[hg] using %s (rank %d)", buf, pctx.my_rank);
    INFO(msg);
  }
#endif

  return buf;
}

const char* shuffle_prepare_uri(char* buf) {
  int family;
  int port;
  const char* env;
  int min_port;
  int max_port;
  struct ifaddrs *ifaddr, *cur;
  struct sockaddr_in addr;
  socklen_t addr_len;
  MPI_Comm comm;
  int rank;
  int size;
  const char* subnet;
  const char* proto;  // mercury proto
  char msg[100];
  char ip[50];  // ip
  int opt;
  int so;
  int rv;
  int n;

  proto = maybe_getenv("SHUFFLE_Mercury_proto");
  if (proto == NULL) {
    proto = DEFAULT_HG_PROTO;
  }
  if (strstr(proto, "sm") != NULL) {
    return shuffle_prepare_sm_uri(buf, proto);  // special handling for sm addrs
  }
  if (pctx.my_rank == 0) {
    snprintf(msg, sizeof(msg), "using %s", proto);
    if (strstr(proto, "tcp") != NULL) {
      WARN(msg);
    } else {
      INFO(msg);
    }
  }

  subnet = maybe_getenv("SHUFFLE_Subnet");
  if (subnet == NULL) {
    subnet = DEFAULT_SUBNET;
  }
  if (pctx.my_rank == 0) {
    snprintf(msg, sizeof(msg), "using subnet %s*", subnet);
    if (strcmp(subnet, "127.0.0.1") == 0) {
      WARN(msg);
    } else {
      INFO(msg);
    }
  }

  /* settle down an ip addr to use */
  if (getifaddrs(&ifaddr) == -1) {
    ABORT("getifaddrs");
  }

  for (cur = ifaddr; cur != NULL; cur = cur->ifa_next) {
    if (cur->ifa_addr != NULL) {
      family = cur->ifa_addr->sa_family;

      if (family == AF_INET) {
        if (getnameinfo(cur->ifa_addr, sizeof(struct sockaddr_in), ip,
                        sizeof(ip), NULL, 0, NI_NUMERICHOST) == -1)
          ABORT("getnameinfo");

        if (strncmp(subnet, ip, strlen(subnet)) == 0) {
          break;
        } else {
#ifndef NDEBUG
          if (pctx.verr || pctx.my_rank == 0) {
            snprintf(msg, sizeof(msg), "[ip] skip %s (rank %d)", ip,
                     pctx.my_rank);
            INFO(msg);
          }
#endif
        }
      }
    }
  }

  if (cur == NULL) /* maybe a wrong subnet has been specified */
    ABORT("no ip addr");

  freeifaddrs(ifaddr);

  /* get port through MPI rank */

  env = maybe_getenv("SHUFFLE_Min_port");
  if (env == NULL) {
    min_port = DEFAULT_MIN_PORT;
  } else {
    min_port = atoi(env);
  }

  env = maybe_getenv("SHUFFLE_Max_port");
  if (env == NULL) {
    max_port = DEFAULT_MAX_PORT;
  } else {
    max_port = atoi(env);
  }

  /* sanity check on port range */
  if (max_port - min_port < 0) ABORT("bad min-max port");
  if (min_port < 1) ABORT("bad min port");
  if (max_port > 65535) ABORT("bad max port");

  if (pctx.my_rank == 0) {
    snprintf(msg, sizeof(msg), "using port range [%d,%d]", min_port, max_port);
    INFO(msg);
  }

#if MPI_VERSION >= 3
  rv = MPI_Comm_split_type(MPI_COMM_WORLD, MPI_COMM_TYPE_SHARED, 0,
                           MPI_INFO_NULL, &comm);
  if (rv != MPI_SUCCESS) {
    ABORT("MPI_Comm_split_type");
  }
#else
  comm = MPI_COMM_WORLD;
#endif
  MPI_Comm_rank(comm, &rank);
  MPI_Comm_size(comm, &size);
  if (comm != MPI_COMM_WORLD) {
    MPI_Comm_free(&comm);
  }

  /* try and test port availability */
  port = min_port + (rank % (1 + max_port - min_port));
  for (; port <= max_port; port += size) {
    so = socket(PF_INET, SOCK_STREAM, 0);
    if (so != -1) {
      memset(&addr, 0, sizeof(addr));
      addr.sin_family = AF_INET;
      addr.sin_addr.s_addr = INADDR_ANY;
      addr.sin_port = htons(port);
      opt = 1;
      setsockopt(so, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
      n = bind(so, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
      close(so);
      if (n == 0) {
        break;
      }
    } else {
      ABORT("socket");
    }
  }

  if (port > max_port) {
    port = 0;
    WARN(
        "no free ports available within the specified range\n>>> "
        "auto detecting ports ...");
    so = socket(PF_INET, SOCK_STREAM, 0);
    if (so != -1) {
      memset(&addr, 0, sizeof(addr));
      addr.sin_family = AF_INET;
      addr.sin_addr.s_addr = INADDR_ANY;
      addr.sin_port = htons(0);
      opt = 0; /* do not reuse ports */
      setsockopt(so, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
      n = bind(so, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
      if (n == 0) {
        addr_len = sizeof(addr);
        n = getsockname(so, reinterpret_cast<struct sockaddr*>(&addr),
                        &addr_len);
        if (n == 0) {
          port = ntohs(addr.sin_port);
        }
      }
      close(so);
    } else {
      ABORT("socket");
    }
  }

  errno = 0;

  if (port == 0) /* maybe a wrong port range has been specified */
    ABORT("no free ports");

  /* finalize uri */
  sprintf(buf, "%s://%s:%d", proto, ip, port);
#ifndef NDEBUG
  if (pctx.verr || pctx.my_rank == 0) {
    snprintf(msg, sizeof(msg), "[hg] using %s (rank %d)", buf, pctx.my_rank);
    INFO(msg);
  }
#endif

  return buf;
}

void shuffle_epoch_pre_start(shuffle_ctx_t* ctx) {
  assert(ctx != NULL);
  if (ctx->type == SHUFFLE_XN) {
    xn_ctx_t* rep = static_cast<xn_ctx_t*>(ctx->rep);
    xn_shuffler_epoch_start(rep);
  } else {
    nn_shuffler_bgwait();
  }
}

/*
 * This function is called at the beginning of each epoch but before the epoch
 * really starts and before the final stats for the previous epoch are collected
 * and dumped. Therefore, this is a good time for us to copy xn_shuffler's
 * internal stats counters into preload's global mon context.
 */
void shuffle_epoch_start(shuffle_ctx_t* ctx) {
  assert(ctx != NULL);
  if (ctx->type == SHUFFLE_XN) {
    xn_ctx_t* rep = static_cast<xn_ctx_t*>(ctx->rep);
    xn_shuffler_epoch_start(rep);
    pctx.mctx.nlmr = rep->stat.local.recvs - rep->last_stat.local.recvs;
    pctx.mctx.min_nlmr = pctx.mctx.max_nlmr = pctx.mctx.nlmr;
    pctx.mctx.nlms = rep->stat.local.sends - rep->last_stat.local.sends;
    pctx.mctx.min_nlms = pctx.mctx.max_nlms = pctx.mctx.nlms;
    pctx.mctx.nlmd = pctx.mctx.nlms;
    pctx.mctx.nmr = rep->stat.remote.recvs - rep->last_stat.remote.recvs;
    pctx.mctx.min_nmr = pctx.mctx.max_nmr = pctx.mctx.nmr;
    pctx.mctx.nms = rep->stat.remote.sends - rep->last_stat.remote.sends;
    pctx.mctx.min_nms = pctx.mctx.max_nms = pctx.mctx.nms;
    pctx.mctx.nmd = pctx.mctx.nms;
  } else {
    nn_shuffler_bgwait();
  }
}

void shuffle_epoch_end(shuffle_ctx_t* ctx) {
  assert(ctx != NULL);
  if (ctx->type == SHUFFLE_XN) {
    xn_shuffler_epoch_end(static_cast<xn_ctx_t*>(ctx->rep));
  } else {
    nn_shuffler_flushq(); /* flush rpc queues */
    if (!nnctx.force_sync) {
      /* wait for rpc replies */
      nn_shuffler_waitcb();
    }
  }
}

int shuffle_write(shuffle_ctx_t* ctx, const char* path, char* data, size_t len,
                  int epoch) {
  int rv;
  const char* fname;
  unsigned char fname_len;
  unsigned long target;
  int world_sz;
  int peer_rank;
  int rank;

#ifndef NDEBUG
  char msg[200];
  int ha;
  int n;
#endif

  assert(ctx != NULL);
  assert(pctx.len_plfsdir != 0);
  assert(pctx.plfsdir != NULL);
  assert(path != NULL);

  fname = path + pctx.len_plfsdir + 1; /* remove parent path */
  assert(strlen(fname) < 256);
  fname_len = static_cast<unsigned char>(strlen(fname));
  assert(len < 256);

  if (ctx->type == SHUFFLE_XN) {
    world_sz = xn_shuffler_world_size(static_cast<xn_ctx_t*>(ctx->rep));
    rank = xn_shuffler_my_rank(static_cast<xn_ctx_t*>(ctx->rep));
  } else {
    world_sz = nn_shuffler_world_size();
    rank = nn_shuffler_my_rank();
  }

  if (world_sz != 1) {
    if (IS_BYPASS_PLACEMENT(pctx.mode)) {
      peer_rank = pdlfs::xxhash32(fname, fname_len, 0) % world_sz;
    } else {
      assert(ctx->chp != NULL);
      ch_placement_find_closest(ctx->chp, pdlfs::xxhash64(fname, fname_len, 0),
                                1, &target);
      peer_rank = int(target);
    }
  } else {
    peer_rank = rank;
  }

  /* skip non-receivers */
  peer_rank &= ctx->receiver_mask;

#ifndef NDEBUG
  /* write trace if we are in testing mode */
  if (pctx.testin && pctx.logfd != -1) {
    ha = pdlfs::xxhash32(data, len, 0); /* checksum */
    if (rank != peer_rank || ctx->force_rpc) {
      n = snprintf(msg, sizeof(msg),
                   "[SEND] %s %d bytes (e%d) r%d >> r%d (hash=%08x)\n", path,
                   int(len), epoch, rank, peer_rank, ha);
    } else {
      n = snprintf(msg, sizeof(msg), "[LO] %s %d bytes (e%d) (hash=%08x)\n",
                   path, int(len), epoch, ha);
    }

    n = write(pctx.logfd, msg, n);

    errno = 0;
  }
#endif

  /* bypass rpc if target is local */
  if (peer_rank == rank && !ctx->force_rpc) {
    rv = preload_local_write(path, data, len, epoch);
    return rv;
  }

  if (ctx->type == SHUFFLE_XN) {
    xn_shuffler_enqueue(static_cast<xn_ctx_t*>(ctx->rep), fname, fname_len,
                        data, len, epoch, peer_rank, rank);
  } else {
    nn_shuffler_enqueue(fname, fname_len, data, len, epoch, peer_rank, rank);
  }

  return 0;
}

int shuffle_handle(const char* fname, unsigned char fname_len, char* data,
                   size_t len, int epoch, int peer_rank, int rank) {
  /* here we assume we will only get called by a single thread.
   * this thread is either a dedicate mercury progressing thread, or a separate
   * rpc worker thread. in the 2nd case, we usually label this thread as the
   * "writing" thread or the "delivery" thread. */
  static char path[PATH_MAX];

#ifndef NDEBUG
  char msg[200];
  int ha;
  int n;
#endif

  int rv;

  assert(fname != NULL);
  assert(pctx.len_plfsdir != 0);
  assert(pctx.plfsdir != NULL);

  assert(fname[fname_len] == 0);
  snprintf(path, sizeof(path), "%s/%s", pctx.plfsdir, fname);
  rv = preload_foreign_write(path, data, len, epoch);
#ifndef NDEBUG
  /* write trace if we are in testing mode */
  if (pctx.testin && pctx.logfd != -1) {
    ha = pdlfs::xxhash32(data, len, 0); /* data checksum */
    n = snprintf(msg, sizeof(msg),
                 "[RECV] %s %d bytes (e%d) r%d "
                 "<< r%d (hash=%08x)\n",
                 path, int(len), epoch, rank, peer_rank, ha);
    n = write(pctx.logfd, msg, n);

    errno = 0;
  }
#endif
  return rv;
}

void shuffle_finalize(shuffle_ctx_t* ctx) {
  char msg[200];
  assert(ctx != NULL);
  if (ctx->type == SHUFFLE_XN && ctx->rep != NULL) {
    xn_ctx_t* rep = static_cast<xn_ctx_t*>(ctx->rep);
    xn_shuffler_destroy(rep);
    if (ctx->finalize_pause > 0) {
      sleep(ctx->finalize_pause);
    }
#ifndef NDEBUG
    unsigned long long sum_rpcs[2];
    unsigned long long min_rpcs[2];
    unsigned long long max_rpcs[2];
    unsigned long long rpcs[2];
    rpcs[0] = rep->stat.local.sends;
    rpcs[1] = rep->stat.remote.sends;
    MPI_Reduce(rpcs, sum_rpcs, 2, MPI_UNSIGNED_LONG_LONG, MPI_SUM, 0,
               MPI_COMM_WORLD);
    MPI_Reduce(rpcs, min_rpcs, 2, MPI_UNSIGNED_LONG_LONG, MPI_MIN, 0,
               MPI_COMM_WORLD);
    MPI_Reduce(rpcs, max_rpcs, 2, MPI_UNSIGNED_LONG_LONG, MPI_MAX, 0,
               MPI_COMM_WORLD);
    if (pctx.my_rank == 0 && (sum_rpcs[0] + sum_rpcs[1]) != 0) {
      snprintf(msg, sizeof(msg),
               "[rpc] total sends: %s intra-node + %s inter-node = %s overall "
               ".....\n"
               " -> intra-node: %s per rank (min: %s, max: %s)\n"
               " -> inter-node: %s per rank (min: %s, max: %s)\n"
               " //",
               pretty_num(sum_rpcs[0]).c_str(), pretty_num(sum_rpcs[1]).c_str(),
               pretty_num(sum_rpcs[0] + sum_rpcs[1]).c_str(),
               pretty_num(double(sum_rpcs[0]) / pctx.comm_sz).c_str(),
               pretty_num(min_rpcs[0]).c_str(), pretty_num(max_rpcs[0]).c_str(),
               pretty_num(double(sum_rpcs[1]) / pctx.comm_sz).c_str(),
               pretty_num(min_rpcs[1]).c_str(),
               pretty_num(max_rpcs[1]).c_str());
      INFO(msg);
    }
#endif
    ctx->rep = NULL;
    free(rep);
  } else {
    hstg_t hg_intvl;
    int p[] = {10, 30, 50, 70, 90, 95, 96, 97, 98, 99};
    double d[] = {99.5,  99.7,   99.9,   99.95,  99.97,
                  99.99, 99.995, 99.997, 99.999, 99.9999};
    nn_rusage_t total_rusage[4];
    unsigned long long total_writes;
    unsigned long long total_msgsz;
    hstg_t iq_dep;
    nn_shuffler_destroy();
    if (ctx->finalize_pause > 0) {
      sleep(ctx->finalize_pause);
    }
    if (pctx.recv_comm != MPI_COMM_NULL) {
      if (pctx.my_rank == 0) {
        INFO("[nn] per-thread cpu usage ... (s)");
        snprintf(msg, sizeof(msg), "                %-16s%-16s%-16s",
                 "USR_per_rank", "SYS_per_rank", "TOTAL_per_rank");
        INFO(msg);
      }
      for (size_t i = 0; i < sizeof(nnctx.r) / sizeof(nn_rusage_t); i++) {
        MPI_Reduce(&nnctx.r[i].usr_micros, &total_rusage[i].usr_micros, 1,
                   MPI_UNSIGNED_LONG_LONG, MPI_SUM, 0, pctx.recv_comm);
        MPI_Reduce(&nnctx.r[i].sys_micros, &total_rusage[i].sys_micros, 1,
                   MPI_UNSIGNED_LONG_LONG, MPI_SUM, 0, pctx.recv_comm);
        if (pctx.my_rank == 0) {
          if (nnctx.r[i].tag[0] != 0) {
            snprintf(
                msg, sizeof(msg), "  %-8s CPU: %-16.3f%-16.3f%-16.3f",
                nnctx.r[i].tag,
                double(total_rusage[i].usr_micros) / 1000000 / pctx.recv_sz,
                double(total_rusage[i].sys_micros) / 1000000 / pctx.recv_sz,
                double(total_rusage[i].usr_micros +
                       total_rusage[i].sys_micros) /
                    1000000 / pctx.recv_sz);
            INFO(msg);
          }
        }
      }
      memset(&hg_intvl, 0, sizeof(hstg_t));
      hstg_reset_min(hg_intvl);
      hstg_reduce(nnctx.hg_intvl, hg_intvl, pctx.recv_comm);
      if (pctx.my_rank == 0 && hstg_num(hg_intvl) >= 1.0) {
        INFO("[nn] hg_progress interval ... (ms)");
        snprintf(msg, sizeof(msg),
                 "  %s samples, avg: %.3f (min: %.0f, max: %.0f)",
                 pretty_num(hstg_num(hg_intvl)).c_str(), hstg_avg(hg_intvl),
                 hstg_min(hg_intvl), hstg_max(hg_intvl));
        INFO(msg);
        for (size_t i = 0; i < sizeof(p) / sizeof(int); i++) {
          snprintf(msg, sizeof(msg), "    - %d%% %-12.2f %.4f%% %.2f", p[i],
                   hstg_ptile(hg_intvl, p[i]), d[i],
                   hstg_ptile(hg_intvl, d[i]));
          INFO(msg);
        }
      }
      memset(&iq_dep, 0, sizeof(hstg_t));
      hstg_reset_min(iq_dep);
      hstg_reduce(nnctx.iq_dep, iq_dep, pctx.recv_comm);
      MPI_Reduce(&nnctx.total_writes, &total_writes, 1, MPI_UNSIGNED_LONG_LONG,
                 MPI_SUM, 0, pctx.recv_comm);
      MPI_Reduce(&nnctx.total_msgsz, &total_msgsz, 1, MPI_UNSIGNED_LONG_LONG,
                 MPI_SUM, 0, pctx.recv_comm);
      if (pctx.my_rank == 0 && hstg_num(iq_dep) >= 1.0) {
        snprintf(
            msg, sizeof(msg),
            "[nn] avg rpc size: %s (%s writes per rpc, %s per write)",
            pretty_size(double(total_msgsz) / hstg_sum(iq_dep)).c_str(),
            pretty_num(double(total_writes) / hstg_sum(iq_dep)).c_str(),
            pretty_size(double(total_msgsz) / double(total_writes)).c_str());
        INFO(msg);
        INFO("[nn] rpc incoming queue depth ...");
        snprintf(msg, sizeof(msg),
                 "  %s samples, avg: %.3f (min: %.0f, max: %.0f)",
                 pretty_num(hstg_num(iq_dep)).c_str(), hstg_avg(iq_dep),
                 hstg_min(iq_dep), hstg_max(iq_dep));
        INFO(msg);
        for (size_t i = 0; i < sizeof(p) / sizeof(int); i++) {
          snprintf(msg, sizeof(msg), "    - %d%% %-12.2f %.4f%% %.2f", p[i],
                   hstg_ptile(iq_dep, p[i]), d[i], hstg_ptile(iq_dep, d[i]));
          INFO(msg);
        }
      }
    }
  }
  if (ctx->chp != NULL) {
    ch_placement_finalize(ctx->chp);
    ctx->chp = NULL;
  }
}

void shuffle_init(shuffle_ctx_t* ctx) {
  int vf;
  int world_sz;
  char msg[200];
  const char* proto;
  const char* env;
  int n;
  assert(ctx != NULL);
  env = maybe_getenv("SHUFFLE_Finalize_pause");
  if (env != NULL) {
    ctx->finalize_pause = atoi(env);
    if (ctx->finalize_pause < 0) {
      ctx->finalize_pause = 0;
    }
  }
  if (pctx.my_rank == 0) {
    if (ctx->finalize_pause > 0) {
      snprintf(msg, sizeof(msg), "shuffle finalize pause: %d secs",
               ctx->finalize_pause);
      INFO(msg);
    }
  }
  if (is_envset("SHUFFLE_Force_rpc")) {
    ctx->force_rpc = 1;
  }
  if (pctx.my_rank == 0) {
    if (!ctx->force_rpc) {
      WARN(
          "shuffle force_rpc is OFF (will skip shuffle if addr is local)\n>>> "
          "main thread may be blocked on writing");
    } else {
      INFO(
          "shuffle force_rpc is ON\n>>> "
          "will always invoke shuffle even addr is local");
    }
  }
  if (is_envset("SHUFFLE_Use_multihop")) {
    ctx->type = SHUFFLE_XN;
    if (pctx.my_rank == 0) {
      snprintf(msg, sizeof(msg), "using the scalable multi-hop shuffler");
      INFO(msg);
    }
  } else {
    ctx->type = SHUFFLE_NN;
    if (pctx.my_rank == 0) {
      snprintf(msg, sizeof(msg),
               "using the default NN shuffler: code might not scale well\n>>> "
               "switch to the multi-hop shuffler for better scalability");
      WARN(msg);
    }
  }
  if (ctx->type == SHUFFLE_XN) {
    xn_ctx_t* rep = static_cast<xn_ctx_t*>(malloc(sizeof(xn_ctx_t)));
    memset(rep, 0, sizeof(xn_ctx_t));
    xn_shuffler_init(rep);
    world_sz = xn_shuffler_world_size(rep);
    ctx->rep = rep;
  } else {
    nn_shuffler_init();
    world_sz = nn_shuffler_world_size();
  }

  if (!IS_BYPASS_PLACEMENT(pctx.mode)) {
    env = maybe_getenv("SHUFFLE_Virtual_factor");
    if (env == NULL) {
      vf = DEFAULT_VIRTUAL_FACTOR;
    } else {
      vf = atoi(env);
    }

    proto = maybe_getenv("SHUFFLE_Placement_protocol");
    if (proto == NULL) {
      proto = DEFAULT_PLACEMENT_PROTO;
    }

    ctx->chp = ch_placement_initialize(proto, world_sz, vf /* vir factor */,
                                       0 /* hash seed */);
    if (ctx->chp == NULL) {
      ABORT("ch_init");
    }
  }

  if (pctx.my_rank == 0) {
    if (!IS_BYPASS_PLACEMENT(pctx.mode)) {
      snprintf(msg, sizeof(msg),
               "ch-placement group size: %s (vir-factor: %s, proto: %s)\n>>> "
               "possible protocols are: "
               "static_modulo, hash_lookup3, xor, and ring",
               pretty_num(world_sz).c_str(), pretty_num(vf).c_str(), proto);
      INFO(msg);
    } else {
      WARN("ch-placement bypassed");
    }
  }

  ctx->receiver_mask = ~static_cast<unsigned int>(0);
  env = maybe_getenv("SHUFFLE_Recv_radix");
  if (env != NULL) {
    n = atoi(env);
    if (n > 8) n = 8;
    if (n > 0) {
      ctx->receiver_mask <<= n;
    }
  }
  if (pctx.my_rank == 0) {
    snprintf(msg, sizeof(msg),
             "shuffle receiver mask = %d (32 - %d)\n>>> "
             "%u senders per receiver",
             bits_count(ctx->receiver_mask),
             32 - bits_count(ctx->receiver_mask),
             1U << (32 - bits_count(ctx->receiver_mask)));
    INFO(msg);
  }

  if (pctx.my_rank == 0) {
    n = 0;
    n += snprintf(msg + n, sizeof(msg) - n, "HG_HAS_POST_LIMIT is ");
#ifdef HG_HAS_POST_LIMIT
    n += snprintf(msg + n, sizeof(msg) - n, "TRUE");
#else
    n += snprintf(msg + n, sizeof(msg) - n, "FALSE");
#endif
    n += snprintf(msg + n, sizeof(msg) - n, ", HG_HAS_SELF_FORWARD is ");
#ifdef HG_HAS_SELF_FORWARD
    n += snprintf(msg + n, sizeof(msg) - n, "TRUE");
#else
    n += snprintf(msg + n, sizeof(msg) - n, "FALSE");
#endif
    n += snprintf(msg + n, sizeof(msg) - n, ", HG_HAS_EAGER_BULK is ");
#ifdef HG_HAS_EAGER_BULK
    n += snprintf(msg + n, sizeof(msg) - n, "TRUE");
#else
    n += snprintf(msg + n, sizeof(msg) - n, "FALSE");
#endif
    n += snprintf(msg + n, sizeof(msg) - n, "\n>>> HG_HAS_CHECKSUMS is ");
#ifdef HG_HAS_CHECKSUMS
    n += snprintf(msg + n, sizeof(msg) - n, "TRUE");
#else
    n += snprintf(msg + n, sizeof(msg) - n, "FALSE");
#endif
    INFO(msg);
  }
}

int shuffle_is_everyone_receiver(shuffle_ctx_t* ctx) {
  assert(ctx != NULL);
  int rv = int(32 == bits_count(ctx->receiver_mask));
  return rv;
}

int shuffle_rank(shuffle_ctx_t* ctx) {
  assert(ctx != NULL);
  if (ctx->type == SHUFFLE_XN) {
    return xn_shuffler_my_rank(static_cast<xn_ctx_t*>(ctx->rep));
  } else {
    return nn_shuffler_my_rank();
  }
}

int shuffle_is_receiver(shuffle_ctx_t* ctx) {
  assert(ctx != NULL);
  int my_rank = shuffle_rank(ctx);
  int rv = int((my_rank & ctx->receiver_mask) == my_rank);
  return rv;
}

int shuffle_receiver_rank(shuffle_ctx_t* ctx) {
  assert(ctx != NULL);
  int my_rank = shuffle_rank(ctx);
  my_rank >>= (32 - bits_count(ctx->receiver_mask));
  return my_rank;
}

void shuffle_resume(shuffle_ctx_t* ctx) {
  assert(ctx != NULL);
  if (ctx->type == SHUFFLE_XN) {
    // TODO
  } else {
    nn_shuffler_wakeup();
  }
}

void shuffle_pause(shuffle_ctx_t* ctx) {
  assert(ctx != NULL);
  if (ctx->type == SHUFFLE_XN) {
    // TODO
  } else {
    nn_shuffler_sleep();
  }
}

void shuffle_msg_sent(size_t n, void** arg1, void** arg2) {
  pctx.mctx.min_nms++;
  pctx.mctx.max_nms++;
  pctx.mctx.nms++;
}

void shuffle_msg_replied(void* arg1, void* arg2) {
  pctx.mctx.nmd++; /* delivered */
}

void shuffle_msg_received() {
  pctx.mctx.min_nmr++;
  pctx.mctx.max_nmr++;
  pctx.mctx.nmr++;
}
