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
#include <assert.h>
#include <time.h> //for random hash table key
#include <stdlib.h> //for random hash table key
#include <sys/time.h>
#include <semaphore.h>
#include <string.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <unistd.h>
#include "common.h"  // NOLINT(build/include_subdir)
#include "rping_test.h"
//#include "time_util.h"

//#define PRINT_TIME 1
#ifdef PRINT_TIME

#define DEBUG_TIME_START(evt) START_TIMER(evt)
#define DEBUG_TIME_END(evt) END_TIMER(evt)
#define DEBUG_EVENT_TIMER(evt) EVENT_TIMER(evt)


#else /* PRINT_TIME */
#define DEBUG_TIME_START(...)
#define DEBUG_TIME_END(...)
#define DEBUG_EVENT_TIMER(...)

#endif

DEBUG_EVENT_TIMER(evt_whole_process );
DEBUG_EVENT_TIMER(evt_post_recv     );
DEBUG_EVENT_TIMER(evt_post_send     );
DEBUG_EVENT_TIMER(evt_wait_send_cq  );
DEBUG_EVENT_TIMER(evt_handle_cqe    );
DEBUG_EVENT_TIMER(evt_event_callback_called);

#ifdef PRINT_TIME
static void print_time_stat(void) {
    PRINT_HDR();
    PRINT_TIMER(evt_whole_process,  "whole_process");
    PRINT_TIMER(evt_post_recv,      "post_recv");
    PRINT_TIMER(evt_post_send,      "post_send");
    PRINT_TIMER(evt_wait_send_cq,   "wait_send_cq");
    PRINT_TIMER(evt_handle_cqe,     "handle_cqe");
    PRINT_TIMER(evt_event_callback_called,     "evt_event_callback_called");
}
#else
static void print_time_stat(void) { }
#endif

#define SZ_4K 4096
#define SZ_2M 2097152

#define MAX_PAYLOAD 2101376
#define MSG_TYPE1 0x10
#define MSG_TYPE2 0x20
#define NETLINK_USER 31

#define THREAD_NUM 1
static pthread_t th_pool[10];

struct extent_t {
    uint64_t addr : 40; // in bytes, 1TB
    uint64_t len : 24; // in bytes, 16MB
};

struct nvme_meta {
    int64_t size_o;
    uint64_t calc_ns;
    uint64_t pre_ns;
    char buf_o[0];
};

// static int handle_request(struct rping_cb *cb);

/*
 * rping "ping/pong" loop:
 *     client sends source rkey/addr/len
 *    server receives source rkey/add/len
 *    server rdma reads "ping" data from source
 *     server sends "go ahead" on rdma read completion
 *    client sends sink rkey/addr/len
 *     server receives sink rkey/addr/len
 *     server rdma writes "pong" data to sink
 *     server sends "go ahead" on rdma write completion
 *     <repeat loop>
 */

static int rping_cma_event_handler(struct rdma_cm_id *cma_id,
                    struct rdma_cm_event *event) {
    int ret = 0;
    struct rping_cb *cb = (struct rping_cb *)cma_id->context;

    printf("cma_event type %s cma_id %p (%s)\n",
          rdma_event_str(event->event), cma_id,
          (cma_id == cb->cm_id) ? "parent" : "child");

    switch (event->event) {
    case RDMA_CM_EVENT_ADDR_RESOLVED:
        cb->state = ADDR_RESOLVED;
        ret = rdma_resolve_route(cma_id, 2000);
        if (ret) {
            cb->state = ERROR;
            perror("rdma_resolve_route");
            sem_post(&cb->sem);
        }
        break;

    case RDMA_CM_EVENT_ROUTE_RESOLVED:
        cb->state = ROUTE_RESOLVED;
        sem_post(&cb->sem);
        break;

    case RDMA_CM_EVENT_CONNECT_REQUEST:
        cb->state = CONNECT_REQUEST;
        cb->child_cm_id = cma_id;
        DEBUG_LOG("child cma %p\n", cb->child_cm_id);
        sem_post(&cb->sem);
        break;

    case RDMA_CM_EVENT_CONNECT_RESPONSE:
        DEBUG_LOG("CONNECT_RESPONSE\n");
        if (!cb->server) {
            cb->state = CONNECTED;
        }
        sem_post(&cb->sem);
        break;

    case RDMA_CM_EVENT_ESTABLISHED:
        DEBUG_LOG("ESTABLISHED\n");

        /*
         * Server will wake up when first RECV completes.
         */
        if (!cb->server) {
            cb->state = CONNECTED;
        }
        sem_post(&cb->sem);
        break;

    case RDMA_CM_EVENT_ADDR_ERROR:
    case RDMA_CM_EVENT_ROUTE_ERROR:
    case RDMA_CM_EVENT_CONNECT_ERROR:
    case RDMA_CM_EVENT_UNREACHABLE:
    case RDMA_CM_EVENT_REJECTED:
        fprintf(stderr, "cma event %s, error %d\n",
            rdma_event_str(event->event), event->status);
        sem_post(&cb->sem);
        ret = -1;
        break;

    case RDMA_CM_EVENT_DISCONNECTED:
        fprintf(stderr, "%s DISCONNECT EVENT...\n",
            cb->server ? "server" : "client");
        cb->state = DISCONNECTED;
        sem_post(&cb->sem);
        break;

    case RDMA_CM_EVENT_DEVICE_REMOVAL:
        fprintf(stderr, "cma detected device removal!!!!\n");
        cb->state = ERROR;
        sem_post(&cb->sem);
        ret = -1;
        break;

    default:
        fprintf(stderr, "unhandled event: %s, ignoring\n",
            rdma_event_str(event->event));
        break;
    }

    return ret;
}

static int server_recv(struct rping_cb *cb, struct ibv_wc *wc) {
    // Do not check size of data received. We use static buf size, 256.
    // if (wc->byte_len != sizeof(cb->recv_buf)) {
    //     fprintf(stderr, "Received bogus data, recved_size=%d buf_size=%d\n", wc->byte_len, sizeof(cb->recv_buf));
    //     return -1;
    // }

    // cb->remote_rkey = be32toh(cb->recv_buf.rkey);
    // // cb->remote_addr = be64toh(cb->recv_buf.buf);
    // cb->remote_addr = (uint64_t)cb->recv_buf.buf;
    // cb->remote_len  = be32toh(cb->recv_buf.size);
    // printf("Received rkey %x addr %" PRIx64 " len %d from peer\n",
    //       cb->remote_rkey, cb->remote_addr, cb->remote_len);

    printf("server_recv\n");
    // if (cb->state <= CONNECTED || cb->state == RDMA_REP_COMPLETE)
    //     cb->state = RDMA_REQ_ADV;
    // else
    //     cb->state = RDMA_REP_ADV;
    if (cb->state == RDMA_REQ_ADV)
        cb->state = RDMA_REQ_COMPLETE;
    else if (cb->state == RDMA_REP_ADV)
        cb->state = RDMA_REP_COMPLETE;
    else {
        printf("(ERROR) cb->state(%d) is incorrect.\n", cb->state);
        return -1;
    }

    return 0;
}

