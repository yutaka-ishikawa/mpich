/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil ; -*- */
/*
 *  (C) 2019 by Argonne National Laboratory.
 *      See COPYRIGHT in top-level directory.
 *
 *  Portions of this code were written by Intel Corporation.
 *  Copyright (C) 2011-2016 Intel Corporation.  Intel provides this material
 *  to Argonne National Laboratory subject to Software Grant and Corporate
 *  Contributor License Agreement dated February 8, 2012.
 */

#include "mpidimpl.h"
#include "mpidch4r.h"
#include "ch4r_callbacks.h"

static int handle_unexp_cmpl(MPIR_Request * rreq);
static int do_send_target(void **data, size_t * p_data_sz, int *is_contig,
                          MPIDIG_am_target_cmpl_cb * target_cmpl_cb, MPIR_Request * rreq);
static int recv_target_cmpl_cb(MPIR_Request * rreq);

/* Checks to make sure that the specified request is the next one expected to finish. If it isn't
 * supposed to finish next, it is appended to a list of requests to be retrieved later. */
int MPIDIG_check_cmpl_order(MPIR_Request * req, MPIDIG_am_target_cmpl_cb target_cmpl_cb)
{
    int ret = 0;

    MPIR_FUNC_VERBOSE_STATE_DECL(MPID_STATE_MPIDIG_CHECK_CMPL_ORDER);
    MPIR_FUNC_VERBOSE_ENTER(MPID_STATE_MPIDIG_CHECK_CMPL_ORDER);

    if (MPIDIG_REQUEST(req, req->seq_no) == (uint64_t) OPA_load_int(&MPIDI_global.exp_seq_no)) {
        OPA_incr_int(&MPIDI_global.exp_seq_no);
        ret = 1;
        goto fn_exit;
    }

    MPIDIG_REQUEST(req, req->target_cmpl_cb) = (void *) target_cmpl_cb;
    MPIDIG_REQUEST(req, req->request) = (uint64_t) req;
    /* MPIDI_CS_ENTER(); */
    DL_APPEND(MPIDI_global.cmpl_list, req->dev.ch4.am.req);
    /* MPIDI_CS_EXIT(); */

  fn_exit:
    MPIR_FUNC_VERBOSE_EXIT(MPID_STATE_MPIDIG_CHECK_CMPL_ORDER);
    return ret;
}

void MPIDIG_progress_compl_list(void)
{
    MPIR_Request *req;
    MPIDIG_req_ext_t *curr, *tmp;
    MPIDIG_am_target_cmpl_cb target_cmpl_cb;

    MPIR_FUNC_VERBOSE_STATE_DECL(MPID_STATE_MPIDIG_PROGRESS_CMPL_LIST);
    MPIR_FUNC_VERBOSE_ENTER(MPID_STATE_MPIDIG_PROGRESS_CMPL_LIST);

    /* MPIDI_CS_ENTER(); */
  do_check_again:
    DL_FOREACH_SAFE(MPIDI_global.cmpl_list, curr, tmp) {
        if (curr->seq_no == (uint64_t) OPA_load_int(&MPIDI_global.exp_seq_no)) {
            DL_DELETE(MPIDI_global.cmpl_list, curr);
            req = (MPIR_Request *) curr->request;
            target_cmpl_cb = (MPIDIG_am_target_cmpl_cb) curr->target_cmpl_cb;
            target_cmpl_cb(req);
            goto do_check_again;
        }
    }
    /* MPIDI_CS_EXIT(); */
    MPIR_FUNC_VERBOSE_EXIT(MPID_STATE_MPIDIG_PROGRESS_CMPL_LIST);
}

