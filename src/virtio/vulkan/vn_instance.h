/*
 * Copyright 2019 Google LLC
 * SPDX-License-Identifier: MIT
 *
 * based in part on anv and radv which are:
 * Copyright © 2015 Intel Corporation
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 */

#ifndef VN_INSTANCE_H
#define VN_INSTANCE_H

#include "vn_common.h"

#include "venus-protocol/vn_protocol_driver_defines.h"

#include "vn_cs.h"
#include "vn_renderer.h"
#include "vn_renderer_util.h"
#include "vn_ring.h"

/* require and request at least Vulkan 1.1 at both instance and device levels
 */
#define VN_MIN_RENDERER_VERSION VK_API_VERSION_1_1

/* max advertised version at both instance and device levels */
#ifdef ANDROID
#define VN_MAX_API_VERSION VK_MAKE_VERSION(1, 1, VK_HEADER_VERSION)
#else
#define VN_MAX_API_VERSION VK_MAKE_VERSION(1, 3, VK_HEADER_VERSION)
#endif

struct vn_instance {
   struct vn_instance_base base;

   struct driOptionCache dri_options;
   struct driOptionCache available_dri_options;

   struct vn_renderer *renderer;

   struct vn_renderer_shmem_pool reply_shmem_pool;

   mtx_t ring_idx_mutex;
   uint64_t ring_idx_used_mask;

   /* XXX staged features to be merged to core venus protocol */
   VkVenusExperimentalFeatures100000MESA experimental;

   struct {
      mtx_t mutex;
      struct vn_renderer_shmem *shmem;
      struct vn_ring ring;
      uint64_t id;

      struct vn_cs_encoder upload;
      uint32_t command_dropped;

      /* to synchronize renderer/ring */
      mtx_t roundtrip_mutex;
      uint32_t roundtrip_next;
   } ring;

   /* Between the driver and the app, VN_MAX_API_VERSION is what we advertise
    * and base.base.app_info.api_version is what the app requests.
    *
    * Between the driver and the renderer, renderer_api_version is the api
    * version we request internally, which can be higher than
    * base.base.app_info.api_version.  renderer_version is the instance
    * version we can use internally.
    */
   uint32_t renderer_api_version;
   uint32_t renderer_version;

   /* for VN_CS_ENCODER_STORAGE_SHMEM_POOL */
   struct {
      mtx_t mutex;
      struct vn_renderer_shmem_pool pool;
   } cs_shmem;

   struct {
      mtx_t mutex;
      bool initialized;

      struct vn_physical_device *devices;
      uint32_t device_count;
      VkPhysicalDeviceGroupProperties *groups;
      uint32_t group_count;
   } physical_device;
};
VK_DEFINE_HANDLE_CASTS(vn_instance,
                       base.base.base,
                       VkInstance,
                       VK_OBJECT_TYPE_INSTANCE)

VkResult
vn_instance_submit_roundtrip(struct vn_instance *instance,
                             uint32_t *roundtrip_seqno);

void
vn_instance_wait_roundtrip(struct vn_instance *instance,
                           uint32_t roundtrip_seqno);

static inline void
vn_instance_roundtrip(struct vn_instance *instance)
{
   uint32_t roundtrip_seqno;
   if (vn_instance_submit_roundtrip(instance, &roundtrip_seqno) == VK_SUCCESS)
      vn_instance_wait_roundtrip(instance, roundtrip_seqno);
}

VkResult
vn_instance_ring_submit(struct vn_instance *instance,
                        const struct vn_cs_encoder *cs);

struct vn_instance_submit_command {
   /* empty command implies errors */
   struct vn_cs_encoder command;
   struct vn_cs_encoder_buffer buffer;
   /* non-zero implies waiting */
   size_t reply_size;

   /* when reply_size is non-zero, NULL can be returned on errors */
   struct vn_renderer_shmem *reply_shmem;
   struct vn_cs_decoder reply;
};

static inline struct vn_cs_encoder *
vn_instance_submit_command_init(struct vn_instance *instance,
                                struct vn_instance_submit_command *submit,
                                void *cmd_data,
                                size_t cmd_size,
                                size_t reply_size)
{
   submit->buffer = VN_CS_ENCODER_BUFFER_INITIALIZER(cmd_data);
   submit->command = VN_CS_ENCODER_INITIALIZER(&submit->buffer, cmd_size);

   submit->reply_size = reply_size;
   submit->reply_shmem = NULL;

   return &submit->command;
}

void
vn_instance_submit_command(struct vn_instance *instance,
                           struct vn_instance_submit_command *submit);

static inline struct vn_cs_decoder *
vn_instance_get_command_reply(struct vn_instance *instance,
                              struct vn_instance_submit_command *submit)
{
   return submit->reply_shmem ? &submit->reply : NULL;
}

static inline void
vn_instance_free_command_reply(struct vn_instance *instance,
                               struct vn_instance_submit_command *submit)
{
   assert(submit->reply_shmem);
   vn_renderer_shmem_unref(instance->renderer, submit->reply_shmem);
}

static inline struct vn_renderer_shmem *
vn_instance_cs_shmem_alloc(struct vn_instance *instance,
                           size_t size,
                           size_t *out_offset)
{
   struct vn_renderer_shmem *shmem;

   mtx_lock(&instance->cs_shmem.mutex);
   shmem = vn_renderer_shmem_pool_alloc(
      instance->renderer, &instance->cs_shmem.pool, size, out_offset);
   mtx_unlock(&instance->cs_shmem.mutex);

   return shmem;
}

static inline int
vn_instance_acquire_ring_idx(struct vn_instance *instance)
{
   mtx_lock(&instance->ring_idx_mutex);
   int ring_idx = ffsll(~instance->ring_idx_used_mask) - 1;
   if (ring_idx >= instance->renderer->info.max_timeline_count)
      ring_idx = -1;
   if (ring_idx > 0)
      instance->ring_idx_used_mask |= (1ULL << (uint32_t)ring_idx);
   mtx_unlock(&instance->ring_idx_mutex);

   assert(ring_idx); /* never acquire the dedicated CPU ring */

   /* returns -1 when no vacant rings */
   return ring_idx;
}

static inline void
vn_instance_release_ring_idx(struct vn_instance *instance, uint32_t ring_idx)
{
   assert(ring_idx > 0);

   mtx_lock(&instance->ring_idx_mutex);
   assert(instance->ring_idx_used_mask & (1ULL << ring_idx));
   instance->ring_idx_used_mask &= ~(1ULL << ring_idx);
   mtx_unlock(&instance->ring_idx_mutex);
}

#endif /* VN_INSTANCE_H */
