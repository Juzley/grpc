/*
 *
 * Copyright 2015, Google Inc.
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

#include "src/core/support/string.h"

#include <grpc/support/port_platform.h>
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/slice.h>
#include <grpc/support/thd.h>
#include <grpc/grpc.h>
#include <grpc/grpc_security.h>

#include <string.h>

#ifdef GPR_WIN32
#define GPR_EXPORT __declspec(dllexport)
#define GPR_CALLTYPE __stdcall
#endif

#ifndef GPR_EXPORT
#define GPR_EXPORT
#endif

#ifndef GPR_CALLTYPE
#define GPR_CALLTYPE
#endif

grpc_byte_buffer *string_to_byte_buffer(const char *buffer, size_t len) {
  gpr_slice slice = gpr_slice_from_copied_buffer(buffer, len);
  grpc_byte_buffer *bb = grpc_byte_buffer_create(&slice, 1);
  gpr_slice_unref(slice);
  return bb;
}

typedef void(GPR_CALLTYPE *callback_funcptr)(grpc_op_error op_error,
                                             void *batch_context);

/*
 * Helper to maintain lifetime of batch op inputs and store batch op outputs.
 */
typedef struct gprcsharp_batch_context {
  grpc_metadata_array send_initial_metadata;
  grpc_byte_buffer *send_message;
  struct {
    grpc_metadata_array trailing_metadata;
    char *status_details;
  } send_status_from_server;
  grpc_metadata_array recv_initial_metadata;
  grpc_byte_buffer *recv_message;
  struct {
    grpc_metadata_array trailing_metadata;
    grpc_status_code status;
    char *status_details;
    size_t status_details_capacity;
  } recv_status_on_client;
  int recv_close_on_server_cancelled;
  struct {
    grpc_call *call;
    grpc_call_details call_details;
    grpc_metadata_array request_metadata;
  } server_rpc_new;

  /* callback will be called upon completion */
  callback_funcptr callback;

} grpcsharp_batch_context;

grpcsharp_batch_context *grpcsharp_batch_context_create() {
  grpcsharp_batch_context *ctx = gpr_malloc(sizeof(grpcsharp_batch_context));
  memset(ctx, 0, sizeof(grpcsharp_batch_context));
  return ctx;
}

/**
 * Destroys metadata array including keys and values.
 */
void grpcsharp_metadata_array_destroy_recursive(grpc_metadata_array *array) {
  if (!array->metadata) {
    return;
  }
  /* TODO: destroy also keys and values */
  grpc_metadata_array_destroy(array);
}

void grpcsharp_batch_context_destroy(grpcsharp_batch_context *ctx) {
  if (!ctx) {
    return;
  }
  grpcsharp_metadata_array_destroy_recursive(&(ctx->send_initial_metadata));

  grpc_byte_buffer_destroy(ctx->send_message);

  grpcsharp_metadata_array_destroy_recursive(
      &(ctx->send_status_from_server.trailing_metadata));
  gpr_free(ctx->send_status_from_server.status_details);

  grpc_metadata_array_destroy(&(ctx->recv_initial_metadata));

  grpc_byte_buffer_destroy(ctx->recv_message);

  grpc_metadata_array_destroy(&(ctx->recv_status_on_client.trailing_metadata));
  gpr_free((void *)ctx->recv_status_on_client.status_details);

  /* NOTE: ctx->server_rpc_new.call is not destroyed because callback handler is
     supposed
     to take its ownership. */

  grpc_call_details_destroy(&(ctx->server_rpc_new.call_details));
  grpc_metadata_array_destroy(&(ctx->server_rpc_new.request_metadata));

  gpr_free(ctx);
}

GPR_EXPORT gpr_intptr GPR_CALLTYPE grpcsharp_batch_context_recv_message_length(
    const grpcsharp_batch_context *ctx) {
  if (!ctx->recv_message) {
    return -1;
  }
  return grpc_byte_buffer_length(ctx->recv_message);
}

/*
 * Copies data from recv_message to a buffer. Fatal error occurs if
 * buffer is too small.
 */