static int rping_cq_event_handler(struct rping_cb *cb, sem_t *t_sem) {
    struct ibv_wc wc;
    struct ibv_recv_wr *bad_wr;
    int ret;
    int flushed = 0;

    DEBUG_TIME_START(evt_handle_cqe);

    while ((ret = ibv_poll_cq(cb->cq, 1, &wc)) == 1) {
        ret = 0;

        if (wc.status) {
            if (wc.status == IBV_WC_WR_FLUSH_ERR) {
                flushed = 1;
                continue;
            }
            fprintf(stderr,
                "cq completion failed status %d\n",
                wc.status);
            ret = -1;
            goto error;
        }

        switch (wc.opcode) {
        case IBV_WC_SEND:
            DEBUG_TIME_END(evt_event_callback_called);
            DEBUG_LOG("send reply completion\n");
            // Do not check completion of sending request, now.
            // ret = server_recv(cb, &wc);
            // sem_post(&cb->sem);
            break;

        // case IBV_WC_RDMA_WRITE:
        //     DEBUG_LOG("rdma write completion\n");
        //     cb->state = RDMA_WRITE_COMPLETE;
        //     sem_post(&cb->sem);
        //     break;

        // case IBV_WC_RDMA_READ:
        //     DEBUG_LOG("rdma read completion\n");
        //     cb->state = RDMA_READ_COMPLETE;
        //     sem_post(&cb->sem);
        //     break;

        case IBV_WC_RECV:
            DEBUG_LOG("recv request completion\n");
            ret = server_recv(cb, &wc);
            if (ret) {
                fprintf(stderr, "recv wc error: %d\n", ret);
                goto error;
            }

            // Handle MDCache request.
            // handle_request(cb);

            // ret = ibv_post_recv(cb->qp, &cb->rq_wr, &bad_wr);
            // if (ret) {
            //     fprintf(stderr, "post recv error: %d\n", ret);
            //     goto error;
            // }
            // DEBUG_LOG("recv posted\n");
            sem_post(t_sem);
            break;

        default:
            DEBUG_LOG("unknown!!!!! completion\n");
            ret = -1;
            goto error;
        }
    }

    DEBUG_TIME_END(evt_handle_cqe);

    if (ret) {
        fprintf(stderr, "poll error %d\n", ret);
        goto error;
    }
    return flushed;

error:
    cb->state = ERROR;
    sem_post(t_sem);
    return ret;
}

static void rping_init_conn_param(struct rping_cb *cb,
                  struct rdma_conn_param *conn_param) {
    memset(conn_param, 0, sizeof(*conn_param));
    conn_param->responder_resources = 1;
    conn_param->initiator_depth = 1;
    conn_param->retry_count = 1;
    conn_param->rnr_retry_count = 1;
    if (cb->self_create_qp)
        conn_param->qp_num = cb->qp->qp_num;
}


static int rping_self_modify_qp(struct rping_cb *cb, struct rdma_cm_id *id) {
    struct ibv_qp_attr qp_attr;
    int qp_attr_mask, ret;

    qp_attr.qp_state = IBV_QPS_INIT;
    ret = rdma_init_qp_attr(id, &qp_attr, &qp_attr_mask);
    if (ret)
        return ret;

    ret = ibv_modify_qp(cb->qp, &qp_attr, qp_attr_mask);
    if (ret)
        return ret;

    qp_attr.qp_state = IBV_QPS_RTR;
    ret = rdma_init_qp_attr(id, &qp_attr, &qp_attr_mask);
    if (ret)
        return ret;

    ret = ibv_modify_qp(cb->qp, &qp_attr, qp_attr_mask);
    if (ret)
        return ret;

    qp_attr.qp_state = IBV_QPS_RTS;
    ret = rdma_init_qp_attr(id, &qp_attr, &qp_attr_mask);
    if (ret)
        return ret;

    return ibv_modify_qp(cb->qp, &qp_attr, qp_attr_mask);
}

static int rping_accept(struct rping_cb *cb) {
    struct rdma_conn_param conn_param;
    int ret;

    DEBUG_LOG("accepting client connection request\n");

    if (cb->self_create_qp) {
        ret = rping_self_modify_qp(cb, cb->child_cm_id);
        if (ret)
            return ret;

        rping_init_conn_param(cb, &conn_param);
        ret = rdma_accept(cb->child_cm_id, &conn_param);
    } else {
        ret = rdma_accept(cb->child_cm_id, NULL);
    }
    if (ret) {
        perror("rdma_accept");
        return ret;
    }

    sem_wait(&cb->sem);
    if (cb->state == ERROR) {
        fprintf(stderr, "wait for CONNECTED state %d\n", cb->state);
        return -1;
    }
    return 0;
}

static int rping_disconnect(struct rping_cb *cb, struct rdma_cm_id *id) {
    struct ibv_qp_attr qp_attr = {};
    int err = 0;

    if (cb->self_create_qp) {
        qp_attr.qp_state = IBV_QPS_ERR;
        err = ibv_modify_qp(cb->qp, &qp_attr, IBV_QP_STATE);
        if (err)
            return err;
    }

    return rdma_disconnect(id);
}

static void rping_setup_wr(struct rping_cb *cb) {
    
    cb->recv_sgl.addr = (uint64_t) &cb->recv_buf;
    cb->recv_sgl.length = sizeof cb->recv_buf;
    cb->recv_sgl.lkey = cb->recv_mr->lkey;
    cb->rq_wr.sg_list = &cb->recv_sgl;
    cb->rq_wr.num_sge = 1;

    cb->send_sgl.addr = (uint64_t) &cb->send_buf;
    cb->send_sgl.length = sizeof cb->send_buf;
    cb->send_sgl.lkey = cb->send_mr->lkey;

    cb->sq_wr.opcode = IBV_WR_SEND;
    cb->sq_wr.send_flags = IBV_SEND_SIGNALED;
    cb->sq_wr.sg_list = &cb->send_sgl;
    cb->sq_wr.num_sge = 1;

    cb->rdma_sgl.addr = (uint64_t) cb->rdma_buf;
    //cb->rdma_sgl.length = sizeof(struct rping_rdma_info);
    cb->rdma_sgl.lkey = cb->rdma_mr->lkey;
    cb->rdma_sq_wr.send_flags = IBV_SEND_SIGNALED;
    cb->rdma_sq_wr.sg_list = &cb->rdma_sgl;
    cb->rdma_sq_wr.num_sge = 1;
}