static int handle_unexp_cmpl(MPIR_Request * rreq)
{
    int mpi_errno = MPI_SUCCESS, in_use;
    MPIR_Comm *root_comm;
    MPIR_Request *match_req = NULL;
    size_t nbytes;
    int dt_contig;
    MPI_Aint dt_true_lb;
    MPIR_Datatype *dt_ptr;
    size_t dt_sz;

#ifndef MPIDI_CH4_DIRECT_NETMOD
    MPIR_Request *anysource_partner = NULL;
#endif

    MPIR_FUNC_VERBOSE_STATE_DECL(MPID_STATE_MPIDIG_HANDLE_UNEXP_CMPL);
    MPIR_FUNC_VERBOSE_ENTER(MPID_STATE_MPIDIG_HANDLE_UNEXP_CMPL);

    /* Check if this message has already been claimed by a probe. */
    /* MPIDI_CS_ENTER(); */
    if (MPIDIG_REQUEST(rreq, req->status) & MPIDIG_REQ_UNEXP_DQUED) {
        if (MPIDIG_REQUEST(rreq, req->status) & MPIDIG_REQ_UNEXP_CLAIMED) {
            MPIDIG_handle_unexp_mrecv(rreq);
        }
        /* MPIDI_CS_EXIT(); */
        goto fn_exit;
    }
    /* MPIDI_CS_EXIT(); */

    root_comm = MPIDIG_context_id_to_comm(MPIDIG_REQUEST(rreq, context_id));

    /* If this request was previously matched, but not handled */
    if (MPIDIG_REQUEST(rreq, req->status) & MPIDIG_REQ_MATCHED) {
        match_req = (MPIR_Request *) MPIDIG_REQUEST(rreq, req->rreq.match_req);

#ifndef MPIDI_CH4_DIRECT_NETMOD
        if (unlikely(match_req && MPIDI_REQUEST_ANYSOURCE_PARTNER(match_req))) {
            anysource_partner = MPIDI_REQUEST_ANYSOURCE_PARTNER(match_req);
            if (!MPIR_STATUS_GET_CANCEL_BIT(anysource_partner->status)) {
                mpi_errno = MPID_Cancel_recv(anysource_partner);
                if (mpi_errno != MPI_SUCCESS) {
                    goto fn_fail;
                }
                /* What should we do if the anysource partner request is not canceled? */
                MPIR_Assertp(MPIR_STATUS_GET_CANCEL_BIT(anysource_partner->status));
            }
            MPIR_Request_free(MPIDI_REQUEST_ANYSOURCE_PARTNER(match_req));
            MPIDI_REQUEST_ANYSOURCE_PARTNER(match_req) = NULL;
            MPIDI_REQUEST_ANYSOURCE_PARTNER(anysource_partner) = NULL;
        }
#endif /* MPIDI_CH4_DIRECT_NETMOD */

    } else {
        /* If this message hasn't been matched yet, look for it in the posted queue. */
        /* MPIDI_CS_ENTER(); */
        if (root_comm) {
#ifdef MPIDI_CH4_DIRECT_NETMOD
            match_req =
                MPIDIG_dequeue_posted(MPIDIG_REQUEST(rreq, rank),
                                      MPIDIG_REQUEST(rreq, tag),
                                      MPIDIG_REQUEST(rreq, context_id),
                                      &MPIDIG_COMM(root_comm, posted_list));
#else /* MPIDI_CH4_DIRECT_NETMOD */
            int continue_matching = 1;
            while (continue_matching) {
                match_req =
                    MPIDIG_dequeue_posted(MPIDIG_REQUEST(rreq, rank),
                                          MPIDIG_REQUEST(rreq, tag),
                                          MPIDIG_REQUEST(rreq, context_id),
                                          &MPIDIG_COMM(root_comm, posted_list));

                if (match_req && MPIDI_REQUEST_ANYSOURCE_PARTNER(match_req)) {
                    anysource_partner = MPIDI_REQUEST_ANYSOURCE_PARTNER(match_req);

                    mpi_errno = MPIDI_anysource_matched(anysource_partner,
                                                        MPIDI_REQUEST(rreq, is_local) ?
                                                        MPIDI_SHM : MPIDI_NETMOD,
                                                        &continue_matching);

                    if (MPI_SUCCESS != mpi_errno)
                        MPIR_ERR_POP(mpi_errno);

                    MPIR_Request_free(MPIDI_REQUEST_ANYSOURCE_PARTNER(match_req));
                    MPIDI_REQUEST_ANYSOURCE_PARTNER(match_req) = NULL;
                    MPIDI_REQUEST_ANYSOURCE_PARTNER(anysource_partner) = NULL;
                }

                break;
            }
#endif /* MPIDI_CH4_DIRECT_NETMOD */
        }

        /* If we found a matching request, remove it from the unexpected queue and clean things up
         * before we move the data around. */
        if (match_req) {
            MPIDIG_delete_unexp(rreq, &MPIDIG_COMM(root_comm, unexp_list));
            /* Decrement the counter twice, one for posted_list and the other for unexp_list */
            MPIR_Comm_release(root_comm);
            MPIR_Comm_release(root_comm);
        }
        /* MPIDI_CS_EXIT(); */
    }

    /* If we didn't match the request, unmark the busy bit and skip the data movement below. */
    if (!match_req) {
        MPIDIG_REQUEST(rreq, req->status) &= ~MPIDIG_REQ_BUSY;
        goto fn_exit;
    }

    match_req->status.MPI_SOURCE = MPIDIG_REQUEST(rreq, rank);
    match_req->status.MPI_TAG = MPIDIG_REQUEST(rreq, tag);

    /* Figure out how much data needs to be moved. */
    MPIDI_Datatype_get_info(MPIDIG_REQUEST(match_req, count),
                            MPIDIG_REQUEST(match_req, datatype),
                            dt_contig, dt_sz, dt_ptr, dt_true_lb);
    MPIR_Datatype_get_size_macro(MPIDIG_REQUEST(match_req, datatype), dt_sz);

    /* Make sure this request has the right amount of data in it. */
    if (MPIDIG_REQUEST(rreq, count) > dt_sz * MPIDIG_REQUEST(match_req, count)) {
        rreq->status.MPI_ERROR = MPI_ERR_TRUNCATE;
        nbytes = dt_sz * MPIDIG_REQUEST(match_req, count);
    } else {
        rreq->status.MPI_ERROR = MPI_SUCCESS;
        nbytes = MPIDIG_REQUEST(rreq, count);   /* incoming message is always count of bytes. */
    }

    MPIR_STATUS_SET_COUNT(match_req->status, nbytes);
    MPIDIG_REQUEST(rreq, count) = dt_sz > 0 ? nbytes / dt_sz : 0;

    /* Perform the data copy (using the datatype engine if necessary for non-contig transfers) */
    if (!dt_contig) {
        MPI_Aint actual_unpack_bytes;
        mpi_errno = MPIR_Typerep_unpack(MPIDIG_REQUEST(rreq, buffer), nbytes,
                                        MPIDIG_REQUEST(match_req, buffer),
                                        MPIDIG_REQUEST(match_req, count),
                                        MPIDIG_REQUEST(match_req, datatype), 0,
                                        &actual_unpack_bytes);
        if (mpi_errno)
            MPIR_ERR_POP(mpi_errno);

        if (actual_unpack_bytes != (MPI_Aint) nbytes) {
            mpi_errno = MPIR_Err_create_code(MPI_SUCCESS, MPIR_ERR_RECOVERABLE,
                                             __FUNCTION__, __LINE__,
                                             MPI_ERR_TYPE, "**dtypemismatch", 0);
            match_req->status.MPI_ERROR = mpi_errno;
        }
    } else {
        MPIR_Memcpy((char *) MPIDIG_REQUEST(match_req, buffer) + dt_true_lb,
                    MPIDIG_REQUEST(rreq, buffer), nbytes);
    }

    /* Now that the unexpected message has been completed, unset the status bit. */
    MPIDIG_REQUEST(rreq, req->status) &= ~MPIDIG_REQ_UNEXPECTED;

    /* If this is a synchronous send, send the reply back to the sender to unlock them. */
    if (MPIDIG_REQUEST(rreq, req->status) & MPIDIG_REQ_PEER_SSEND) {
        mpi_errno = MPIDIG_reply_ssend(rreq);
        if (mpi_errno)
            MPIR_ERR_POP(mpi_errno);
    }
#ifndef MPIDI_CH4_DIRECT_NETMOD
    if (unlikely(anysource_partner)) {
        anysource_partner->status = match_req->status;
    }
#endif /* MPIDI_CH4_DIRECT_NETMOD */

    MPIR_Datatype_release_if_not_builtin(MPIDIG_REQUEST(match_req, datatype));
    MPL_free(MPIDIG_REQUEST(rreq, buffer));
    MPIR_Object_release_ref(rreq, &in_use);
    MPID_Request_complete(rreq);
    MPID_Request_complete(match_req);
  fn_exit:
    MPIR_FUNC_VERBOSE_EXIT(MPID_STATE_MPIDIG_HANDLE_UNEXP_CMPL);
    return mpi_errno;
  fn_fail:
    goto fn_exit;
}