GPR_EXPORT void GPR_CALLTYPE grpcsharp_batch_context_recv_message_to_buffer(
    const grpcsharp_batch_context *ctx, char *buffer, size_t buffer_len) {
  grpc_byte_buffer_reader *reader;
  gpr_slice slice;
  size_t offset = 0;

  reader = grpc_byte_buffer_reader_create(ctx->recv_message);

  while (grpc_byte_buffer_reader_next(reader, &slice)) {
    size_t len = GPR_SLICE_LENGTH(slice);
    GPR_ASSERT(offset + len <= buffer_len);
    memcpy(buffer + offset, GPR_SLICE_START_PTR(slice),
           GPR_SLICE_LENGTH(slice));
    offset += len;
    gpr_slice_unref(slice);
  }
  grpc_byte_buffer_reader_destroy(reader);
}

GPR_EXPORT grpc_status_code GPR_CALLTYPE
grpcsharp_batch_context_recv_status_on_client_status(
    const grpcsharp_batch_context *ctx) {
  return ctx->recv_status_on_client.status;
}

GPR_EXPORT const char *GPR_CALLTYPE
grpcsharp_batch_context_recv_status_on_client_details(
    const grpcsharp_batch_context *ctx) {
  return ctx->recv_status_on_client.status_details;
}

GPR_EXPORT grpc_call *GPR_CALLTYPE grpcsharp_batch_context_server_rpc_new_call(
    const grpcsharp_batch_context *ctx) {
  return ctx->server_rpc_new.call;
}

GPR_EXPORT const char *GPR_CALLTYPE
grpcsharp_batch_context_server_rpc_new_method(
    const grpcsharp_batch_context *ctx) {
  return ctx->server_rpc_new.call_details.method;
}

/* Init & shutdown */

GPR_EXPORT void GPR_CALLTYPE grpcsharp_init(void) { grpc_init(); }

GPR_EXPORT void GPR_CALLTYPE grpcsharp_shutdown(void) { grpc_shutdown(); }

/* Completion queue */

GPR_EXPORT grpc_completion_queue *GPR_CALLTYPE
grpcsharp_completion_queue_create(void) {
  return grpc_completion_queue_create();
}

GPR_EXPORT void GPR_CALLTYPE
grpcsharp_completion_queue_shutdown(grpc_completion_queue *cq) {
  grpc_completion_queue_shutdown(cq);
}

GPR_EXPORT void GPR_CALLTYPE
grpcsharp_completion_queue_destroy(grpc_completion_queue *cq) {
  grpc_completion_queue_destroy(cq);
}

GPR_EXPORT grpc_completion_type GPR_CALLTYPE
grpcsharp_completion_queue_next_with_callback(grpc_completion_queue *cq) {
  grpc_event *ev;
  grpcsharp_batch_context *batch_context;
  grpc_completion_type t;
  void(GPR_CALLTYPE * callback)(grpc_event *);

  ev = grpc_completion_queue_next(cq, gpr_inf_future);
  t = ev->type;
  if (t == GRPC_OP_COMPLETE && ev->tag) {
    /* NEW API handler */
    batch_context = (grpcsharp_batch_context *)ev->tag;
    batch_context->callback(ev->data.op_complete, batch_context);
    grpcsharp_batch_context_destroy(batch_context);
  } else if (ev->tag) {
    /* call the callback in ev->tag */
    /* C forbids to cast object pointers to function pointers, so
     * we cast to intptr first.
     */
    callback = (void(GPR_CALLTYPE *)(grpc_event *))(gpr_intptr)ev->tag;
    (*callback)(ev);
  }
  grpc_event_finish(ev);

  /* return completion type to allow some handling for events that have no
   * tag - such as GRPC_QUEUE_SHUTDOWN
   */
  return t;
}

/* Channel */

GPR_EXPORT grpc_channel *GPR_CALLTYPE
grpcsharp_channel_create(const char *target, const grpc_channel_args *args) {
  return grpc_channel_create(target, args);
}

GPR_EXPORT void GPR_CALLTYPE grpcsharp_channel_destroy(grpc_channel *channel) {
  grpc_channel_destroy(channel);
}