static int rping_setup_buffers(struct rping_cb *cb) {
    int ret;

    DEBUG_LOG("rping_setup_buffers called on cb %p\n", cb);

    cb->recv_mr = ibv_reg_mr(cb->pd, &cb->recv_buf, sizeof cb->recv_buf,
                 IBV_ACCESS_LOCAL_WRITE);
    if (!cb->recv_mr) {
        fprintf(stderr, "recv_buf reg_mr failed\n");
        return errno;
    }

    cb->send_mr = ibv_reg_mr(cb->pd, &cb->send_buf, sizeof cb->send_buf, 0);
    if (!cb->send_mr) {
        fprintf(stderr, "send_buf reg_mr failed\n");
        ret = errno;
        goto err1;
    }

    cb->rdma_buf = (char *)malloc(cb->size);
    if (!cb->rdma_buf) {
        fprintf(stderr, "rdma_buf malloc failed\n");
        ret = -ENOMEM;
        goto err2;
    }

    cb->rdma_mr = ibv_reg_mr(cb->pd, cb->rdma_buf, cb->size,
                 IBV_ACCESS_LOCAL_WRITE |
                 IBV_ACCESS_REMOTE_READ |
                 IBV_ACCESS_REMOTE_WRITE);
    if (!cb->rdma_mr) {
        fprintf(stderr, "rdma_buf reg_mr failed\n");
        ret = errno;
        goto err3;
    }

    if (!cb->server) {
        cb->start_buf = (char *)malloc(cb->size);
        if (!cb->start_buf) {
            fprintf(stderr, "start_buf malloc failed\n");
            ret = -ENOMEM;
            goto err4;
        }

        cb->start_mr = ibv_reg_mr(cb->pd, cb->start_buf, cb->size,
                      IBV_ACCESS_LOCAL_WRITE |
                      IBV_ACCESS_REMOTE_READ |
                      IBV_ACCESS_REMOTE_WRITE);
        if (!cb->start_mr) {
            fprintf(stderr, "start_buf reg_mr failed\n");
            ret = errno;
            goto err5;
        }
    }

    rping_setup_wr(cb);
    DEBUG_LOG("allocated & registered buffers...\n");
    return 0;

err5:
    free(cb->start_buf);
err4:
    ibv_dereg_mr(cb->rdma_mr);
err3:
    free(cb->rdma_buf);
err2:
    ibv_dereg_mr(cb->send_mr);
err1:
    ibv_dereg_mr(cb->recv_mr);
    return ret;
}

static void rping_free_buffers(struct rping_cb *cb) {
    DEBUG_LOG("rping_free_buffers called on cb %p\n", cb);
    ibv_dereg_mr(cb->recv_mr);
    ibv_dereg_mr(cb->send_mr);
    ibv_dereg_mr(cb->rdma_mr);
    free(cb->rdma_buf);
    if (!cb->server) {
        ibv_dereg_mr(cb->start_mr);
        free(cb->start_buf);
    }
}

static int rping_create_qp(struct rping_cb *cb) {
    struct ibv_qp_init_attr init_attr;
    struct rdma_cm_id *id;
    int ret;

    memset(&init_attr, 0, sizeof(init_attr));
    init_attr.cap.max_send_wr = RPING_SQ_DEPTH;
    init_attr.cap.max_recv_wr = RPING_SQ_DEPTH;
    init_attr.cap.max_recv_sge = 1;
    init_attr.cap.max_send_sge = 1;
    init_attr.qp_type = IBV_QPT_RC;
    init_attr.send_cq = cb->cq;
    init_attr.recv_cq = cb->cq;
    id = cb->server ? cb->child_cm_id : cb->cm_id;

    if (cb->self_create_qp) {
        cb->qp = ibv_create_qp(cb->pd, &init_attr);
        if (!cb->qp) {
            perror("ibv_create_qp");
            return -1;
        }

        struct ibv_qp_attr attr;
        memset(&attr, 0 ,sizeof(struct ibv_qp_attr));
        attr.qp_state = IBV_QPS_INIT;
        attr.pkey_index = 0;
        attr.port_num = id->port_num;
        attr.qp_access_flags = 0;
        //attr.path_mtu = IBV_MTU_4096;

        // ret = ibv_modify_qp(cb->qp, &attr,
        //             IBV_QP_STATE | IBV_QP_PATH_MTU | IBV_QP_PKEY_INDEX |
        //             IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);

        ret = ibv_modify_qp(cb->qp, &attr,
                    IBV_QP_STATE | IBV_QP_PKEY_INDEX |
                    IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);

        if (ret) {
            perror("ibv_modify_qp");
            ibv_destroy_qp(cb->qp);
        }
        return ret ? -1 : 0;
    }

    ret = rdma_create_qp(id, cb->pd, &init_attr);
    if (!ret)
        cb->qp = id->qp;
    else
        perror("rdma_create_qp");
    return ret;
}

static void rping_free_qp(struct rping_cb *cb) {
    ibv_destroy_qp(cb->qp);
    ibv_destroy_cq(cb->cq);
    ibv_destroy_comp_channel(cb->channel);
    ibv_dealloc_pd(cb->pd);
}

static int rping_setup_qp(struct rping_cb *cb, struct rdma_cm_id *cm_id) {
    int ret;

    cb->pd = ibv_alloc_pd(cm_id->verbs);
    if (!cb->pd) {
        fprintf(stderr, "ibv_alloc_pd failed\n");
        return errno;
    }
    DEBUG_LOG("created pd %p\n", cb->pd);

    cb->channel = ibv_create_comp_channel(cm_id->verbs);
    if (!cb->channel) {
        fprintf(stderr, "ibv_create_comp_channel failed\n");
        ret = errno;
        goto err1;
    }
    DEBUG_LOG("created channel %p\n", cb->channel);

    cb->cq = ibv_create_cq(cm_id->verbs, RPING_SQ_DEPTH * 2, cb,
                cb->channel, 0);
    if (!cb->cq) {
        fprintf(stderr, "ibv_create_cq failed\n");
        ret = errno;
        goto err2;
    }
    DEBUG_LOG("created cq %p\n", cb->cq);

    ret = ibv_req_notify_cq(cb->cq, 0);
    if (ret) {
        fprintf(stderr, "ibv_create_cq failed\n");
        ret = errno;
        goto err3;
    }

    ret = rping_create_qp(cb);
    if (ret) {
        goto err3;
    }
    DEBUG_LOG("created qp %p\n", cb->qp);
    return 0;

err3:
    ibv_destroy_cq(cb->cq);
err2:
    ibv_destroy_comp_channel(cb->channel);
err1:
    ibv_dealloc_pd(cb->pd);
    return ret;
}

void *cm_thread(void *arg) {
    struct rping_cb *cb = (struct rping_cb *)arg;
    struct rdma_cm_event *event;
    int ret;

    while (1) {
        ret = rdma_get_cm_event(cb->cm_channel, &event);
        if (ret) {
            perror("rdma_get_cm_event");
            exit(ret);
        }
        ret = rping_cma_event_handler(event->id, event);
        rdma_ack_cm_event(event);
        if (ret)
            exit(ret);
    }
}

