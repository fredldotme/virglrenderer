/**************************************************************************
 *
 * Copyright (C) 2019 Chromium.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

#ifndef VIRGL_GBM_H
#define VIRGL_GBM_H

#include <gbm.h>
#include "vrend_iov.h"
#include "virglrenderer.h"

/*
 * If fd >= 0, virglrenderer owns the fd since it was opened via a rendernode
 * query. If fd < 0, the gbm device was opened with the fd provided by the
 * (*get_drm_fd) hook.
 */
struct virgl_gbm {
   int fd;
   struct gbm_device *device;
};

struct virgl_gbm *virgl_gbm_init(int fd);

void virgl_gbm_fini(struct virgl_gbm *gbm);

uint32_t virgl_gbm_convert_format(uint32_t virgl_format);

int virgl_gbm_transfer(struct gbm_bo *bo, uint32_t direction, struct iovec *iovecs,
                       uint32_t num_iovecs, const struct vrend_transfer_info *info);

uint32_t virgl_gbm_convert_flags(uint32_t virgl_bind_flags);

int virgl_gbm_export_query(struct gbm_bo *bo, struct virgl_renderer_export_query *query);

#endif
