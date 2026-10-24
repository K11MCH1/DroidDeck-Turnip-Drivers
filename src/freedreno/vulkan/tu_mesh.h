/*
 * Copyright © 2026 MaxsTechReview
 * SPDX-License-Identifier: MIT
 */

#ifndef TU_MESH_H
#define TU_MESH_H

#include "tu_common.h"

/* Mesh and task shaders run as compute dispatches that write into a
 * device-wide ring. A generated vertex shader then pulls one vertex per
 * primitive corner out of the ring in a non-indexed draw.
 *
 * Ring layout:
 *  - setup parameters (enum tu_mesh_param);
 *  - a mesh and a task table, each holding an entry count, per-chunk
 *    dispatch and draw arguments, and entries sorted by first linear
 *    workgroup (first workgroup, group counts, draw id, task slot);
 *  - task slots: launch dimensions and draw id, followed by the payload;
 *  - records: per mesh workgroup vertex, primitive and index data.
 */
#define TU_MESH_RING_SIZE (64u << 20)

enum tu_mesh_param {
   TU_MESH_PARAM_TABLE,
   TU_MESH_PARAM_SOURCE,
   TU_MESH_PARAM_SRC_LO,
   TU_MESH_PARAM_SRC_HI,
   TU_MESH_PARAM_STRIDE,
   TU_MESH_PARAM_MAX_COUNT,
   TU_MESH_PARAM_COUNT,
   TU_MESH_PARAM_FIRST,
   TU_MESH_PARAM_CHUNK,
   TU_MESH_PARAM_VERTICES,
   TU_MESH_PARAM_CHUNKS,
   TU_MESH_PARAM_NUM,
};

enum tu_mesh_source {
   TU_MESH_SOURCE_INDIRECT,
   TU_MESH_SOURCE_TASK,
};

#define TU_MESH_PARAMS_OFFSET 0
#define TU_MESH_MAX_CHUNKS 32
#define TU_MESH_MAX_TASK_CHUNKS 4
#define TU_MESH_TABLE_MAX_ENTRIES 16384
#define TU_MESH_TABLE_ENTRY_SIZE 32
/* Chunk arguments: dispatch size, enable flag, then a VkDrawIndirectCommand. */
#define TU_MESH_ARGS_SIZE 32
#define TU_MESH_ARG_ENABLE 12
#define TU_MESH_ARG_DRAW 16
#define TU_MESH_TABLE_ARGS 64
#define TU_MESH_TABLE_ENTRIES                                                \
   (TU_MESH_TABLE_ARGS + TU_MESH_MAX_CHUNKS * TU_MESH_ARGS_SIZE)
#define TU_MESH_TABLE_SIZE                                                   \
   (TU_MESH_TABLE_ENTRIES + TU_MESH_TABLE_MAX_ENTRIES * TU_MESH_TABLE_ENTRY_SIZE)
#define TU_MESH_MS_TABLE_OFFSET 64
#define TU_MESH_TS_TABLE_OFFSET (TU_MESH_MS_TABLE_OFFSET + TU_MESH_TABLE_SIZE)
#define TU_MESH_TASK_OFFSET (TU_MESH_TS_TABLE_OFFSET + TU_MESH_TABLE_SIZE)
#define TU_MESH_TASK_HEADER_SIZE 16
#define TU_MESH_TASK_SIZE (16u << 20)
#define TU_MESH_RECORD_OFFSET (TU_MESH_TASK_OFFSET + TU_MESH_TASK_SIZE)
#define TU_MESH_RECORD_SIZE (TU_MESH_RING_SIZE - TU_MESH_RECORD_OFFSET)

#define TU_MESH_SETUP_WORKGROUP_SIZE 128
#define TU_MESH_DEAD_INDEX 0xffffffffu

/* The fragment shader reads a mesh shader's PrimitiveId from this generic
 * varying, since only a geometry shader may write the hardware one.
 */
#define TU_MESH_PRIMITIVE_ID_SLOT VARYING_SLOT_TEX0

struct tu_mesh_io {
   uint8_t vertex_slot[VARYING_SLOT_MAX];
   uint8_t prim_slot[VARYING_SLOT_MAX];
   unsigned vertex_slots;
   unsigned prim_slots;

   unsigned max_vertices;
   unsigned max_primitives;
   unsigned verts_per_prim;

   unsigned prim_offset;
   unsigned index_offset;
   unsigned cull_offset;
   unsigned stride;
   unsigned chunk_workgroups;

   bool writes_primitive_id;
};

/* State a mesh or task compute variant carries into the draw path. */
struct tu_mesh_state {
   uint32_t stride;
   uint32_t chunk_workgroups;
   uint32_t task_payload_stride;
   uint16_t max_primitives;
   uint8_t verts_per_prim;
   uint8_t topology;
};

void
tu_mesh_gather_io(const nir_shader *ms, struct tu_mesh_io *io);

nir_shader *
tu_mesh_build_vs(const nir_shader *ms, const struct tu_mesh_io *io,
                 const nir_shader_compiler_options *options);

void
tu_mesh_lower_ms(nir_shader *ms, const struct tu_mesh_io *io,
                 unsigned task_payload_stride);

unsigned
tu_mesh_lower_ts(nir_shader *ts);

unsigned
tu_mesh_task_chunk(unsigned task_payload_stride);

void
tu_mesh_lower_fs_inputs(nir_shader *fs, bool remap_primitive_id);

nir_shader *
tu_mesh_build_setup_cs(const nir_shader_compiler_options *options);

#endif /* TU_MESH_H */
