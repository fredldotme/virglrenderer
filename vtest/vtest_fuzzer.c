// Copyright 2019 The Chromium OS Authors. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//    * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//    * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//    * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include <stdio.h>
#include <signal.h>
#include <stdbool.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/un.h>
#include <fcntl.h>
#include <getopt.h>
#include <string.h>

#include "util.h"
#include "util/u_memory.h"
#include "vtest.h"
#include "vtest_protocol.h"
#include "virglrenderer.h"

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

#ifndef CLEANUP_EACH_INPUT
// eglInitialize leaks unless eglTeriminate is called (which only happens
// with CLEANUP_EACH_INPUT), so suppress leak detection on everything
// allocated by it.

#if !defined(__has_feature)
#define __has_feature(x) 0
#endif

#if __has_feature(address_sanitizer)
const char* __lsan_default_suppressions(void);

const char* __lsan_default_suppressions() {
   return "leak:dri2_initialize_surfaceless\n";
}
#endif // __has_feature(address_sanitizer)

#endif // !CLEANUP_EACH_INPUT

typedef int (*vtest_cmd_fptr_t)(uint32_t);

static const vtest_cmd_fptr_t vtest_commands[] = {
   NULL /* CMD ids starts at 1 */,
   vtest_send_caps,
   vtest_create_resource,
   vtest_resource_unref,
   vtest_transfer_get_nop,
   vtest_transfer_put_nop,
   vtest_submit_cmd,
   vtest_resource_busy_wait,
   NULL, /* vtest_create_renderer is a specific case */
   vtest_send_caps2,
   vtest_ping_protocol_version,
   vtest_protocol_version,
   vtest_create_resource2,
   vtest_transfer_get2_nop,
   vtest_transfer_put2_nop,
};

static void vtest_fuzzer_run_renderer(int out_fd, struct vtest_input *input,
                                      int ctx_flags, bool create_fences)
{
   int ret;
   uint32_t header[VTEST_HDR_SIZE];
   int initialized = 0;

   do {
      ret = input->read(input, &header, sizeof(header));
      if (ret < 0 || (size_t)ret < sizeof(header)) {
         break;
      }

      if (!initialized) {
         /* The first command MUST be VCMD_CREATE_RENDERER */
         if (header[1] != VCMD_CREATE_RENDERER) {
            break;
         }

         ret = vtest_create_renderer(input, out_fd, header[0], ctx_flags);
         if (ret < 0) {
            break;
         }
         initialized = 1;
         vtest_poll();
         continue;
      }

      vtest_poll();
      if (header[1] <= 0 || header[1] >= ARRAY_SIZE(vtest_commands)) {
         break;
      }

      if (vtest_commands[header[1]] == NULL) {
         break;
      }

      ret = vtest_commands[header[1]](header[0]);
      if (ret < 0) {
         break;
      }

      /* GL draws are fenced, while possible fence creations are too */
      if (create_fences &&
          (header[1] == VCMD_SUBMIT_CMD || header[1] == VCMD_RESOURCE_CREATE ||
           header[1] == VCMD_RESOURCE_CREATE2))
         vtest_renderer_create_fence();
   } while (1);

   vtest_destroy_renderer();
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
   /* Limit unbounded allocations under fuzzer default limits. */
   vtest_set_max_length(256 * 1024 * 1024);

   int out_fd = open("/dev/null", O_WRONLY);

   struct vtest_buffer buffer;
   buffer.buffer = data;
   buffer.size = size;
   struct vtest_input input;
   input.data.buffer = &buffer;
   input.read = vtest_buf_read;

   vtest_fuzzer_run_renderer(out_fd, &input,
                             VIRGL_RENDERER_USE_EGL |
                             VIRGL_RENDERER_USE_SURFACELESS |
                             (getenv("VTEST_FUZZER_USE_GL") != NULL ?
                              0 : VIRGL_RENDERER_USE_GLES),
                             getenv("VTEST_FUZZER_FENCES") != NULL);

   close(out_fd);

   return 0;
}