static void *cq_thread(void *arg) {
    struct t_cb *thread_cb = (struct t_cb *)arg;
    struct rping_cb *cb = thread_cb->cb;
    sem_t *t_sem = thread_cb->t_sem;
    struct ibv_cq *ev_cq;
    void *ev_ctx;
    int ret;

    DEBUG_LOG("cq_thread started.\n");

    while (1) {
        pthread_testcancel();

        ret = ibv_get_cq_event(cb->channel, &ev_cq, &ev_ctx);
        if (ret) {
            fprintf(stderr, "Failed to get cq event!\n");
            pthread_exit(NULL);
        }
        if (ev_cq != cb->cq) {
            fprintf(stderr, "Unknown CQ!\n");
            pthread_exit(NULL);
        }
        ret = ibv_req_notify_cq(cb->cq, 0);
        if (ret) {
            fprintf(stderr, "Failed to set notify!\n");
            pthread_exit(NULL);
        }
        ret = rping_cq_event_handler(cb, t_sem);
        ibv_ack_cq_events(cb->cq, 1);
        if (ret)
            pthread_exit(NULL);
    }
}

// Not called.
/*
static void rping_format_send(struct rping_cb *cb, char *buf, struct ibv_mr *mr) {
    struct rping_rdma_info *info = &cb->send_buf;

    info->buf = htobe64((uint64_t) buf);
    info->rkey = htobe32(mr->rkey);
    info->size = htobe32(cb->size);

    DEBUG_LOG("RDMA addr %" PRIx64" rkey %x len %d\n",
          be64toh(info->buf), be32toh(info->rkey), be32toh(info->size));
}
*/

// static void fill_mb(struct mdt_body *mb, struct mdcache_entry *data)
// {
//     memcpy(mb, &data->body, sizeof(struct mdt_body));

//     /*
//     mb->mbo_fid1.f_seq = data->body.mbo_fid1.f_seq;
//     mb->mbo_fid1.f_oid = data->body.mbo_fid1.f_oid;
//     mb->mbo_fid1.f_ver = data->body.mbo_fid1.f_ver;
//     mb->mbo_fid2.f_seq = data->body.mbo_fid2.f_seq;
//     mb->mbo_fid2.f_oid = data->body.mbo_fid2.f_oid;
//     mb->mbo_fid2.f_ver = data->body.mbo_fid2.f_ver;

//     mb->mbo_valid = data->body.mbo_valid;
//     mb->mbo_size = data->body.mbo_size;
//     mb->mbo_mtime = data->body.mbo_mtime;
//     mb->mbo_atime = data->body.mbo_atime;
//     mb->mbo_ctime = data->body.mbo_ctime;
//     mb->mbo_blocks = data->body.mbo_blocks;
//     mb->mbo_version = data->body.mbo_version;
//     mb->mbo_t_state = data->body.mbo_t_state;
//     mb->mbo_fsuid = data->body.mbo_fsuid;
//     mb->mbo_fsgid = data->body.mbo_fsgid;
//     mb->mbo_capability = data->body.mbo_capability;
//     mb->mbo_mode = data->body.mbo_mode;
//     mb->mbo_uid = data->body.mbo_uid;
//     mb->mbo_gid = data->body.mbo_gid;
//     mb->mbo_flags = data->body.mbo_flags;
//     mb->mbo_rdev = data->body.mbo_rdev;
//     mb->mbo_nlink = data->body.mbo_nlink;
//     mb->mbo_layout_gen = data->body.mbo_layout_gen;
//     mb->mbo_suppgid = data->body.mbo_suppgid;
//     mb->mbo_eadatasize = data->body.mbo_eadatasize;
//     mb->mbo_aclsize = data->body.mbo_aclsize;
//     mb->mbo_max_mdsize = data->body.mbo_max_mdsize;
//     mb->mbo_unused3 = data->body.mbo_unused3;
//     mb->mbo_uid_h = data->body.mbo_uid_h;
//     mb->mbo_gid_h = data->body.mbo_gid_h;
//     mb->mbo_projid = data->body.mbo_projid;
//     mb->mbo_dom_size = data->body.mbo_dom_size;
//     mb->mbo_dom_blocks = data->body.mbo_dom_blocks;
//     mb->mbo_btime = data->body.mbo_btime;
//     mb->mbo_padding_9 = data->body.mbo_padding_9;
//     mb->mbo_padding_10 = data->body.mbo_padding_10;
//     */
// }

static unsigned long send_wr_posted_cnt = 0;
static unsigned long sent_cnt = 0;
static unsigned long pending_cnt = 0;
static unsigned long recv_wr_posted_cnt = 0;
static unsigned long recved_cnt = 0;

static int post_recv_wr(struct rping_cb *cb) {
    struct ibv_recv_wr *bad_recv_wr;
    int ret;

    /* Wait for query */
    cb->state = RDMA_REQ_COMPLETE;
    ret = ibv_post_recv(cb->qp, &cb->rq_wr, &bad_recv_wr);
    recv_wr_posted_cnt++;
    if (ret) {
        fprintf(stderr, "post recv error: %d\n", ret);
        return -1;
    }
    return 0;
}

static int rping_poll_cq(struct rping_cb *cb){
    struct ibv_wc wc;
    int ret;

    DEBUG_LOG("server is waiting a request. send_wr_posted_cnt = %lu recv_wr_posted_cnt=%lu\n",
              send_wr_posted_cnt, recv_wr_posted_cnt);

    // Get one recv CQE.
    while(1){
        DEBUG_LOG("Start polling cq\n");
        while(ibv_poll_cq(cb->cq, 1, &wc) == 0){}
        DEBUG_LOG("End polling cq\n");

        if (wc.status != IBV_WC_SUCCESS){
            printf("Cq completion failed with "
                            "wr_id %Lx status %d opcode %d vender_err %x\n",
                                wc.wr_id, wc.status, wc.opcode, wc.vendor_err);
            return -1;
        }

        if (wc.opcode == IBV_WC_SEND){
            sent_cnt++;
            DEBUG_LOG("Response sent. sent_cnt=%lu\n", sent_cnt);

        } else if (wc.opcode == IBV_WC_RECV){
            recved_cnt++;
            DEBUG_LOG("Server got a request recved_cnt=%lu\n", recved_cnt);
            break;

        } else {
            printf("cq completion wrong op code: not IBV_WC_SEND or IBV_WC_RECV.\n");
            return -1;
        }
    }
    return 0;
}

// static int poll_send_cq(struct rping_cb *cb){
//     struct ibv_wc send_wc;
//     int ret;