static int do_send_target(void **data, size_t * p_data_sz, int *is_contig,
                          MPIDIG_am_target_cmpl_cb * target_cmpl_cb, MPIR_Request * rreq)
{
    int dt_contig;
    MPI_Aint dt_true_lb, num_iov;
    MPIR_Datatype *dt_ptr;
    size_t data_sz;

    MPIR_FUNC_VERBOSE_STATE_DECL(MPID_STATE_MPIDIG_DO_SEND_TARGET);
    MPIR_FUNC_VERBOSE_ENTER(MPID_STATE_MPIDIG_DO_SEND_TARGET);

    *target_cmpl_cb = recv_target_cmpl_cb;
    MPIDIG_REQUEST(rreq, req->seq_no) = OPA_fetch_and_add_int(&MPIDI_global.nxt_seq_no, 1);

    if (p_data_sz == NULL || 0 == MPIDIG_REQUEST(rreq, count))
        return MPI_SUCCESS;

    MPIDI_Datatype_get_info(MPIDIG_REQUEST(rreq, count), MPIDIG_REQUEST(rreq, datatype), dt_contig,
                            data_sz, dt_ptr, dt_true_lb);
    *is_contig = dt_contig;

    if (dt_contig) {
        *p_data_sz = data_sz;
        *data = (char *) MPIDIG_REQUEST(rreq, buffer) + dt_true_lb;
    } else {
        if (*p_data_sz > data_sz) {
            rreq->status.MPI_ERROR = MPI_ERR_TRUNCATE;
            *p_data_sz = data_sz;
        }

        MPIR_Typerep_iov_len(MPIDIG_REQUEST(rreq, buffer), MPIDIG_REQUEST(rreq, count),
                             MPIDIG_REQUEST(rreq, datatype), 0, data_sz, &num_iov);

        MPIR_Assert(num_iov > 0);
        MPIDIG_REQUEST(rreq, req->iov) =
            (struct iovec *) MPL_malloc(num_iov * sizeof(struct iovec), MPL_MEM_BUFFER);
        MPIR_Assert(MPIDIG_REQUEST(rreq, req->iov));

        int actual_iov_len;
        MPI_Aint actual_iov_bytes;
        MPIR_Typerep_to_iov(MPIDIG_REQUEST(rreq, buffer), MPIDIG_REQUEST(rreq, count),
                            MPIDIG_REQUEST(rreq, datatype), 0, MPIDIG_REQUEST(rreq, req->iov),
                            (int) num_iov, *p_data_sz, &actual_iov_len, &actual_iov_bytes);

        if (actual_iov_bytes != (MPI_Aint) * p_data_sz) {
            rreq->status.MPI_ERROR = MPI_ERR_TYPE;
        }
        *data = MPIDIG_REQUEST(rreq, req->iov);
        *p_data_sz = actual_iov_len;
        MPIDIG_REQUEST(rreq, req->status) |= MPIDIG_REQ_RCV_NON_CONTIG;
    }

    MPIR_FUNC_VERBOSE_EXIT(MPID_STATE_MPIDIG_DO_SEND_TARGET);
    return MPI_SUCCESS;
}

