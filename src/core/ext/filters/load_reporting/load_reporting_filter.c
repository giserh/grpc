/*
 *
 * Copyright 2016, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <string.h>

#include <grpc/load_reporting.h>
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/string_util.h>
#include <grpc/support/sync.h>

#include "src/core/ext/filters/load_reporting/load_reporting.h"
#include "src/core/ext/filters/load_reporting/load_reporting_filter.h"
#include "src/core/lib/channel/channel_args.h"
#include "src/core/lib/profiling/timers.h"
#include "src/core/lib/slice/slice_internal.h"
#include "src/core/lib/transport/static_metadata.h"

typedef struct call_data {
  intptr_t id; /**< an id unique to the call */
  bool have_initial_md_string;
  grpc_slice initial_md_string;
  bool have_service_method;
  grpc_slice service_method;

  /* stores the recv_initial_metadata op's ready closure, which we wrap with our
   * own (on_initial_md_ready) in order to capture the incoming initial metadata
   * */
  grpc_closure *ops_recv_initial_metadata_ready;

  /* to get notified of the availability of the incoming initial metadata. */
  grpc_closure on_initial_md_ready;
  grpc_metadata_batch *recv_initial_metadata;
} call_data;

typedef struct channel_data {
  intptr_t id; /**< an id unique to the channel */
} channel_data;

static void on_initial_md_ready(grpc_exec_ctx *exec_ctx, void *user_data,
                                grpc_error *err) {
  grpc_call_element *elem = user_data;
  call_data *calld = elem->call_data;

  if (err == GRPC_ERROR_NONE) {
    if (calld->recv_initial_metadata->idx.named.path != NULL) {
      calld->service_method = grpc_slice_ref_internal(
          GRPC_MDVALUE(calld->recv_initial_metadata->idx.named.path->md));
      calld->have_service_method = true;
    } else {
      err = grpc_error_add_child(
          err, GRPC_ERROR_CREATE_FROM_STATIC_STRING("Missing :path header"));
    }
    if (calld->recv_initial_metadata->idx.named.lb_token != NULL) {
      calld->initial_md_string = grpc_slice_ref_internal(
          GRPC_MDVALUE(calld->recv_initial_metadata->idx.named.lb_token->md));
      calld->have_initial_md_string = true;
      grpc_metadata_batch_remove(
          exec_ctx, calld->recv_initial_metadata,
          calld->recv_initial_metadata->idx.named.lb_token);
    }
  } else {
    GRPC_ERROR_REF(err);
  }
  calld->ops_recv_initial_metadata_ready->cb(
      exec_ctx, calld->ops_recv_initial_metadata_ready->cb_arg, err);
  GRPC_ERROR_UNREF(err);
}

/* Constructor for call_data */
static grpc_error *init_call_elem(grpc_exec_ctx *exec_ctx,
                                  grpc_call_element *elem,
                                  const grpc_call_element_args *args) {
  call_data *calld = elem->call_data;
  calld->id = (intptr_t)args->call_stack;
  grpc_closure_init(&calld->on_initial_md_ready, on_initial_md_ready, elem,
                    grpc_schedule_on_exec_ctx);

  /* TODO(dgq): do something with the data
  channel_data *chand = elem->channel_data;
  grpc_load_reporting_call_data lr_call_data = {GRPC_LR_POINT_CALL_CREATION,
                                                (intptr_t)chand->id,
                                                (intptr_t)calld->id,
                                                NULL,
                                                NULL,
                                                NULL,
                                                NULL};
  */

  return GRPC_ERROR_NONE;
}

/* Destructor for call_data */
static void destroy_call_elem(grpc_exec_ctx *exec_ctx, grpc_call_element *elem,
                              const grpc_call_final_info *final_info,
                              grpc_closure *ignored) {
  call_data *calld = elem->call_data;

  /* TODO(dgq): do something with the data
  channel_data *chand = elem->channel_data;
  grpc_load_reporting_call_data lr_call_data = {GRPC_LR_POINT_CALL_DESTRUCTION,
                                                (intptr_t)chand->id,
                                                (intptr_t)calld->id,
                                                final_info,
                                                calld->initial_md_string,
                                                calld->trailing_md_string,
                                                calld->service_method};
  */

  if (calld->have_initial_md_string) {
    grpc_slice_unref_internal(exec_ctx, calld->initial_md_string);
  }
  if (calld->have_service_method) {
    grpc_slice_unref_internal(exec_ctx, calld->service_method);
  }
}

/* Constructor for channel_data */
static grpc_error *init_channel_elem(grpc_exec_ctx *exec_ctx,
                                     grpc_channel_element *elem,
                                     grpc_channel_element_args *args) {
  GPR_ASSERT(!args->is_last);

  channel_data *chand = elem->channel_data;
  chand->id = (intptr_t)args->channel_stack;

  /* TODO(dgq): do something with the data
  grpc_load_reporting_call_data lr_call_data = {GRPC_LR_POINT_CHANNEL_CREATION,
                                                (intptr_t)chand,
                                                0,
                                                NULL,
                                                NULL,
                                                NULL,
                                                NULL};
                                                */

  return GRPC_ERROR_NONE;
}

/* Destructor for channel data */
static void destroy_channel_elem(grpc_exec_ctx *exec_ctx,
                                 grpc_channel_element *elem) {
  /* TODO(dgq): do something with the data
  channel_data *chand = elem->channel_data;
  grpc_load_reporting_call_data lr_call_data = {
      GRPC_LR_POINT_CHANNEL_DESTRUCTION,
      (intptr_t)chand->id,
      0,
      NULL,
      NULL,
      NULL,
      NULL};
  */
}

static void lr_start_transport_stream_op_batch(
    grpc_exec_ctx *exec_ctx, grpc_call_element *elem,
    grpc_transport_stream_op_batch *op) {
  GPR_TIMER_BEGIN("lr_start_transport_stream_op_batch", 0);
  call_data *calld = elem->call_data;

  if (op->recv_initial_metadata) {
    calld->recv_initial_metadata =
        op->payload->recv_initial_metadata.recv_initial_metadata;
    /* substitute our callback for the higher callback */
    calld->ops_recv_initial_metadata_ready =
        op->payload->recv_initial_metadata.recv_initial_metadata_ready;
    op->payload->recv_initial_metadata.recv_initial_metadata_ready =
        &calld->on_initial_md_ready;
  }
  grpc_call_next_op(exec_ctx, elem, op);

  GPR_TIMER_END("lr_start_transport_stream_op_batch", 0);
}

const grpc_channel_filter grpc_load_reporting_filter = {
    lr_start_transport_stream_op_batch,
    grpc_channel_next_op,
    sizeof(call_data),
    init_call_elem,
    grpc_call_stack_ignore_set_pollset_or_pollset_set,
    destroy_call_elem,
    sizeof(channel_data),
    init_channel_elem,
    destroy_channel_elem,
    grpc_call_next_get_peer,
    grpc_channel_next_get_info,
    "load_reporting"};