//     while((ret = ibv_poll_cq(cb->cq, 1, &send_wc)) == 0){}
//     if (send_wc.status != IBV_WC_SUCCESS){
//         printf("Send cq completion failed with "
//                         "wr_id %Lx status %d opcode %d vender_err %x\n",
//                             send_wc.wr_id, send_wc.status, send_wc.opcode, send_wc.vendor_err);
//         return -1;
//     }
//     if (send_wc.opcode != IBV_WC_SEND){
//         printf("cq completion wrong op code: not IBV_WC_SEND\n");
//     }        
//     sent_cnt++;
//     DEBUG_LOG("Response sent. sent_cnt=%lu\n", sent_cnt);

//     return 0;
// }

// static int poll_recv_cq(struct rping_cb *cb){
//     struct ibv_wc recv_wc;
//     int ret;

//     DEBUG_LOG("server is wating a request. recv_wr_posted_cnt=%lu\n", recv_wr_posted_cnt);
//     while((ret = ibv_poll_cq(cb->cq, 1, &recv_wc)) == 0){}
//     if (recv_wc.status != IBV_WC_SUCCESS){
//         printf("Recv cq completion failed with "
//                         "wr_id %Lx status %d opcode %d vender_err %x\n",
//                             recv_wc.wr_id, recv_wc.status, recv_wc.opcode, recv_wc.vendor_err);
//         return -1;
//     }
//     if (recv_wc.opcode != IBV_WC_RECV){
//         printf("cq completion wrong op code: not IBV_WC_RECV\n");
//     }
//     recved_cnt++;
//     DEBUG_LOG("server got a request recved_cnt=%lu\n", recved_cnt);
//     return 0;
// }

static int post_send_wr(struct rping_cb *cb) {
    struct ibv_send_wr *bad_send_wr;
    int ret;
    /* Send response */
    cb->state = RDMA_REP_COMPLETE;
    ret = ibv_post_send(cb->qp, &cb->sq_wr, &bad_send_wr);
    if (ret) {
        fprintf(stderr, "post send error %d\n", ret);
        return ret;
    }
    send_wr_posted_cnt++;
    DEBUG_LOG("server posted a response. send_wr_posted_cnt=%lu\n", send_wr_posted_cnt);

    return 0;
}

// static int handle_request(struct rping_cb *cb) {
//     int ret;
//     struct lu_fid *fid;

//     /* Display data in recv buf */
//     fid = (struct lu_fid *)&cb->recv_buf.buf;
//     // if (cb->verbose)
//     // printf("server ping data: %s\n", cb->rdma_buf);
//     DEBUG_LOG("fid1.f_seq=%lu fid1.f_old=%u fid1.f_ver=%u recved_cnt=%lu\n",
//               fid->f_seq, fid->f_oid, fid->f_ver, recved_cnt);

//     ///////////////////////////////////////////////////////////////////////////
//     // TODO: To be enabled.
//     // TODO: process the query
//     // struct mdcache_entry *data = mdcache_get("hello");
//     // fill_mb_with_dummy_data((struct mdt_body *)cb->send_buf.buf);
//     ////////////////////////////////////////////////////////////////////////////

//     // TODO: [Optimize] Remove post recv overhead from critical path.
//     // Post a recv WR (request) before sending response.
//     if (ret = post_recv_wr(cb))
//     {
//         fprintf(stderr, "post send wr error %d\n", ret);
//         goto error;
//     }

//     // Post a send WR (response) and poll its CQE.
//     if (ret = post_send_wr(cb))
//     {
//         fprintf(stderr, "post send wr error %d\n", ret);
//         goto error;
//     }

// error:

//     return (cb->state == DISCONNECTED) ? 0 : ret;

// }

// int rdma_server(struct rping_cb *cb, sem_t *t_sem) {
//     int ret;
//     char *recv_buf, *send_buf;
//     struct nvme_passthru_cmd *cmd;
//     struct lu_fid *fid;

//     //srand(time(NULL));

//     while (1) {
//         sem_wait(t_sem);

//         if (cb->state != RDMA_REQ_ADV) {
// 			fprintf(stderr, "(State error) wait for RDMA_REQ_ADV state %d\n",
// 				cb->state);
// 			ret = -1;
// 			break;
// 		}

//         call_lambda(&cb->recv_buf.buf, &cb->send_buf.buf, cb->cb_num);

//         // if (cb->verbose)
//         // printf("server ping data: %s\n", cb->rdma_buf);
//         //DEBUG_LOG("fid1.f_seq=%lu fid1.f_old=%u fid1.f_ver=%u recved_cnt=%lu\n",
//         //    fid->f_seq, fid->f_oid, fid->f_ver, recved_cnt);
//         // Get meta data.
//         //data = mdcache_get(rand() % 10000, h);
//         // 1 memcpy occurs.
//         //fill_mb((struct mdt_body *)cb->send_buf.buf, data);
//         // fill_mb_with_dummy_data((struct mdt_body *)cb->send_buf.buf);

//         // TODO: [Optimize] Remove post recv overhead from critical path.
//         // Post a recv WR (request) before sending response.
//         if (ret = post_recv_wr(cb)){
//             fprintf(stderr, "post send wr error %d\n", ret);
//             break;
//         }

//         // DEBUG_TIME_START(evt_wait_recv_cq);
//         // sem_wait(&cb->sem);
//         // DEBUG_TIME_END(evt_wait_recv_cq);

//         // Post a send WR (response) and poll its CQE.
//         if(ret = post_send_wr(cb)){
//             fprintf(stderr, "post send wr error %d\n", ret);
//             break;
//         }
//         // DEBUG_TIME_START(evt_wait_send_cq);
//         // sem_wait(&cb->sem);
//         // DEBUG_TIME_END(evt_wait_send_cq);
//     }

//     //print_time_stat();

//     return (cb->state == DISCONNECTED) ? 0 : ret;
// }

// static int rping_test_server(struct rping_cb *cb, khash_t(mdcache) *h) {
//     int ret;
//     struct lu_fid *fid;

//     srand(time(NULL));

//     // Post two recv WR.
//     if (post_recv_wr(cb))
//         return -1;

//     while (1) {
//         // Waiting for a request from client.
//         if (ret = rping_poll_cq(cb)){
//             fprintf(stderr, "poll cq error %d\n", ret);
//             break;
//         }

//         /* Display data in recv buf */
//         fid = (struct lu_fid *)&cb->recv_buf.buf;
//         // if (cb->verbose)
//         // printf("server ping data: %s\n", cb->rdma_buf);
//         DEBUG_LOG("fid1.f_seq=%lu fid1.f_old=%u fid1.f_ver=%u recved_cnt=%lu\n",
//             fid->f_seq, fid->f_oid, fid->f_ver, recved_cnt);

//         ///////////////////////////////////////////////////////////////////////////
//         // TODO: To be enabled.
//         // TODO: process the query
//         // struct mdcache_entry *data = mdcache_get("hello");
//         // fill_mb_with_dummy_data((struct mdt_body *)cb->send_buf.buf);
//         ////////////////////////////////////////////////////////////////////////////
//         struct mdcache_entry *data = mdcache_get(rand()%10000, h);