/* This function is called when a receive has completed on the receiver side. The input is the
 * request that has been completed. */
static int recv_target_cmpl_cb(MPIR_Request * rreq)
{
    int mpi_errno = MPI_SUCCESS;

    MPIR_FUNC_VERBOSE_STATE_DECL(MPID_STATE_MPIDIG_RECV_TARGET_CMPL_CB);
    MPIR_FUNC_VERBOSE_ENTER(MPID_STATE_MPIDIG_RECV_TARGET_CMPL_CB);

    /* Check if this request is supposed to complete next or if it should be delayed. */
    if (!MPIDIG_check_cmpl_order(rreq, recv_target_cmpl_cb))
        return mpi_errno;

    /* If the request contained noncontiguous data, free the iov array that described it. */
    if (MPIDIG_REQUEST(rreq, req->status) & MPIDIG_REQ_RCV_NON_CONTIG) {
        MPL_free(MPIDIG_REQUEST(rreq, req->iov));
    }

    if (MPIDIG_REQUEST(rreq, req->status) & MPIDIG_REQ_UNEXPECTED) {
        mpi_errno = handle_unexp_cmpl(rreq);
        if (mpi_errno)
            MPIR_ERR_POP(mpi_errno);
        goto fn_exit;
    }

    rreq->status.MPI_SOURCE = MPIDIG_REQUEST(rreq, rank);
    rreq->status.MPI_TAG = MPIDIG_REQUEST(rreq, tag);

    if (MPIDIG_REQUEST(rreq, req->status) & MPIDIG_REQ_PEER_SSEND) {
        mpi_errno = MPIDIG_reply_ssend(rreq);
        if (mpi_errno)
            MPIR_ERR_POP(mpi_errno);
    }
#ifndef MPIDI_CH4_DIRECT_NETMOD
    if (MPIDI_REQUEST_ANYSOURCE_PARTNER(rreq)) {
        int continue_matching = 1;
        if (MPIDI_REQUEST(rreq, is_local)) {
            MPIDI_anysource_matched(MPIDI_REQUEST_ANYSOURCE_PARTNER(rreq), MPIDI_SHM,
                                    &continue_matching);
        } else {
            MPIDI_anysource_matched(MPIDI_REQUEST_ANYSOURCE_PARTNER(rreq),
                                    MPIDI_NETMOD, &continue_matching);
        }

        MPIR_Request_free(MPIDI_REQUEST_ANYSOURCE_PARTNER(rreq));
        if (unlikely(MPIDI_REQUEST_ANYSOURCE_PARTNER(rreq))) {
            MPIDI_REQUEST_ANYSOURCE_PARTNER(MPIDI_REQUEST_ANYSOURCE_PARTNER(rreq)) = NULL;
            MPIDI_REQUEST_ANYSOURCE_PARTNER(rreq) = NULL;
        }
        /* In case of anysource recieve, a request object generated by
         * the shmmod is always returned to the user. Thus, the user-level
         * request cleanup functions (e.g. MPI_Wait) will only free the
         * request object from shmmod. Therefore, netmod should decrement
         * its request object by itself, otherwise it will leak.
         *
         * This logic follows what the OFI netmod is already doing (see
         * MPIDI_OFI_recv_event.) */
        MPIR_Request_free(rreq);
    }
#endif

    MPIR_Datatype_release_if_not_builtin(MPIDIG_REQUEST(rreq, datatype));
    MPID_Request_complete(rreq);
  fn_exit:
    MPIDIG_progress_compl_list();
    MPIR_FUNC_VERBOSE_EXIT(MPID_STATE_MPIDIG_RECV_TARGET_CMPL_CB);
    return mpi_errno;
  fn_fail:
    goto fn_exit;
}

int MPIDIG_send_origin_cb(MPIR_Request * sreq)
{
    int mpi_errno = MPI_SUCCESS;
    MPIR_FUNC_VERBOSE_STATE_DECL(MPID_STATE_MPIDIG_SEND_ORIGIN_CB);
    MPIR_FUNC_VERBOSE_ENTER(MPID_STATE_MPIDIG_SEND_ORIGIN_CB);
    MPID_Request_complete(sreq);
    MPIR_FUNC_VERBOSE_EXIT(MPID_STATE_MPIDIG_SEND_ORIGIN_CB);
    return mpi_errno;
}

int MPIDIG_send_long_lmt_origin_cb(MPIR_Request * sreq)
{
    int mpi_errno = MPI_SUCCESS;
    MPIR_FUNC_VERBOSE_STATE_DECL(MPID_STATE_MPIDIG_SEND_LONG_LMT_ORIGIN_CB);
    MPIR_FUNC_VERBOSE_ENTER(MPID_STATE_MPIDIG_SEND_LONG_LMT_ORIGIN_CB);
    MPIR_Datatype_release_if_not_builtin(MPIDIG_REQUEST(sreq, req->lreq).datatype);
    MPID_Request_complete(sreq);
    MPIR_FUNC_VERBOSE_EXIT(MPID_STATE_MPIDIG_SEND_LONG_LMT_ORIGIN_CB);
    return mpi_errno;
}

