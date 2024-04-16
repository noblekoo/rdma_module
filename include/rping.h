/*
 * Copyright (c) 2005 Ammasso, Inc. All rights reserved.
 * Copyright (c) 2006 Open Grid Computing, Inc. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */


#include <endian.h>
#include <getopt.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <semaphore.h>
#include <pthread.h>
#include <inttypes.h>
#include <rdma/rdma_cma.h>
#include <rdma/rsocket.h>
#include <infiniband/ib.h>
#pragma once

static int debug = 0;
#define DEBUG_LOG if (debug) printf
#define CB_THREAD 8

#define BUF_SIZE 2105344 - sizeof(__be32)*2

/*
 * These states are used to signal events between the completion handler
 * and the main client or server thread.
 *
 * Once CONNECTED, they cycle through RDMA_READ_ADV, RDMA_WRITE_ADV,
 * and RDMA_WRITE_COMPLETE for each ping.
 */
enum test_state {
    IDLE = 1,
    CONNECT_REQUEST,
    ADDR_RESOLVED,
    ROUTE_RESOLVED,
    CONNECTED,
    RDMA_REQ_ADV,
    RDMA_REQ_COMPLETE,
    RDMA_REP_ADV,
    RDMA_REP_COMPLETE,
    DISCONNECTED,
    ERROR
};

struct rping_rdma_info {
    // __be64 buf;
    //char buf[256];
    char buf[BUF_SIZE];
    __be32 rkey;
    __be32 size;
};

/*
 * Default max buffer size for IO...
 */
#define RPING_BUFSIZE 64*1024
#define RPING_SQ_DEPTH 2
#define MTU 4096

/* Default string for print data and
 * minimum buffer size
 */
#define _stringify(_x) # _x
#define stringify(_x) _stringify(_x)

#define RPING_MSG_FMT           "rdma-ping-%d: "
#define RPING_MIN_BUFSIZE       sizeof(stringify(INT_MAX)) + sizeof(RPING_MSG_FMT)

/*
 * Control block struct.
 */
struct rping_cb {
    int cb_num;

    int server;            /* 0 iff client */
    pthread_t cqthread;
    pthread_t persistent_server_thread;
    struct ibv_comp_channel *channel;
    struct ibv_cq *cq;
    struct ibv_pd *pd;
    struct ibv_qp *qp;

    struct ibv_recv_wr rq_wr;    /* recv work request record */
    struct ibv_sge recv_sgl;    /* recv single SGE */
    struct rping_rdma_info recv_buf;/* malloc'd buffer */
    struct ibv_mr *recv_mr;        /* MR associated with this buffer */

    struct ibv_send_wr sq_wr;    /* send work request record */
    struct ibv_sge send_sgl;
    struct rping_rdma_info send_buf;/* single send buf */
    struct ibv_mr *send_mr;

    struct ibv_send_wr rdma_sq_wr;    /* rdma work request record */
    struct ibv_sge rdma_sgl;    /* rdma single SGE */
    char *rdma_buf;            /* used as rdma sink */
    struct ibv_mr *rdma_mr;

    uint32_t remote_rkey;        /* remote guys RKEY */
    uint64_t remote_addr;        /* remote guys TO */
    uint32_t remote_len;        /* remote guys LEN */

    char *start_buf;        /* rdma read src */
    struct ibv_mr *start_mr;

    enum test_state state;        /* used for cond/signalling */
    sem_t sem;

    struct sockaddr_storage sin;
    struct sockaddr_storage ssource;
    __be16 port;            /* dst port in NBO */
    int verbose;            /* verbose logging */
    int self_create_qp;        /* Create QP not via cma */
    int count;            /* ping count */
    int size;            /* ping data size */
    int validate;            /* validate ping data */

    /* CM stuff */
    pthread_t cmthread;
    struct rdma_event_channel *cm_channel;
    struct rdma_cm_id *cm_id;    /* connection on client side,*/
                    /* listener on service side. */
    struct rdma_cm_id *child_cm_id;    /* connection on server side */
};

struct t_cb {
    struct rping_cb *cb;
    sem_t *t_sem;
};

int get_addr(char *dst, struct sockaddr *addr);
void *cm_thread(void *arg);
void *t_rping_run_server(void *arg);
extern int lambda_server(struct rping_cb *cb, sem_t *t_sem);
// int rping_run_server(struct rping_cb *cb);