//         // TODO: [Optimize] Do not memcpy.
//         fill_mb((struct mdt_body *)cb->send_buf.buf, data);

//         //fill_mb_with_dummy_data((struct mdt_body *)cb->send_buf.buf);

//         // TODO: [Optimize] Remove post recv overhead from critical path.
//         // Post a recv WR (request) before sending response.
//         if (ret = post_recv_wr(cb)){
//             fprintf(stderr, "post send wr error %d\n", ret);
//             break;
//         }

//         // Post a send WR (response) and poll its CQE.
//         if(ret = post_send_wr(cb)){
//             fprintf(stderr, "post send wr error %d\n", ret);
//             break;
//         }


//     }

//     assert(0);

//     return (cb->state == DISCONNECTED) ? 0 : ret;
// }

static int rping_bind_server(struct rping_cb *cb) {
    int ret;

    if (cb->sin.ss_family == AF_INET)
        ((struct sockaddr_in *) &cb->sin)->sin_port = cb->port;
    else
        ((struct sockaddr_in6 *) &cb->sin)->sin6_port = cb->port;

    ret = rdma_bind_addr(cb->cm_id, (struct sockaddr *) &cb->sin);
    if (ret) {
        perror("rdma_bind_addr");
        return ret;
    }
    DEBUG_LOG("rdma_bind_addr successful\n");

    DEBUG_LOG("rdma_listen\n");
    ret = rdma_listen(cb->cm_id, 3);
    if (ret) {
        perror("rdma_listen");
        return ret;
    }

    return 0;
}

static struct rping_cb *clone_cb(struct rping_cb *listening_cb) {
    struct rping_cb *cb = (struct rping_cb *)malloc(sizeof *cb);
    if (!cb)
        return NULL;
    memset(cb, 0, sizeof *cb);
    *cb = *listening_cb;
    cb->child_cm_id->context = cb;
    return cb;
}

static void free_cb(struct rping_cb *cb) {
    free(cb);
}

int rping_connect_client(struct rping_cb *cb)
{
	struct rdma_conn_param conn_param;
	int ret;

	memset(&conn_param, 0, sizeof conn_param);
	conn_param.responder_resources = 1;
	conn_param.initiator_depth = 1;
	conn_param.retry_count = 7;

	ret = rdma_connect(cb->cm_id, &conn_param);
	if (ret) {
		perror("rdma_connect");
		return ret;
	}

	sem_wait(&cb->sem);
	if (cb->state != CONNECTED) {
		fprintf(stderr, "wait for CONNECTED state %d\n", cb->state);
		return -1;
	}

	DEBUG_LOG("rmda_connect successful\n");
	return 0;
}

int rping_bind_client(struct rping_cb *cb)
{
	int ret;
    struct addrinfo *res;

    ret = getaddrinfo("192.168.14.118", NULL, NULL, &res);
    if (ret) {
        perror("getaddrinfo");
        return ret;
    }
    memcpy(&cb->sin, res->ai_addr, sizeof(struct sockaddr_in));

    freeaddrinfo(res);

	if (cb->sin.ss_family == AF_INET)
		((struct sockaddr_in *) &cb->sin)->sin_port = cb->port;
	else
		((struct sockaddr_in6 *) &cb->sin)->sin6_port = cb->port;

    printf("addr: %u\n", ((struct sockaddr_in *)&cb->sin)->sin_addr.s_addr);
retry:
	ret = rdma_resolve_addr(cb->cm_id, NULL, (struct sockaddr *) &cb->sin, 2000);
	if (ret) {
		//perror("rdma_resolve_addr");
		//return ret;
        goto retry;
	}

	sem_wait(&cb->sem);
	if (cb->state != ROUTE_RESOLVED) {
		fprintf(stderr, "waiting for addr/route resolution state %d\n",
			cb->state);
		return -1;
	}

	DEBUG_LOG("rdma_resolve_addr - rdma_resolve_route successful\n");
	return 0;
}

// static void *rping_test_server_thread(void *arg) {
//     struct rping_cb *cb = (struct rping_cb *)arg;
//     rping_test_server(cb);

//     return NULL;
// }