GPR_EXPORT grpc_call *GPR_CALLTYPE
grpcsharp_channel_create_call(grpc_channel *channel, grpc_completion_queue *cq,
                              const char *method, const char *host,
                              gpr_timespec deadline) {
  return grpc_channel_create_call(channel, cq, method, host, deadline);
}

/* Channel args */

GPR_EXPORT grpc_channel_args *GPR_CALLTYPE
grpcsharp_channel_args_create(size_t num_args) {
  grpc_channel_args *args =
      (grpc_channel_args *)gpr_malloc(sizeof(grpc_channel_args));
  memset(args, 0, sizeof(grpc_channel_args));

  args->num_args = num_args;
  args->args = (grpc_arg *)gpr_malloc(sizeof(grpc_arg) * num_args);
  memset(args->args, 0, sizeof(grpc_arg) * num_args);
  return args;
}

GPR_EXPORT void GPR_CALLTYPE
grpcsharp_channel_args_set_string(grpc_channel_args *args, size_t index,
                                  const char *key, const char *value) {
  GPR_ASSERT(args);
  GPR_ASSERT(index < args->num_args);
  args->args[index].type = GRPC_ARG_STRING;
  args->args[index].key = gpr_strdup(key);
  args->args[index].value.string = gpr_strdup(value);
}

GPR_EXPORT void GPR_CALLTYPE
grpcsharp_channel_args_destroy(grpc_channel_args *args) {
  size_t i;
  if (args) {
    for (i = 0; i < args->num_args; i++) {
      gpr_free(args->args[i].key);
      if (args->args[i].type == GRPC_ARG_STRING) {
        gpr_free(args->args[i].value.string);
      }
    }
    gpr_free(args->args);
    gpr_free(args);
  }
}

/* Timespec */

GPR_EXPORT gpr_timespec GPR_CALLTYPE gprsharp_now(void) { return gpr_now(); }

GPR_EXPORT gpr_timespec GPR_CALLTYPE gprsharp_inf_future(void) {
  return gpr_inf_future;
}

GPR_EXPORT gpr_int32 GPR_CALLTYPE gprsharp_sizeof_timespec(void) {
  return sizeof(gpr_timespec);
}

/* Call */

GPR_EXPORT grpc_call_error GPR_CALLTYPE grpcsharp_call_cancel(grpc_call *call) {
  return grpc_call_cancel(call);
}

GPR_EXPORT grpc_call_error GPR_CALLTYPE
grpcsharp_call_cancel_with_status(grpc_call *call, grpc_status_code status,
                                  const char *description) {
  return grpc_call_cancel_with_status(call, status, description);
}

GPR_EXPORT void GPR_CALLTYPE grpcsharp_call_destroy(grpc_call *call) {
  grpc_call_destroy(call);
}

GPR_EXPORT void GPR_CALLTYPE
grpcsharp_call_start_write_from_copied_buffer(grpc_call *call,
                                              const char *buffer, size_t len,
                                              void *tag, gpr_uint32 flags) {
  grpc_byte_buffer *byte_buffer = string_to_byte_buffer(buffer, len);
  GPR_ASSERT(grpc_call_start_write_old(call, byte_buffer, tag, flags) ==
             GRPC_CALL_OK);
  grpc_byte_buffer_destroy(byte_buffer);
}