int MPIDIG_ssend_ack_origin_cb(MPIR_Request * req)
{
    int mpi_errno = MPI_SUCCESS;
    MPIR_FUNC_VERBOSE_STATE_DECL(MPID_STATE_MPIDIG_SSEND_ACK_ORIGIN_CB);
    MPIR_FUNC_VERBOSE_ENTER(MPID_STATE_MPIDIG_SSEND_ACK_ORIGIN_CB);
    MPID_Request_complete(req);
    MPIR_FUNC_VERBOSE_EXIT(MPID_STATE_MPIDIG_SSEND_ACK_ORIGIN_CB);
    return mpi_errno;
}


int MPIDIG_send_target_msg_cb(int handler_id, void *am_hdr, void **data, size_t * p_data_sz,
                              int is_local, int *is_contig,
                              MPIDIG_am_target_cmpl_cb * target_cmpl_cb, MPIR_Request ** req)
{
    int mpi_errno = MPI_SUCCESS;
    MPIR_Request *rreq = NULL;
    MPIR_Comm *root_comm;
    MPIDIG_hdr_t *hdr = (MPIDIG_hdr_t *) am_hdr;

#ifndef MPIDI_CH4_DIRECT_NETMOD
    MPIR_Request *anysource_partner = NULL;
#endif

    MPIR_FUNC_VERBOSE_STATE_DECL(MPID_STATE_MPIDIG_SEND_TARGET_MSG_CB);
    MPIR_FUNC_VERBOSE_ENTER(MPID_STATE_MPIDIG_SEND_TARGET_MSG_CB);
    root_comm = MPIDIG_context_id_to_comm(hdr->context_id);
    if (root_comm) {
        /* MPIDI_CS_ENTER(); */
#ifdef MPIDI_CH4_DIRECT_NETMOD
        rreq = MPIDIG_dequeue_posted(hdr->src_rank, hdr->tag, hdr->context_id,
                                     &MPIDIG_COMM(root_comm, posted_list));
#else /* MPIDI_CH4_DIRECT_NETMOD */
        while (TRUE) {
            rreq = MPIDIG_dequeue_posted(hdr->src_rank, hdr->tag, hdr->context_id,
                                         &MPIDIG_COMM(root_comm, posted_list));

            if (rreq && MPIDI_REQUEST_ANYSOURCE_PARTNER(rreq)) {
                anysource_partner = MPIDI_REQUEST_ANYSOURCE_PARTNER(rreq);
                if (!MPIR_STATUS_GET_CANCEL_BIT(anysource_partner->status)) {
                    mpi_errno = MPID_Cancel_recv(anysource_partner);
                    if (mpi_errno != MPI_SUCCESS) {
                        goto fn_fail;
                    }
                    if (!MPIR_STATUS_GET_CANCEL_BIT(anysource_partner->status)) {
                        anysource_partner = NULL;
                        MPIR_Comm_release(root_comm);   /* -1 for posted_list */
                        MPIR_Datatype_release_if_not_builtin(MPIDIG_REQUEST(rreq, datatype));
                        continue;
                    }
                }
                MPIDI_REQUEST_ANYSOURCE_PARTNER(rreq) = NULL;
                MPIDI_REQUEST_ANYSOURCE_PARTNER(anysource_partner) = NULL;

                MPIR_Request_free(anysource_partner);
            }

            break;
        }
#endif /* MPIDI_CH4_DIRECT_NETMOD */
        /* MPIDI_CS_EXIT(); */
    }

    if (rreq == NULL) {
        rreq = MPIDIG_request_create(MPIR_REQUEST_KIND__RECV, 2);
        MPIR_ERR_CHKANDSTMT(rreq == NULL, mpi_errno, MPIX_ERR_NOREQ, goto fn_fail, "**nomemreq");
        MPIDIG_REQUEST(rreq, datatype) = MPI_BYTE;
        if (p_data_sz) {
            MPIDIG_REQUEST(rreq, buffer) = (char *) MPL_malloc(*p_data_sz, MPL_MEM_BUFFER);
            MPIDIG_REQUEST(rreq, count) = *p_data_sz;
        } else {
            MPIDIG_REQUEST(rreq, buffer) = NULL;
            MPIDIG_REQUEST(rreq, count) = 0;
        }
        MPIDIG_REQUEST(rreq, rank) = hdr->src_rank;
        MPIDIG_REQUEST(rreq, tag) = hdr->tag;
        MPIDIG_REQUEST(rreq, context_id) = hdr->context_id;
        MPIDIG_REQUEST(rreq, req->status) |= MPIDIG_REQ_BUSY;
        MPIDIG_REQUEST(rreq, req->status) |= MPIDIG_REQ_UNEXPECTED;
#ifndef MPIDI_CH4_DIRECT_NETMOD
        MPIDI_REQUEST(rreq, is_local) = is_local;
#endif
        /* MPIDI_CS_ENTER(); */
        if (root_comm) {
            MPIR_Comm_add_ref(root_comm);
            MPIDIG_enqueue_unexp(rreq, &MPIDIG_COMM(root_comm, unexp_list));
        } else {
            MPIDIG_enqueue_unexp(rreq, MPIDIG_context_id_to_uelist(hdr->context_id));
        }
        /* MPIDI_CS_EXIT(); */
    } else {
        /* rreq != NULL <=> root_comm != NULL */
        MPIR_Assert(root_comm);
        /* Decrement the refcnt when popping a request out from posted_list */
        MPIR_Comm_release(root_comm);
        MPIDIG_REQUEST(rreq, rank) = hdr->src_rank;
        MPIDIG_REQUEST(rreq, tag) = hdr->tag;
        MPIDIG_REQUEST(rreq, context_id) = hdr->context_id;
    }

    rreq->status.MPI_ERROR = hdr->error_bits;
    MPIDIG_REQUEST(rreq, req->status) |= MPIDIG_REQ_IN_PROGRESS;

    *req = rreq;

    mpi_errno = do_send_target(data, p_data_sz, is_contig, target_cmpl_cb, rreq);

#ifndef MPIDI_CH4_DIRECT_NETMOD
    if (unlikely(anysource_partner)) {
        anysource_partner->status = rreq->status;
    }
#endif /* MPIDI_CH4_DIRECT_NETMOD */

  fn_exit:
    MPIR_FUNC_VERBOSE_EXIT(MPID_STATE_MPIDIG_SEND_TARGET_MSG_CB);
    return mpi_errno;
  fn_fail:
    goto fn_exit;
}