void *t_rping_run_server(void *arg) {
    struct ibv_recv_wr *bad_wr;
    int ret, i;
    // struct mdcache_entry *data = new mdcache_entry;
    // khash_t(mdcache) *h;
    struct rping_cb *cb = (struct rping_cb *)arg;
    struct t_cb *thread_cb;
    sem_t *t_sem;

    thread_cb = (struct t_cb *)malloc(sizeof(struct t_cb));

    t_sem = (sem_t *)malloc(sizeof(sem_t));
    sem_init(t_sem, 0, 0);


    DEBUG_LOG("rping_bind_server before\n");
    ret = rping_bind_client(cb);
    DEBUG_LOG("rping_bind_server after\n");

    // sem_wait(&cb->sem);
    // if (cb->state != CONNECT_REQUEST) {
    //     fprintf(stderr, "wait for CONNECT_REQUEST state %d\n",
    //         cb->state);
    // }

    DEBUG_LOG("rping_setup_qp before\n");
    ret = rping_setup_qp(cb, cb->cm_id);
    if (ret) {
        fprintf(stderr, "setup_qp failed: %d\n", ret);
    }
    DEBUG_LOG("rping_setup_qp after\n");
    ret = rping_setup_buffers(cb);
    if (ret) {
        fprintf(stderr, "rping_setup_buffers failed: %d\n", ret);
        //goto err1;
    }

    printf("before post_recv\n");
    printf("bufsize: %llu\n", BUF_SIZE);

    // struct ibv_recv_wr ex_wr;
    // ex_wr.sg_list = &cb->recv_sgl;
    // ex_wr.num_sge = 1;

    // // Event-driven scheduling.////////////////////////
    ret = ibv_post_recv(cb->qp, &cb->rq_wr, &bad_wr);
    //ret = ibv_post_recv(cb->qp, &ex_wr, &bad_wr);
    if (ret) {
        fprintf(stderr, "ibv_post_recv failed: %d\n", ret);
        //goto err2;
    }
    DEBUG_LOG("recv posted\n");
    printf("after post_recv\n");

    thread_cb->cb = cb;
    thread_cb->t_sem = t_sem;

    ret = pthread_create(&cb->cqthread, NULL, cq_thread, thread_cb);
    if (ret) {
        perror("pthread_create");
        //goto err2;
    }
    ////////////////////////////////////////////

    // DEBUG_LOG("Press enter to accept:");
    // getchar();

    // ret = rping_accept(cb);
    // if (ret) {
    //     fprintf(stderr, "connect error %d\n", ret);
    //     goto err2;
    // }

    ret = rping_connect_client(cb);
	if (ret) {
		fprintf(stderr, "connect error %d\n", ret);
		//goto err3;
	}

    printf("new rdma connection %d\n", cb->cb_num);

    // DEBUG_LOG("Press enter to test server:");
    // getchar();

/*
    // Multiple server threads.
    for (i = 0; i < THREAD_NUM; i++){
        ret = pthread_create(&th_pool[i], NULL, rping_test_server_thread, cb);
        printf("thread %d created.\n", i);
        if (ret)
        {
            fprintf(stderr, "thread init error %d\n", ret);
            goto err2;
        }
    }

    for (i = 0; i < THREAD_NUM; i++){
        pthread_join(th_pool[i], NULL);
    }
    */

    // single thread.
    // h = mdcache_init();
    // fill_mb_with_dummy_data(&(data->body)); // TODO: load files into hash table later
    // for (i = 0; i < 10000; i++)
    //     mdcache_insert(i, data, h);
    // ret = rping_test_server(cb, h);
    // ret = rdma_server(cb, t_sem);
    // if (ret) {
    //     fprintf(stderr, "rping server failed: %d\n", ret);
    //     goto err3;
    // }

    const struct ibv_send_wr *bad_send_wr;
    const struct ibv_recv_wr *bad_recv_wr;
    struct ibv_wc send_wc;
    struct ibv_wc recv_wc;
    uint32_t recv_size;

    while (1) {
        //sem_wait(t_sem);

        //nlh = (struct nlmsghdr *)malloc(NLMSG_SPACE(MAX_PAYLOAD));
        char test[2105344];
        for (int i=0; i<2105344; i++){
            if (i > 1024)
                test[i] = 'b';
            else
                test[i] = 'a';
            //test[i] = 'a';
        }

        printf("message received\n");

        //cb->send_sgl.addr = NLMSG_DATA(nlh);
        //cb->send_sgl.length = nlh->nlmsg_len - NLMSG_SPACE(0);
        //cb->send_sgl.lkey = 0;
        cb->send_sgl.length = 4096;

        memcpy(cb->send_buf.buf, test, 4096);
        //memcpy(cb->send_buf.buf, test, BUF_SIZE);
        //cb->send_sgl.length = nlh->nlmsg_len - NLMSG_SPACE(0);

        //printf("send_sgl.addr: %llu send_sgl.length: %llu\n", cb->send_sgl.addr,cb->send_sgl.length);

        //cb->sq_wr.opcode = IBV_WR_SEND;
        //cb->sq_wr.send_flags = IBV_SEND_INLINE|IBV_SEND_SIGNALED;
        //cb->sq_wr.send_flags = IBV_SEND_SIGNALED;
        //cb->sq_wr.sg_list = &cb->send_sgl;
        //cb->sq_wr.num_sge = 1;
        cb->state = RDMA_REQ_ADV;

        ret = ibv_post_send(cb->qp, &cb->sq_wr, &bad_send_wr);
        if (ret){
            printf("post send error %d\n", ret);
            break;
        }
        send_wr_posted_cnt++;
        pending_cnt++;

        printf("rdma message sent\n");

        cb->state = RDMA_REP_ADV;

        ret = ibv_post_recv(cb->qp, &cb->rq_wr, &bad_recv_wr);

        recv_wr_posted_cnt++;

        while ((ret = ibv_poll_cq(cb->cq, 1, &send_wc)) == 0){}

        if (send_wc.status != IBV_WC_SUCCESS){
            printf("Send cq completion failed with "
                            "wr_id %Lx status %d opcode %d vender_err %x\n",
                                send_wc.wr_id, send_wc.status, send_wc.opcode, send_wc.vendor_err);
            break;
        }
        if (send_wc.opcode != IBV_WC_SEND){
            printf("cq completion wrong op code: not IBV_WC_SEND\n");
        }

        while ((ret = ibv_poll_cq(cb->cq, 1, &recv_wc)) == 0){}

        if (recv_wc.status != IBV_WC_SUCCESS){
            printf("Recv cq completion failed with "
                            "wr_id %Lx status %d opcode %d vender_err %x\n",
                                recv_wc.wr_id, recv_wc.status, recv_wc.opcode, recv_wc.vendor_err);
            break;
        }
        if (recv_wc.opcode != IBV_WC_RECV){
            printf("cq completion wrong op code: not IBV_WC_RECV\n");
        }

        char recv[2105344];

        // KOO TODO: How do I know the receive size? size_o of the struct nvme_meta
        memcpy(recv, cb->recv_buf.buf, BUF_SIZE);

        printf("message received: %c %c\n", recv[0], recv[BUF_SIZE-1]);
        // if(ret = post_send_wr(cb)){
        //     fprintf(stderr, "post send wr error %d\n", ret);
        //     break;
        // }
        break;
    }

    ret = 0;
// //err3:
//     rping_disconnect(cb, cb->child_cm_id);
//     //pthread_join(cb->cqthread, NULL);
//     rdma_destroy_id(cb->child_cm_id);
// //err2:
//     rping_free_buffers(cb);
// //err1:
//     rping_free_qp(cb);
//     rdma_destroy_id(cb->cm_id);
//     rdma_destroy_event_channel(cb->cm_channel);
//     free(cb);
}

// int rping_run_server(struct rping_cb *cb) {
//     struct ibv_recv_wr *bad_wr;
//     int ret, i;
//     struct mdcache_entry *data = new mdcache_entry;
//     khash_t(mdcache) *h;

//     ret = rping_bind_server(cb);
//     sem_wait(&cb->sem);
//     if (cb->state != CONNECT_REQUEST) {
//         fprintf(stderr, "wait for CONNECT_REQUEST state %d\n",
//             cb->state);
//     }

//     ret = rping_setup_qp(cb, cb->child_cm_id);
//     if (ret) {
//         fprintf(stderr, "setup_qp failed: %d\n", ret);
//     }
//     ret = rping_setup_buffers(cb);
//     if (ret) {
//         fprintf(stderr, "rping_setup_buffers failed: %d\n", ret);
//         goto err1;
//     }

//     // Event-driven scheduling.////////////////////////
//     ret = ibv_post_recv(cb->qp, &cb->rq_wr, &bad_wr);
//     if (ret) {
//         fprintf(stderr, "ibv_post_recv failed: %d\n", ret);
//         goto err2;
//     }

//     //ret = pthread_create(&cb->cqthread, NULL, cq_thread, cb);
//     //if (ret) {
//     //    perror("pthread_create");
//     //    goto err2;
//     //}
//     ////////////////////////////////////////////

//     // DEBUG_LOG("Press enter to accept:");
//     // getchar();

//     ret = rping_accept(cb);
//     if (ret) {
//         fprintf(stderr, "connect error %d\n", ret);
//         goto err2;
//     }

//     // DEBUG_LOG("Press enter to test server:");
//     // getchar();