GPR_EXPORT grpc_call_error GPR_CALLTYPE
grpcsharp_call_start_unary(grpc_call *call, callback_funcptr callback,
                           const char *send_buffer, size_t send_buffer_len) {
  /* TODO: don't use magic number */
  grpc_op ops[6];
  grpcsharp_batch_context *ctx = grpcsharp_batch_context_create();
  ctx->callback = callback;

  /* TODO: implement sending the metadata... */
  ops[0].op = GRPC_OP_SEND_INITIAL_METADATA;
  /* ctx->send_initial_metadata is already zeroed out. */
  ops[0].data.send_initial_metadata.count = 0;
  ops[0].data.send_initial_metadata.metadata = NULL;

  ops[1].op = GRPC_OP_SEND_MESSAGE;
  ctx->send_message = string_to_byte_buffer(send_buffer, send_buffer_len);
  ops[1].data.send_message = ctx->send_message;

  ops[2].op = GRPC_OP_SEND_CLOSE_FROM_CLIENT;

  ops[3].op = GRPC_OP_RECV_INITIAL_METADATA;
  ops[3].data.recv_initial_metadata = &(ctx->recv_initial_metadata);

  ops[4].op = GRPC_OP_RECV_MESSAGE;
  ops[4].data.recv_message = &(ctx->recv_message);

  ops[5].op = GRPC_OP_RECV_STATUS_ON_CLIENT;
  ops[5].data.recv_status_on_client.trailing_metadata =
      &(ctx->recv_status_on_client.trailing_metadata);
  ops[5].data.recv_status_on_client.status =
      &(ctx->recv_status_on_client.status);
  /* not using preallocation for status_details */
  ops[5].data.recv_status_on_client.status_details =
      &(ctx->recv_status_on_client.status_details);
  ops[5].data.recv_status_on_client.status_details_capacity =
      &(ctx->recv_status_on_client.status_details_capacity);

  return grpc_call_start_batch(call, ops, sizeof(ops) / sizeof(ops[0]), ctx);
}

/* Synchronous unary call */
GPR_EXPORT void GPR_CALLTYPE
grpcsharp_call_blocking_unary(grpc_call *call,
                              grpc_completion_queue *dedicated_cq,
                              callback_funcptr callback,
                              const char *send_buffer, size_t send_buffer_len) {
  GPR_ASSERT(grpcsharp_call_start_unary(call, callback, send_buffer,
                                        send_buffer_len) == GRPC_CALL_OK);

  /* TODO: we would like to use pluck, but we don't know the tag */
  GPR_ASSERT(grpcsharp_completion_queue_next_with_callback(dedicated_cq) ==
             GRPC_OP_COMPLETE);
  grpc_completion_queue_shutdown(dedicated_cq);
  GPR_ASSERT(grpcsharp_completion_queue_next_with_callback(dedicated_cq) ==
             GRPC_QUEUE_SHUTDOWN);
}

GPR_EXPORT grpc_call_error GPR_CALLTYPE
grpcsharp_call_start_client_streaming(grpc_call *call,
                                      callback_funcptr callback) {
  /* TODO: don't use magic number */
  grpc_op ops[4];
  grpcsharp_batch_context *ctx = grpcsharp_batch_context_create();
  ctx->callback = callback;

  /* TODO: implement sending the metadata... */
  ops[0].op = GRPC_OP_SEND_INITIAL_METADATA;
  /* ctx->send_initial_metadata is already zeroed out. */
  ops[0].data.send_initial_metadata.count = 0;
  ops[0].data.send_initial_metadata.metadata = NULL;

  ops[1].op = GRPC_OP_RECV_INITIAL_METADATA;
  ops[1].data.recv_initial_metadata = &(ctx->recv_initial_metadata);

  ops[2].op = GRPC_OP_RECV_MESSAGE;
  ops[2].data.recv_message = &(ctx->recv_message);

  ops[3].op = GRPC_OP_RECV_STATUS_ON_CLIENT;
  ops[3].data.recv_status_on_client.trailing_metadata =
      &(ctx->recv_status_on_client.trailing_metadata);
  ops[3].data.recv_status_on_client.status =
      &(ctx->recv_status_on_client.status);
  /* not using preallocation for status_details */
  ops[3].data.recv_status_on_client.status_details =
      &(ctx->recv_status_on_client.status_details);
  ops[3].data.recv_status_on_client.status_details_capacity =
      &(ctx->recv_status_on_client.status_details_capacity);

  return grpc_call_start_batch(call, ops, sizeof(ops) / sizeof(ops[0]), ctx);
}