int MPIDIG_send_long_req_target_msg_cb(int handler_id, void *am_hdr, void **data,
                                       size_t * p_data_sz, int is_local, int *is_contig,
                                       MPIDIG_am_target_cmpl_cb * target_cmpl_cb,
                                       MPIR_Request ** req)
{
    int mpi_errno = MPI_SUCCESS;
    MPIR_Request *rreq = NULL;
    MPIR_Comm *root_comm;
    MPIDIG_hdr_t *hdr = (MPIDIG_hdr_t *) am_hdr;
    MPIDIG_send_long_req_mst_t *lreq_hdr = (MPIDIG_send_long_req_mst_t *) am_hdr;

#ifndef MPIDI_CH4_DIRECT_NETMOD
    MPIR_Request *anysource_partner = NULL;
#endif

    MPIR_FUNC_VERBOSE_STATE_DECL(MPID_STATE_MPIDIG_SEND_LONG_REQ_TARGET_MSG_CB);
    MPIR_FUNC_VERBOSE_ENTER(MPID_STATE_MPIDIG_SEND_LONG_REQ_TARGET_MSG_CB);

    root_comm = MPIDIG_context_id_to_comm(hdr->context_id);
    if (root_comm) {
        /* MPIDI_CS_ENTER(); */
#ifdef MPIDI_CH4_DIRECT_NETMOD
        rreq = MPIDIG_dequeue_posted(hdr->src_rank, hdr->tag, hdr->context_id,
                                     &MPIDIG_COMM(root_comm, posted_list));
#else /* MPIDI_CH4_DIRECT_NETMOD */
        int continue_matching = 1;
        while (continue_matching) {
            rreq = MPIDIG_dequeue_posted(hdr->src_rank, hdr->tag, hdr->context_id,
                                         &MPIDIG_COMM(root_comm, posted_list));

            if (rreq && MPIDI_REQUEST_ANYSOURCE_PARTNER(rreq)) {
                anysource_partner = MPIDI_REQUEST_ANYSOURCE_PARTNER(rreq);

                mpi_errno = MPIDI_anysource_matched(anysource_partner,
                                                    MPIDI_REQUEST(rreq, is_local) ?
                                                    MPIDI_SHM : MPIDI_NETMOD, &continue_matching);

                if (MPI_SUCCESS != mpi_errno)
                    MPIR_ERR_POP(mpi_errno);

                MPIDI_REQUEST_ANYSOURCE_PARTNER(rreq) = NULL;
                MPIDI_REQUEST_ANYSOURCE_PARTNER(anysource_partner) = NULL;
            }

            break;
        }
#endif /* MPIDI_CH4_DIRECT_NETMOD */
        /* MPIDI_CS_EXIT(); */
    }

    if (rreq == NULL) {
        rreq = MPIDIG_request_create(MPIR_REQUEST_KIND__RECV, 2);
        MPIR_ERR_CHKANDSTMT(rreq == NULL, mpi_errno, MPIX_ERR_NOREQ, goto fn_fail, "**nomemreq");

        MPIDIG_REQUEST(rreq, buffer) = NULL;
        MPIDIG_REQUEST(rreq, datatype) = MPI_BYTE;
        MPIDIG_REQUEST(rreq, count) = lreq_hdr->data_sz;
        MPIDIG_REQUEST(rreq, req->status) |= MPIDIG_REQ_LONG_RTS;
        MPIDIG_REQUEST(rreq, req->rreq.peer_req_ptr) = lreq_hdr->sreq_ptr;
        MPIDIG_REQUEST(rreq, rank) = hdr->src_rank;
        MPIDIG_REQUEST(rreq, tag) = hdr->tag;
        MPIDIG_REQUEST(rreq, context_id) = hdr->context_id;
        MPIDIG_REQUEST(rreq, req->status) |= MPIDIG_REQ_IN_PROGRESS;

#ifndef MPIDI_CH4_DIRECT_NETMOD
        MPIDI_REQUEST(rreq, is_local) = is_local;
#endif

        /* MPIDI_CS_ENTER(); */
        if (root_comm) {
            MPIR_Comm_add_ref(root_comm);
            MPIDIG_enqueue_unexp(rreq, &MPIDIG_COMM(root_comm, unexp_list));
        } else {
            MPIDIG_enqueue_unexp(rreq,
                                 MPIDIG_context_id_to_uelist(MPIDIG_REQUEST(rreq, context_id)));
        }
        /* MPIDI_CS_EXIT(); */
    } else {
        /* Matching receive was posted, tell the netmod */
        MPIR_Comm_release(root_comm);   /* -1 for posted_list */
        MPIDIG_REQUEST(rreq, req->status) |= MPIDIG_REQ_LONG_RTS;
        MPIDIG_REQUEST(rreq, req->rreq.peer_req_ptr) = lreq_hdr->sreq_ptr;
        MPIDIG_REQUEST(rreq, rank) = hdr->src_rank;
        MPIDIG_REQUEST(rreq, tag) = hdr->tag;
        MPIDIG_REQUEST(rreq, context_id) = hdr->context_id;
        MPIDIG_REQUEST(rreq, req->status) |= MPIDIG_REQ_IN_PROGRESS;

#ifndef MPIDI_CH4_DIRECT_NETMOD
        if (MPIDI_REQUEST(rreq, is_local))
            mpi_errno = MPIDI_SHM_am_recv(rreq);
        else
#endif
        {
            mpi_errno = MPIDI_NM_am_recv(rreq);
        }

        if (mpi_errno)
            MPIR_ERR_POP(mpi_errno);

#ifndef MPIDI_CH4_DIRECT_NETMOD
        if (unlikely(anysource_partner)) {
            anysource_partner->status = rreq->status;
        }
#endif /* MPIDI_CH4_DIRECT_NETMOD */
    }

  fn_exit:
    MPIR_FUNC_VERBOSE_EXIT(MPID_STATE_MPIDIG_SEND_LONG_REQ_TARGET_MSG_CB);
    return mpi_errno;
  fn_fail:
    goto fn_exit;
}

