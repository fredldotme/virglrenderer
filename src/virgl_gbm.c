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

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include <stdio.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <xf86drm.h>
#include <unistd.h>

#include "util/u_math.h"
#include "util/u_memory.h"
#include "pipe/p_state.h"

#include "virgl_gbm.h"
#include "virgl_hw.h"
#include "vrend_debug.h"

struct planar_layout {
    size_t num_planes;
    int horizontal_subsampling[4];
    int vertical_subsampling[4];
    int bytes_per_pixel[4];
};

static const struct planar_layout packed_1bpp_layout = {
    .num_planes = 1,
    .horizontal_subsampling = { 1 },
    .vertical_subsampling = { 1 },
    .bytes_per_pixel = { 1 }
};

static const struct planar_layout packed_2bpp_layout = {
    .num_planes = 1,
    .horizontal_subsampling = { 1 },
    .vertical_subsampling = { 1 },
    .bytes_per_pixel = { 2 }
};

static const struct planar_layout packed_4bpp_layout = {
    .num_planes = 1,
    .horizontal_subsampling = { 1 },
    .vertical_subsampling = { 1 },
    .bytes_per_pixel = { 4 }
};

static const struct planar_layout biplanar_yuv_420_layout = {
    .num_planes = 2,
    .horizontal_subsampling = { 1, 2 },
    .vertical_subsampling = { 1, 2 },
    .bytes_per_pixel = { 1, 2 }
};

static const struct planar_layout triplanar_yuv_420_layout = {
    .num_planes = 3,
    .horizontal_subsampling = { 1, 2, 2 },
    .vertical_subsampling = { 1, 2, 2 },
    .bytes_per_pixel = { 1, 1, 1 }
};