GPR_EXPORT grpc_call_error GPR_CALLTYPE
grpcsharp_call_start_server_streaming(grpc_call *call,
                                      callback_funcptr callback,
                                      const char *send_buffer,
                                      size_t send_buffer_len) {
  /* TODO: don't use magic number */
  grpc_op ops[5];
  grpcsharp_batch_context *ctx = grpcsharp_batch_context_create();
  ctx->callback = callback;

  /* TODO: implement sending the metadata... */
  ops[0].op = GRPC_OP_SEND_INITIAL_METADATA;
  /* ctx->send_initial_metadata is already zeroed out. */
  ops[0].data.send_initial_metadata.count = 0;
  ops[0].data.send_initial_metadata.metadata = NULL;

  ops[1].op = GRPC_OP_SEND_MESSAGE;
  ctx->send_message = string_to_byte_buffer(send_buffer, send_buffer_len);
  ops[1].data.send_message = ctx->send_message;

  ops[2].op = GRPC_OP_SEND_CLOSE_FROM_CLIENT;

  ops[3].op = GRPC_OP_RECV_INITIAL_METADATA;
  ops[3].data.recv_initial_metadata = &(ctx->recv_initial_metadata);

  ops[4].op = GRPC_OP_RECV_STATUS_ON_CLIENT;
  ops[4].data.recv_status_on_client.trailing_metadata =
      &(ctx->recv_status_on_client.trailing_metadata);
  ops[4].data.recv_status_on_client.status =
      &(ctx->recv_status_on_client.status);
  /* not using preallocation for status_details */
  ops[4].data.recv_status_on_client.status_details =
      &(ctx->recv_status_on_client.status_details);
  ops[4].data.recv_status_on_client.status_details_capacity =
      &(ctx->recv_status_on_client.status_details_capacity);

  return grpc_call_start_batch(call, ops, sizeof(ops) / sizeof(ops[0]), ctx);
}

GPR_EXPORT grpc_call_error GPR_CALLTYPE
grpcsharp_call_start_duplex_streaming(grpc_call *call,
                                      callback_funcptr callback) {
  /* TODO: don't use magic number */
  grpc_op ops[3];
  grpcsharp_batch_context *ctx = grpcsharp_batch_context_create();
  ctx->callback = callback;

  /* TODO: implement sending the metadata... */
  ops[0].op = GRPC_OP_SEND_INITIAL_METADATA;
  /* ctx->send_initial_metadata is already zeroed out. */
  ops[0].data.send_initial_metadata.count = 0;
  ops[0].data.send_initial_metadata.metadata = NULL;

  ops[1].op = GRPC_OP_RECV_INITIAL_METADATA;
  ops[1].data.recv_initial_metadata = &(ctx->recv_initial_metadata);

  ops[2].op = GRPC_OP_RECV_STATUS_ON_CLIENT;
  ops[2].data.recv_status_on_client.trailing_metadata =
      &(ctx->recv_status_on_client.trailing_metadata);
  ops[2].data.recv_status_on_client.status =
      &(ctx->recv_status_on_client.status);
  /* not using preallocation for status_details */
  ops[2].data.recv_status_on_client.status_details =
      &(ctx->recv_status_on_client.status_details);
  ops[2].data.recv_status_on_client.status_details_capacity =
      &(ctx->recv_status_on_client.status_details_capacity);

  return grpc_call_start_batch(call, ops, sizeof(ops) / sizeof(ops[0]), ctx);
}

GPR_EXPORT grpc_call_error GPR_CALLTYPE
grpcsharp_call_send_message(grpc_call *call, callback_funcptr callback,
                            const char *send_buffer, size_t send_buffer_len) {
  /* TODO: don't use magic number */
  grpc_op ops[1];
  grpcsharp_batch_context *ctx = grpcsharp_batch_context_create();
  ctx->callback = callback;

  ops[0].op = GRPC_OP_SEND_MESSAGE;
  ctx->send_message = string_to_byte_buffer(send_buffer, send_buffer_len);
  ops[0].data.send_message = ctx->send_message;

  return grpc_call_start_batch(call, ops, sizeof(ops) / sizeof(ops[0]), ctx);
}

GPR_EXPORT grpc_call_error GPR_CALLTYPE
grpcsharp_call_send_close_from_client(grpc_call *call,
                                      callback_funcptr callback) {
  /* TODO: don't use magic number */
  grpc_op ops[1];
  grpcsharp_batch_context *ctx = grpcsharp_batch_context_create();
  ctx->callback = callback;

  ops[0].op = GRPC_OP_SEND_CLOSE_FROM_CLIENT;

  return grpc_call_start_batch(call, ops, sizeof(ops) / sizeof(ops[0]), ctx);
}