int MPIDIG_send_long_lmt_target_msg_cb(int handler_id, void *am_hdr, void **data,
                                       size_t * p_data_sz, int is_local, int *is_contig,
                                       MPIDIG_am_target_cmpl_cb * target_cmpl_cb,
                                       MPIR_Request ** req)
{
    int mpi_errno;
    MPIR_Request *rreq;
    MPIDIG_send_long_lmt_msg_t *lmt_hdr = (MPIDIG_send_long_lmt_msg_t *) am_hdr;

    MPIR_FUNC_VERBOSE_STATE_DECL(MPID_STATE_MPIDIG_SEND_LONG_LMT_TARGET_MSG_CB);
    MPIR_FUNC_VERBOSE_ENTER(MPID_STATE_MPIDIG_SEND_LONG_LMT_TARGET_MSG_CB);

    rreq = (MPIR_Request *) lmt_hdr->rreq_ptr;
    MPIR_Assert(rreq);
    mpi_errno = do_send_target(data, p_data_sz, is_contig, target_cmpl_cb, rreq);
    *req = rreq;

    MPIR_FUNC_VERBOSE_EXIT(MPID_STATE_MPIDIG_SEND_LONG_LMT_TARGET_MSG_CB);

    return mpi_errno;
}

int MPIDIG_ssend_target_msg_cb(int handler_id, void *am_hdr, void **data, size_t * p_data_sz,
                               int is_local, int *is_contig,
                               MPIDIG_am_target_cmpl_cb * target_cmpl_cb, MPIR_Request ** req)
{
    int mpi_errno = MPI_SUCCESS;

    MPIDIG_ssend_req_msg_t *msg_hdr = (MPIDIG_ssend_req_msg_t *) am_hdr;
    MPIR_FUNC_VERBOSE_STATE_DECL(MPID_STATE_MPIDIG_SSEND_TARGET_MSG_CB);
    MPIR_FUNC_VERBOSE_ENTER(MPID_STATE_MPIDIG_SSEND_TARGET_MSG_CB);

    mpi_errno =
        MPIDIG_send_target_msg_cb(handler_id, am_hdr, data, p_data_sz, is_local, is_contig,
                                  target_cmpl_cb, req);
    if (mpi_errno)
        MPIR_ERR_POP(mpi_errno);

    MPIR_Assert(req);
    MPIDIG_REQUEST(*req, req->rreq.peer_req_ptr) = msg_hdr->sreq_ptr;
    MPIDIG_REQUEST(*req, req->status) |= MPIDIG_REQ_PEER_SSEND;
  fn_exit:
    MPIR_FUNC_VERBOSE_EXIT(MPID_STATE_MPIDIG_SSEND_TARGET_MSG_CB);
    return mpi_errno;
  fn_fail:
    goto fn_exit;
}