static int rendernode_open(void)
{
   DIR *dir;
   int ret, fd;
   bool undesired_found;
   drmVersionPtr version;
   char *rendernode_name;
   struct dirent *dir_ent;
   const char *undesired[3] = { "vgem", "pvr", NULL };

   dir = opendir("/dev/dri");
   if (!dir)
      return -1;

   fd = -1;
   while ((dir_ent = readdir(dir))) {
      if (dir_ent->d_type != DT_CHR)
         continue;

      if (strncmp(dir_ent->d_name, "renderD", 7))
         continue;

      ret = asprintf(&rendernode_name, "/dev/dri/%s", dir_ent->d_name);
      if (ret < 0)
         goto out;

      fd = open(rendernode_name, O_RDWR | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
      free(rendernode_name);

      if (fd < 0)
         continue;

      version = drmGetVersion(fd);
      if (!version) {
         close(fd);
         fd = -1;
         continue;
      }

      undesired_found = false;
      for (uint32_t i = 0; i < ARRAY_SIZE(undesired); i++) {
         if (undesired[i] && !strcmp(version->name, undesired[i]))
            undesired_found = true;
      }

      drmFreeVersion(version);
      if (undesired_found) {
         close(fd);
         fd = -1;
         continue;
      }

      break;
   }

out:
   closedir(dir);
   return fd;
}

static const struct planar_layout *layout_from_format(uint32_t format)
{
   switch (format) {
   case GBM_FORMAT_R8:
      return &packed_1bpp_layout;
   case GBM_FORMAT_YVU420:
      return &triplanar_yuv_420_layout;
   case GBM_FORMAT_NV12:
      return &biplanar_yuv_420_layout;
   case GBM_FORMAT_RGB565:
      return &packed_2bpp_layout;
   case GBM_FORMAT_ARGB8888:
   case GBM_FORMAT_XRGB8888:
   case GBM_FORMAT_ABGR8888:
   case GBM_FORMAT_XBGR8888:
      return &packed_4bpp_layout;
   default:
      return NULL;
   }
}

static void virgl_gbm_transfer_internal(uint32_t planar_bytes_per_pixel,
                                        uint32_t subsampled_width,
                                        uint32_t subsampled_height,
                                        uint32_t guest_plane_stride,
                                        uint32_t guest_resource_offset,
                                        uint32_t host_plane_stride, uint8_t *host_address,
                                        struct iovec *iovecs, uint32_t num_iovecs,
                                        uint32_t direction)
{
   bool next_iovec, next_line;
   uint32_t current_height, current_iovec, iovec_start_offset;
   current_height = current_iovec = iovec_start_offset = 0;

   while (current_height < subsampled_height && current_iovec < num_iovecs) {
      uint32_t iovec_size = iovecs[current_iovec].iov_len;
      uint32_t iovec_end_offset = iovec_start_offset + iovec_size;

      uint32_t box_start_offset = guest_resource_offset + current_height * guest_plane_stride;
      uint32_t box_end_offset = box_start_offset + subsampled_width * planar_bytes_per_pixel;

      uint32_t max_start = MAX2(iovec_start_offset, box_start_offset);
      uint32_t min_end = MIN2(iovec_end_offset, box_end_offset);

      if (max_start < min_end) {
         uint32_t offset_in_iovec = (max_start > iovec_start_offset) ?
                                    (max_start - iovec_start_offset) : 0;

         uint32_t copy_iovec_size = min_end - max_start;
         if (min_end >= iovec_end_offset) {
            next_iovec = true;
            next_line = false;
         } else {
            next_iovec = false;
            next_line = true;
         }

         uint8_t *guest_start = (uint8_t*)iovecs[current_iovec].iov_base + offset_in_iovec;
         uint8_t *host_start = host_address + (current_height * host_plane_stride) +
                               (max_start - box_start_offset);

         if (direction == VIRGL_TRANSFER_TO_HOST)
            memcpy(host_start, guest_start, copy_iovec_size);
         else
            memcpy(guest_start, host_start, copy_iovec_size);
      } else {
         if (box_start_offset >= iovec_start_offset) {
            next_iovec = true;
            next_line = false;
         } else {
            next_iovec = false;
            next_line = true;
         }
      }

      if (next_iovec) {
         iovec_start_offset += iovec_size;
         current_iovec++;
      }

      if (next_line)
         current_height++;
   }
}

struct virgl_gbm *virgl_gbm_init(int fd)
{
   struct virgl_gbm *gbm = calloc(1, sizeof(struct virgl_gbm));
   if (!gbm)
      return NULL;

   gbm->fd = -1;
   if (fd < 0) {
      gbm->fd = rendernode_open();
      if (gbm->fd < 0)
         goto out_error;

      gbm->device = gbm_create_device(gbm->fd);
      if (!gbm->device) {
         close(gbm->fd);
         goto out_error;
      }
   } else {
      gbm->device = gbm_create_device(fd);
      if (!gbm->device)
         goto out_error;
   }

   return gbm;

out_error:
   free(gbm);
   return NULL;
}

void virgl_gbm_fini(struct virgl_gbm *gbm)
{
   gbm_device_destroy(gbm->device);
   if (gbm->fd >= 0)
      close(gbm->fd);
   free(gbm);
}

uint32_t virgl_gbm_convert_format(uint32_t virgl_format)
{
   switch (virgl_format) {
   case VIRGL_FORMAT_B5G6R5_UNORM:
      return GBM_FORMAT_RGB565;
   case VIRGL_FORMAT_B8G8R8A8_UNORM:
      return GBM_FORMAT_ARGB8888;
   case VIRGL_FORMAT_B8G8R8X8_UNORM:
      return GBM_FORMAT_XRGB8888;
   case VIRGL_FORMAT_NV12:
      return GBM_FORMAT_NV12;
   case VIRGL_FORMAT_R8G8B8A8_UNORM:
      return GBM_FORMAT_ABGR8888;
   case VIRGL_FORMAT_R8G8B8X8_UNORM:
      return GBM_FORMAT_XBGR8888;
   case VIRGL_FORMAT_R8_UNORM:
      return GBM_FORMAT_R8;
   case VIRGL_FORMAT_YV12:
      return GBM_FORMAT_YVU420;
   default:
      return 0;
   }
}

int virgl_gbm_transfer(struct gbm_bo *bo, uint32_t direction, struct iovec *iovecs,
                       uint32_t num_iovecs, const struct vrend_transfer_info *info)
{
   void *map_data;
   uint32_t host_plane_offset, guest_plane_offset, guest_stride0, calc_stride0, host_map_stride0;

   uint32_t width = gbm_bo_get_width(bo);
   uint32_t height = gbm_bo_get_height(bo);
   uint32_t format = gbm_bo_get_format(bo);
   int plane_count = gbm_bo_get_plane_count(bo);
   const struct planar_layout *layout = layout_from_format(format);
   if (!layout)
      return -1;

   host_plane_offset = guest_plane_offset = host_map_stride0 = guest_stride0 = 0;
   uint32_t map_flags = (direction == VIRGL_TRANSFER_TO_HOST) ? GBM_BO_TRANSFER_WRITE :
                                                                GBM_BO_TRANSFER_READ;
   void *addr = gbm_bo_map(bo, 0, 0, width, height, map_flags, &host_map_stride0, &map_data);
   if (!addr)
      return -1;

   /*
    * Unfortunately, the kernel doesn't actually pass the guest layer_stride and
    * guest stride to the host (compare virtio_gpu.h and virtgpu_drm.h). We can use
    * the level (always zero for 2D images) to work around this.
    */
   guest_stride0 = info->stride;
   calc_stride0 = width * layout->bytes_per_pixel[0];
   if (!guest_stride0)
      guest_stride0 = (info->level > 0) ? (uint32_t)info->level : calc_stride0;

   if (guest_stride0 < calc_stride0)
      return -1;

   if (guest_stride0 > host_map_stride0)
      return -1;

   for (int plane = 0; plane < plane_count; plane++) {
      host_plane_offset += gbm_bo_get_offset(bo, plane);

      uint32_t subsampled_x = info->box->x / layout->horizontal_subsampling[plane];
      uint32_t subsampled_y = info->box->y / layout->vertical_subsampling[plane];
      uint32_t subsampled_width = info->box->width / layout->horizontal_subsampling[plane];
      uint32_t subsampled_height = info->box->height / layout->vertical_subsampling[plane];
      uint32_t plane_height = height / layout->vertical_subsampling[plane];
      uint32_t guest_plane_stride = guest_stride0 / layout->horizontal_subsampling[plane];
      uint32_t host_plane_stride = host_map_stride0 / layout->horizontal_subsampling[plane];

      uint32_t guest_resource_offset = guest_plane_offset + (subsampled_y * guest_plane_stride)
                                       + subsampled_x * layout->bytes_per_pixel[plane];
      uint32_t host_resource_offset = host_plane_offset + (subsampled_y * host_plane_stride)
                                       + subsampled_x * layout->bytes_per_pixel[plane];

      uint8_t *host_address = (uint8_t*)addr + host_resource_offset;

      virgl_gbm_transfer_internal(layout->bytes_per_pixel[plane], subsampled_width,
                                  subsampled_height, guest_plane_stride, guest_resource_offset,
                                  host_plane_stride, host_address, iovecs, num_iovecs, direction);

      guest_plane_offset += plane_height * guest_plane_stride;
   }

   gbm_bo_unmap(bo, map_data);
   return 0;
}

uint32_t virgl_gbm_convert_flags(uint32_t virgl_bind_flags)
{
   uint32_t flags = 0;
   if (virgl_bind_flags & VIRGL_BIND_RENDER_TARGET)
      flags |= GBM_BO_USE_RENDERING;
   if (virgl_bind_flags & VIRGL_BIND_SCANOUT)
      flags |= GBM_BO_USE_SCANOUT;
   if (virgl_bind_flags & VIRGL_BIND_CURSOR)
      flags |= GBM_BO_USE_CURSOR;
   return flags;
}

int virgl_gbm_export_query(struct gbm_bo *bo, struct virgl_renderer_export_query *query)
{
   int ret = -1;
   uint32_t handles[4] = {0, 0, 0, 0};
   struct gbm_device *gbm = gbm_bo_get_device(bo);
   int num_planes = gbm_bo_get_plane_count(bo);
   if (num_planes < 0 || num_planes > 4)
      return ret;

   query->out_num_fds = 0;
   query->out_fourcc = 0;
   query->out_modifier = 0;
   for (int plane = 0; plane < 4; plane++) {
      query->out_fds[plane] = -1;
      query->out_strides[plane] = 0;
      query->out_offsets[plane] = 0;
   }

   for (int plane = 0; plane < num_planes; plane++) {
      uint32_t i, handle;
      query->out_strides[plane] = gbm_bo_get_stride_for_plane(bo, plane);
      query->out_offsets[plane] = gbm_bo_get_offset(bo, plane);
      handle = gbm_bo_get_handle_for_plane(bo, plane).u32;

      for (i = 0; i < query->out_num_fds; i++) {
         if (handles[query->out_num_fds] == handle)
            break;
      }

      if (i == query->out_num_fds) {
         if (query->in_export_fds) {
            ret = drmPrimeHandleToFD(gbm_device_get_fd(gbm), handle, DRM_CLOEXEC,
                                     &query->out_fds[query->out_num_fds]);
            if (ret)
               goto err_close;
         }
         query->out_num_fds++;
      }
   }

   query->out_modifier = gbm_bo_get_modifier(bo);
   query->out_fourcc = gbm_bo_get_format(bo);
   return 0;

err_close:
   for (int plane = 0; plane < 4; plane++) {
      if (query->out_fds[plane] >= 0) {
         close(query->out_fds[plane]);
         query->out_fds[plane] = -1;
      }

      query->out_strides[plane] = 0;
      query->out_offsets[plane] = 0;
   }

   query->out_num_fds = 0;
   return ret;
}