GPR_EXPORT grpc_call_error GPR_CALLTYPE
grpcsharp_call_send_status_from_server(grpc_call *call,
                                       callback_funcptr callback,
                                       grpc_status_code status_code,
                                       const char *status_details) {
  /* TODO: don't use magic number */
  grpc_op ops[1];
  grpcsharp_batch_context *ctx = grpcsharp_batch_context_create();
  ctx->callback = callback;

  ops[0].op = GRPC_OP_SEND_STATUS_FROM_SERVER;
  ops[0].data.send_status_from_server.status = status_code;
  ops[0].data.send_status_from_server.status_details =
      gpr_strdup(status_details);
  ops[0].data.send_status_from_server.trailing_metadata = NULL;
  ops[0].data.send_status_from_server.trailing_metadata_count = 0;

  return grpc_call_start_batch(call, ops, sizeof(ops) / sizeof(ops[0]), ctx);
}

GPR_EXPORT grpc_call_error GPR_CALLTYPE
grpcsharp_call_recv_message(grpc_call *call, callback_funcptr callback) {
  /* TODO: don't use magic number */
  grpc_op ops[1];
  grpcsharp_batch_context *ctx = grpcsharp_batch_context_create();
  ctx->callback = callback;

  ops[0].op = GRPC_OP_RECV_MESSAGE;
  ops[0].data.recv_message = &(ctx->recv_message);
  return grpc_call_start_batch(call, ops, sizeof(ops) / sizeof(ops[0]), ctx);
}

GPR_EXPORT grpc_call_error GPR_CALLTYPE
grpcsharp_call_start_serverside(grpc_call *call, callback_funcptr callback) {
  /* TODO: don't use magic number */
  grpc_op ops[2];

  grpcsharp_batch_context *ctx = grpcsharp_batch_context_create();
  ctx->callback = callback;

  ops[0].op = GRPC_OP_SEND_INITIAL_METADATA;
  ops[0].data.send_initial_metadata.count = 0;
  ops[0].data.send_initial_metadata.metadata = NULL;

  ops[1].op = GRPC_OP_RECV_CLOSE_ON_SERVER;
  ops[1].data.recv_close_on_server.cancelled =
      (&ctx->recv_close_on_server_cancelled);

  return grpc_call_start_batch(call, ops, sizeof(ops) / sizeof(ops[0]), ctx);
}

/* Server */

GPR_EXPORT grpc_server *GPR_CALLTYPE
grpcsharp_server_create(grpc_completion_queue *cq,
                        const grpc_channel_args *args) {
  return grpc_server_create(cq, args);
}

GPR_EXPORT gpr_int32 GPR_CALLTYPE
grpcsharp_server_add_http2_port(grpc_server *server, const char *addr) {
  return grpc_server_add_http2_port(server, addr);
}

GPR_EXPORT void GPR_CALLTYPE grpcsharp_server_start(grpc_server *server) {
  grpc_server_start(server);
}

GPR_EXPORT void GPR_CALLTYPE grpcsharp_server_shutdown(grpc_server *server) {
  grpc_server_shutdown(server);
}

GPR_EXPORT void GPR_CALLTYPE
grpcsharp_server_shutdown_and_notify(grpc_server *server, void *tag) {
  grpc_server_shutdown_and_notify(server, tag);
}

GPR_EXPORT void GPR_CALLTYPE grpcsharp_server_destroy(grpc_server *server) {
  grpc_server_destroy(server);
}

GPR_EXPORT grpc_call_error GPR_CALLTYPE
grpcsharp_server_request_call(grpc_server *server, grpc_completion_queue *cq,
                              callback_funcptr callback) {
  grpcsharp_batch_context *ctx = grpcsharp_batch_context_create();
  ctx->callback = callback;

  return grpc_server_request_call(
      server, &(ctx->server_rpc_new.call), &(ctx->server_rpc_new.call_details),
      &(ctx->server_rpc_new.request_metadata), cq, ctx);
}