// /*
//     // Multiple server threads.
//     for (i = 0; i < THREAD_NUM; i++){
//         ret = pthread_create(&th_pool[i], NULL, rping_test_server_thread, cb);
//         printf("thread %d created.\n", i);
//         if (ret)
//         {
//             fprintf(stderr, "thread init error %d\n", ret);
//             goto err2;
//         }
//     }

//     for (i = 0; i < THREAD_NUM; i++){
//         pthread_join(th_pool[i], NULL);
//     }
//     */

//     // single thread.
//     h = mdcache_init();
//     //fill_mb_with_dummy_data(&(data->body)); // TODO: load files into hash table later
//     for (i = 0; i < 10000; i++)
//         mdcache_insert(i, data, h);
//     //ret = rping_test_server(cb, h);
//     ret = mdcache_server(cb);
//     if (ret) {
//         fprintf(stderr, "rping server failed: %d\n", ret);
//         goto err3;
//     }

//     ret = 0;
// err3:
//     rping_disconnect(cb, cb->child_cm_id);
//     //pthread_join(cb->cqthread, NULL);
//     rdma_destroy_id(cb->child_cm_id);
// err2:
//     rping_free_buffers(cb);
// err1:
//     rping_free_qp(cb);
//     rdma_destroy_id(cb->cm_id);
//     rdma_destroy_event_channel(cb->cm_channel);
//     free(cb);
// }

int get_addr(char *dst, struct sockaddr *addr) {
    struct addrinfo *res;
    int ret;

    ret = getaddrinfo(dst, NULL, NULL, &res);
    if (ret) {
        printf("getaddrinfo failed (%s) - invalid hostname or IP address\n", gai_strerror(ret));
        return ret;
    }

    if (res->ai_family == PF_INET)
        memcpy(addr, res->ai_addr, sizeof(struct sockaddr_in));
    else if (res->ai_family == PF_INET6)
        memcpy(addr, res->ai_addr, sizeof(struct sockaddr_in6));
    else
        ret = -1;

    freeaddrinfo(res);
    return ret;
}

static void usage(const char *name) {
    printf("%s -s [-vVd] [-S size] [-C count] [-a addr] [-p port]\n",
           basename(name));
    printf("%s -c [-vVd] [-S size] [-C count] [-I addr] -a addr [-p port]\n",
           basename(name));
    printf("\t-I\t\tSource address to bind to for client.\n");
    printf("\t-s\t\tserver side.  To bind to any address with IPv6 use -a ::0\n");
    printf("\t-v\t\tdisplay ping data to stdout\n");
    printf("\t-V\t\tvalidate ping data\n");
    printf("\t-d\t\tdebug printfs\n");
    printf("\t-S size \tping data size\n");
    printf("\t-C count\tping count times\n");
    printf("\t-a addr\t\taddress\n");
    printf("\t-p port\t\tport\n");
    printf("\t-q\t\tuse self-created, self-modified QP\n");
}

int
main(int argc, char **argv)
{
	int rc;

	struct rping_cb *cb[CB_THREAD];
    int op;
    int ret = 0;
    int persistent_server = 0;
    int i;
    pthread_t thread[CB_THREAD];


    // // KOO TODO: To check device capabilities.
    // // Get list of all RDMA devices
    // struct ibv_device **dev_list;
    // struct ibv_context *context;
    // struct ibv_port_attr port_attr;

    // // Get list of all RDMA devices
    // dev_list = ibv_get_device_list(NULL);
    // if (!dev_list) {
    //     perror("Failed to get RDMA devices list");
    //     return 1;
    // }

    // // Assume we use the first device (for simplicity)
    // context = ibv_open_device(dev_list[0]);
    // if (!context) {
    //     perror("Failed to open device");
    //     ibv_free_device_list(dev_list);
    //     return 1;
    // }

    // // Query the port attributes of the first port (port_num = 1)
    // ret = ibv_query_port(context, 1, &port_attr);
    // if (ret != 0) {
    //     perror("Failed to query port");
    //     ibv_close_device(context);
    //     ibv_free_device_list(dev_list);
    //     return 1;
    // }

    // // Print the maximum message size
    // printf("Maximum message size supported by the port: %u\n", port_attr.max_mtu);
    // printf("active message size supported by the port: %u\n", port_attr.active_mtu);


    for (i=0; i<CB_THREAD; i++){
    	cb[i] = (struct rping_cb *)malloc(sizeof(struct rping_cb));
    	if (!cb[i])
        	return -ENOMEM;

    	memset(cb[i], 0, sizeof(struct rping_cb));
        cb[i]->cb_num = i;
    	cb[i]->server = 0;
    	cb[i]->state = IDLE;
    	// cb->size = 64;
    	cb[i]->size = sizeof(struct rping_rdma_info);
    	cb[i]->sin.ss_family = AF_INET;
    	cb[i]->port = htobe16(7174+i+1);
    	sem_init(&cb[i]->sem, 0, 0);
    }

    // h = mdcache_init();
    // fill_mb_with_dummy_data(&(data->body)); // TODO: load files into hash table later
    // for (i = 0; i < 10000; i++)
    //     mdcache_insert(i, data, h);

    for (i=0; i<CB_THREAD; i++){
    	cb[i]->cm_channel = create_first_event_channel();
    	if (!cb[i]->cm_channel) {
        	ret = errno;
        	goto out;
    	}

    	ret = rdma_create_id(cb[i]->cm_channel, &(cb[i]->cm_id), cb[i], RDMA_PS_TCP);
    	if (ret) {
        	perror("rdma_create_id");
        	goto out2;
    	}
    	DEBUG_LOG("created cm_id %p\n", cb[i]->cm_id);

    	ret = pthread_create(&cb[i]->cmthread, NULL, cm_thread, cb[i]);
    	if (ret) {
        	perror("pthread_create");
        	goto out2;
    	}

//	if (i == 0)
//    		ret = rping_run_server(cb[i]);
//	else
		ret = pthread_create(&thread[i], NULL, t_rping_run_server, cb[i]);

    	//DEBUG_LOG("destroy cm_id %p\n", cb[i]->cm_id);
    	//rdma_destroy_id(cb[i]->cm_id);
    }
    for (i=0; i<CB_THREAD; i++){
		pthread_join(thread[i], NULL);
	}

    for (i=0; i<CB_THREAD; i++){
        rping_free_buffers(cb[i]);
        rping_disconnect(cb[i], cb[i]->cm_id);
        rping_free_qp(cb[i]);
        rdma_destroy_id(cb[i]->cm_id);
        rdma_destroy_event_channel(cb[i]->cm_channel);
        free(cb[i]);
    }
out2:
    //for (i=0; i<CB_THREAD; i++)
    	//rdma_destroy_event_channel(cb[i]->cm_channel);
out:
    //for (i=0; i<CB_THREAD; i++)
    	//free(cb[i]);
    
	// while (1)
	//     sleep(1000);

	return 0;
}