int MPIDIG_ssend_ack_target_msg_cb(int handler_id, void *am_hdr, void **data, size_t * p_data_sz,
                                   int is_local, int *is_contig,
                                   MPIDIG_am_target_cmpl_cb * target_cmpl_cb, MPIR_Request ** req)
{
    int mpi_errno = MPI_SUCCESS;
    MPIR_Request *sreq;
    MPIDIG_ssend_ack_msg_t *msg_hdr = (MPIDIG_ssend_ack_msg_t *) am_hdr;
    MPIR_FUNC_VERBOSE_STATE_DECL(MPID_STATE_MPIDIG_SSEND_ACK_TARGET_MSG_CB);
    MPIR_FUNC_VERBOSE_ENTER(MPID_STATE_MPIDIG_SSEND_ACK_TARGET_MSG_CB);

    sreq = (MPIR_Request *) msg_hdr->sreq_ptr;
    MPID_Request_complete(sreq);

    if (req)
        *req = NULL;
    if (target_cmpl_cb)
        *target_cmpl_cb = NULL;

    MPIR_FUNC_VERBOSE_EXIT(MPID_STATE_MPIDIG_SSEND_ACK_TARGET_MSG_CB);
    return mpi_errno;
}

int MPIDIG_send_long_ack_target_msg_cb(int handler_id, void *am_hdr, void **data,
                                       size_t * p_data_sz, int is_local, int *is_contig,
                                       MPIDIG_am_target_cmpl_cb * target_cmpl_cb,
                                       MPIR_Request ** req)
{
    int mpi_errno = MPI_SUCCESS;
    MPIR_Request *sreq;
    MPIDIG_send_long_ack_msg_t *msg_hdr = (MPIDIG_send_long_ack_msg_t *) am_hdr;
    MPIDIG_send_long_lmt_msg_t send_hdr;
    MPIR_FUNC_VERBOSE_STATE_DECL(MPID_STATE_MPIDIG_SEND_LONG_ACK_TARGET_MSG_CB);
    MPIR_FUNC_VERBOSE_ENTER(MPID_STATE_MPIDIG_SEND_LONG_ACK_TARGET_MSG_CB);

    sreq = (MPIR_Request *) msg_hdr->sreq_ptr;
    MPIR_Assert(sreq != NULL);

    /* Start the main data transfer */
    send_hdr.rreq_ptr = msg_hdr->rreq_ptr;
#ifndef MPIDI_CH4_DIRECT_NETMOD
    if (MPIDI_REQUEST(sreq, is_local))
        mpi_errno =
            MPIDI_SHM_am_isend_reply(MPIDIG_REQUEST(sreq, req->lreq).context_id,
                                     MPIDIG_REQUEST(sreq, rank), MPIDIG_SEND_LONG_LMT,
                                     &send_hdr, sizeof(send_hdr),
                                     MPIDIG_REQUEST(sreq, req->lreq).src_buf,
                                     MPIDIG_REQUEST(sreq, req->lreq).count,
                                     MPIDIG_REQUEST(sreq, req->lreq).datatype, sreq);
    else
#endif
    {
        mpi_errno =
            MPIDI_NM_am_isend_reply(MPIDIG_REQUEST(sreq, req->lreq).context_id,
                                    MPIDIG_REQUEST(sreq, rank), MPIDIG_SEND_LONG_LMT,
                                    &send_hdr, sizeof(send_hdr),
                                    MPIDIG_REQUEST(sreq, req->lreq).src_buf,
                                    MPIDIG_REQUEST(sreq, req->lreq).count,
                                    MPIDIG_REQUEST(sreq, req->lreq).datatype, sreq);
    }

    if (mpi_errno)
        MPIR_ERR_POP(mpi_errno);

    if (target_cmpl_cb)
        *target_cmpl_cb = MPIDIG_send_origin_cb;

    MPIR_FUNC_VERBOSE_EXIT(MPID_STATE_MPIDIG_SEND_LONG_ACK_TARGET_MSG_CB);

  fn_exit:
    return mpi_errno;
  fn_fail:
    goto fn_exit;
}

int MPIDIG_comm_abort_origin_cb(MPIR_Request * sreq)
{
    MPIR_FUNC_VERBOSE_STATE_DECL(MPID_STATE_MPIDIG_COMM_ABORT_ORIGIN_CB);
    MPIR_FUNC_VERBOSE_ENTER(MPID_STATE_MPIDIG_COMM_ABORT_ORIGIN_CB);
    MPID_Request_complete(sreq);
    MPIR_FUNC_VERBOSE_EXIT(MPID_STATE_MPIDIG_COMM_ABORT_ORIGIN_CB);
    return MPI_SUCCESS;
}

int MPIDIG_comm_abort_target_msg_cb(int handler_id, void *am_hdr, void **data, size_t * p_data_sz,
                                    int is_local, int *is_contig,
                                    MPIDIG_am_target_cmpl_cb * target_cmpl_cb, MPIR_Request ** req)
{
    MPIDIG_hdr_t *hdr = (MPIDIG_hdr_t *) am_hdr;

    MPIR_FUNC_VERBOSE_STATE_DECL(MPID_STATE_MPIDIG_COMM_ABORT_TARGET_MSG_CB);
    MPIR_FUNC_VERBOSE_ENTER(MPID_STATE_MPIDIG_COMM_ABORT_TARGET_MSG_CB);
    MPIR_FUNC_VERBOSE_EXIT(MPID_STATE_MPIDIG_COMM_ABORT_TARGET_MSG_CB);
    MPL_exit(hdr->tag);
    return MPI_SUCCESS;
}