/* Security */

GPR_EXPORT grpc_credentials *GPR_CALLTYPE
grpcsharp_ssl_credentials_create(const char *pem_root_certs,
                                 const char *key_cert_pair_cert_chain,
                                 const char *key_cert_pair_private_key) {
  grpc_ssl_pem_key_cert_pair key_cert_pair;
  if (key_cert_pair_cert_chain || key_cert_pair_private_key) {
    key_cert_pair.cert_chain = key_cert_pair_cert_chain;
    key_cert_pair.private_key = key_cert_pair_private_key;
    return grpc_ssl_credentials_create(pem_root_certs, &key_cert_pair);
  } else {
    GPR_ASSERT(!key_cert_pair_cert_chain);
    GPR_ASSERT(!key_cert_pair_private_key);
    return grpc_ssl_credentials_create(pem_root_certs, NULL);
  }
}

GPR_EXPORT void grpcsharp_credentials_release(grpc_credentials *creds) {
  grpc_credentials_release(creds);
}

GPR_EXPORT grpc_channel *GPR_CALLTYPE
grpcsharp_secure_channel_create(grpc_credentials *creds, const char *target,
                                const grpc_channel_args *args) {
  return grpc_secure_channel_create(creds, target, args);
}

GPR_EXPORT grpc_server_credentials *GPR_CALLTYPE
grpcsharp_ssl_server_credentials_create(
    const char *pem_root_certs, const char **key_cert_pair_cert_chain_array,
    const char **key_cert_pair_private_key_array, size_t num_key_cert_pairs) {
  size_t i;
  grpc_server_credentials *creds;
  grpc_ssl_pem_key_cert_pair *key_cert_pairs =
      gpr_malloc(sizeof(grpc_ssl_pem_key_cert_pair) * num_key_cert_pairs);
  memset(key_cert_pairs, 0,
         sizeof(grpc_ssl_pem_key_cert_pair) * num_key_cert_pairs);

  for (i = 0; i < num_key_cert_pairs; i++) {
    if (key_cert_pair_cert_chain_array[i] ||
        key_cert_pair_private_key_array[i]) {
      key_cert_pairs[i].cert_chain = key_cert_pair_cert_chain_array[i];
      key_cert_pairs[i].private_key = key_cert_pair_private_key_array[i];
    }
  }
  creds = grpc_ssl_server_credentials_create(pem_root_certs, key_cert_pairs,
                                             num_key_cert_pairs);
  gpr_free(key_cert_pairs);
  return creds;
}

GPR_EXPORT void grpcsharp_server_credentials_release(
    grpc_server_credentials *creds) {
  grpc_server_credentials_release(creds);
}

GPR_EXPORT gpr_int32 GPR_CALLTYPE
grpcsharp_server_add_secure_http2_port(grpc_server *server, const char *addr,
                                       grpc_server_credentials *creds) {
  return grpc_server_add_secure_http2_port(server, addr, creds);
}

/* Logging */

typedef void(GPR_CALLTYPE *grpcsharp_log_func)(const char *file, gpr_int32 line,
                                               gpr_uint64 thd_id,
                                               const char *severity_string,
                                               const char *msg);
static grpcsharp_log_func log_func = NULL;

/* Redirects gpr_log to log_func callback */
static void grpcsharp_log_handler(gpr_log_func_args *args) {
  log_func(args->file, args->line, gpr_thd_currentid(),
           gpr_log_severity_string(args->severity), args->message);
}

GPR_EXPORT void GPR_CALLTYPE grpcsharp_redirect_log(grpcsharp_log_func func) {
  GPR_ASSERT(func);
  log_func = func;
  gpr_set_log_function(grpcsharp_log_handler);
}

/* For testing */
GPR_EXPORT void GPR_CALLTYPE
grpcsharp_test_callback(callback_funcptr callback) {
  callback(GRPC_OP_OK, NULL);
}

/* For testing */
GPR_EXPORT void *GPR_CALLTYPE grpcsharp_test_nop(void *ptr) { return ptr; }
