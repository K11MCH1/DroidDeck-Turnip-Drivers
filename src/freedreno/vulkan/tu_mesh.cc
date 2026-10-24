/*
 * Copyright © 2026 MaxsTechReview
 * SPDX-License-Identifier: MIT
 */

#include "tu_mesh.h"

#include "nir/nir_builder.h"
#include "util/u_math.h"

static bool
is_indices_or_cull(const nir_variable *var)
{
   return var->data.location == VARYING_SLOT_PRIMITIVE_INDICES ||
          var->data.location == VARYING_SLOT_CULL_PRIMITIVE;
}

/* Number of vec4 record slots a per-vertex or per-primitive element uses. */
static unsigned
io_slots(const nir_variable *var, const glsl_type *elem)
{
   if (var->data.compact)
      return DIV_ROUND_UP(var->data.location_frac + glsl_get_length(elem), 4);
   return glsl_count_vec4_slots(elem, false, true);
}

/* Explicit layout of an output element inside a record: every slot takes 16
 * bytes and compact arrays pack four scalars per slot, matching how varyings
 * are assigned to locations and components.
 */
static const glsl_type *
io_explicit_type(const glsl_type *type, bool compact)
{
   if (glsl_type_is_array(type)) {
      const glsl_type *elem = glsl_get_array_element(type);
      unsigned stride = compact ? glsl_get_bit_size(elem) / 8 :
                        glsl_count_vec4_slots(elem, false, true) * 16;
      return glsl_array_type(io_explicit_type(elem, compact),
                             glsl_get_length(type), stride);
   }

   if (glsl_type_is_matrix(type))
      return glsl_explicit_matrix_type(type, 16, false);

   if (glsl_type_is_struct(type)) {
      unsigned num_fields = glsl_get_length(type);
      glsl_struct_field *fields =
         (glsl_struct_field *) calloc(num_fields, sizeof(*fields));
      unsigned slot = 0;
      for (unsigned i = 0; i < num_fields; i++) {
         fields[i] = *glsl_get_struct_field_data(type, i);
         fields[i].type = io_explicit_type(fields[i].type, false);
         fields[i].offset = slot * 16;
         slot += glsl_count_vec4_slots(glsl_get_struct_field(type, i), false, true);
      }
      const glsl_type *result =
         glsl_struct_type_with_explicit_alignment(fields, num_fields,
                                                  glsl_get_type_name(type),
                                                  false, 16);
      free(fields);
      return result;
   }

   return type;
}

static void
assign_slots(uint8_t *slot_map, unsigned *count, const nir_variable *var,
             const glsl_type *elem)
{
   unsigned slots = io_slots(var, elem);
   for (unsigned i = 0; i < slots; i++)
      slot_map[var->data.location + i] = 1;
   *count = MAX2(*count, (unsigned) var->data.location + slots);
}

static unsigned
compact_slots(uint8_t *slot_map, unsigned max_location)
{
   unsigned n = 0;
   for (unsigned loc = 0; loc < max_location; loc++)
      slot_map[loc] = slot_map[loc] ? n++ : UINT8_MAX;
   for (unsigned loc = max_location; loc < VARYING_SLOT_MAX; loc++)
      slot_map[loc] = UINT8_MAX;
   return n;
}

void
tu_mesh_gather_io(const nir_shader *ms, struct tu_mesh_io *io)
{
   memset(io, 0, sizeof(*io));

   unsigned vertex_end = 0, prim_end = 0;
   nir_foreach_shader_out_variable (var, ms) {
      if (is_indices_or_cull(var))
         continue;

      assert(var->data.location < VARYING_SLOT_MAX);
      const glsl_type *elem = glsl_get_array_element(var->type);
      if (var->data.per_primitive)
         assign_slots(io->prim_slot, &prim_end, var, elem);
      else
         assign_slots(io->vertex_slot, &vertex_end, var, elem);

      if (var->data.location == VARYING_SLOT_PRIMITIVE_ID)
         io->writes_primitive_id = true;
   }

   io->vertex_slots = compact_slots(io->vertex_slot, vertex_end);
   io->prim_slots = compact_slots(io->prim_slot, prim_end);

   io->max_vertices = MAX2(ms->info.mesh.max_vertices_out, 1);
   io->max_primitives = MAX2(ms->info.mesh.max_primitives_out, 1);
   io->verts_per_prim = mesa_vertices_per_prim(ms->info.mesh.primitive_type);

   unsigned vertex_size = io->max_vertices * io->vertex_slots * 16;
   io->prim_offset = vertex_size;
   io->index_offset =
      io->prim_offset + io->max_primitives * io->prim_slots * 16;
   io->cull_offset = io->index_offset +
      align(io->max_primitives * io->verts_per_prim * 4, 16);
   io->stride = io->cull_offset + align(io->max_primitives * 4, 16);
   io->chunk_workgroups = MIN2(TU_MESH_RECORD_SIZE / io->stride, 65535);
}

static nir_def *
addr_add(nir_builder *b, nir_def *addr, nir_def *offset)
{
   return nir_iadd(b, addr, nir_u2u64(b, offset));
}

static nir_def *
load_ring(nir_builder *b)
{
   return nir_pack_64_2x32(b, nir_load_mesh_ring_ir3(b));
}

static unsigned
var_record_offset(const struct tu_mesh_io *io, const nir_variable *var)
{
   const uint8_t *slot_map =
      var->data.per_primitive ? io->prim_slot : io->vertex_slot;
   return slot_map[var->data.location] * 16 + var->data.location_frac * 4;
}

/* Rebuilds every deref chain rooted at old onto new_parent. */
static void
rewrite_deref_chain(nir_builder *b, nir_deref_instr *old,
                    nir_deref_instr *new_parent)
{
   nir_foreach_use_safe (src, &old->def) {
      nir_instr *user = nir_src_use_instr(src);
      if (user->type == nir_instr_type_deref) {
         nir_deref_instr *child = nir_instr_as_deref(user);
         b->cursor = nir_before_instr(user);
         rewrite_deref_chain(b, child,
                             nir_build_deref_follower(b, new_parent, child));
      } else {
         nir_src_rewrite(src, &new_parent->def);
      }
   }
}

/* Turns all accesses to var into global memory accesses at addr. */
static void
retarget_var(nir_function_impl *impl, nir_variable *var, nir_def *addr,
             const glsl_type *type, unsigned align_offset)
{
   nir_builder b = nir_builder_create(impl);

   nir_foreach_block (block, impl) {
      nir_foreach_instr_safe (instr, block) {
         if (instr->type != nir_instr_type_deref)
            continue;

         nir_deref_instr *deref = nir_instr_as_deref(instr);
         if (deref->deref_type != nir_deref_type_var || deref->var != var)
            continue;

         b.cursor = nir_before_instr(instr);
         nir_deref_instr *cast =
            nir_build_deref_cast_with_alignment(&b, addr, nir_var_mem_global,
                                                type, 0, 16, align_offset);
         rewrite_deref_chain(&b, deref, cast);
      }
   }
}

static nir_def *
record_array_addr(nir_builder *b, nir_def *rec, const struct tu_mesh_io *io,
                  const nir_variable *var)
{
   unsigned base = var->data.per_primitive ? io->prim_offset : 0;
   return addr_add(b, rec, nir_imm_int(b, base + var_record_offset(io, var)));
}

static const glsl_type *
record_array_type(const struct tu_mesh_io *io, const nir_variable *var)
{
   unsigned stride =
      (var->data.per_primitive ? io->prim_slots : io->vertex_slots) * 16;
   return glsl_array_type(io_explicit_type(glsl_get_array_element(var->type),
                                           var->data.compact),
                          glsl_get_length(var->type), stride);
}

/* Copies an output element out of its record, one vector at a time. */
static void
copy_output(nir_builder *b, nir_deref_instr *dst, nir_deref_instr *src)
{
   if (glsl_type_is_vector_or_scalar(dst->type)) {
      nir_store_deref(b, dst, nir_load_deref(b, src), ~0);
   } else if (glsl_type_is_struct(dst->type)) {
      for (unsigned i = 0; i < glsl_get_length(dst->type); i++) {
         copy_output(b, nir_build_deref_struct(b, dst, i),
                     nir_build_deref_struct(b, src, i));
      }
   } else {
      for (unsigned i = 0; i < glsl_get_length(dst->type); i++) {
         copy_output(b, nir_build_deref_array_imm(b, dst, i),
                     nir_build_deref_array_imm(b, src, i));
      }
   }
}

nir_shader *
tu_mesh_build_vs(const nir_shader *ms, const struct tu_mesh_io *io,
                 const nir_shader_compiler_options *options)
{
   nir_builder _b =
      nir_builder_init_simple_shader(MESA_SHADER_VERTEX, options, "tu_mesh_vs");
   nir_builder *b = &_b;
   nir_shader *vs = b->shader;

   vs->info.clip_distance_array_size = ms->info.clip_distance_array_size;
   vs->info.cull_distance_array_size = ms->info.cull_distance_array_size;

   unsigned vpp = io->verts_per_prim;
   nir_def *vertex = nir_load_vertex_id(b);
   nir_def *prim = nir_udiv_imm(b, vertex, vpp);
   nir_def *corner = nir_isub(b, vertex, nir_imul_imm(b, prim, vpp));
   nir_def *wg = nir_udiv_imm(b, prim, io->max_primitives);
   nir_def *p = nir_isub(b, prim, nir_imul_imm(b, wg, io->max_primitives));

   nir_def *rec =
      addr_add(b, load_ring(b),
               nir_iadd_imm(b, nir_imul_imm(b, wg, io->stride),
                            TU_MESH_RECORD_OFFSET));

   nir_def *index_offset =
      nir_iadd_imm(b, nir_imul_imm(b, nir_iadd(b, nir_imul_imm(b, p, vpp),
                                               corner), 4),
                   io->index_offset);
   nir_def *index = nir_load_global(b, 1, 32, addr_add(b, rec, index_offset),
                                    .align_mul = 4);
   nir_def *dead = nir_ieq_imm(b, index, TU_MESH_DEAD_INDEX);
   index = nir_bcsel(b, dead, nir_imm_int(b, 0), index);

   nir_def *vertex_rec =
      addr_add(b, rec, nir_imul_imm(b, index, io->vertex_slots * 16));
   nir_def *prim_rec = addr_add(b, rec, nir_imul_imm(b, p, io->prim_slots * 16));

   bool writes_pos = false;
   nir_foreach_shader_out_variable (ms_var, ms) {
      if (is_indices_or_cull(ms_var))
         continue;

      nir_variable *var = nir_variable_clone(ms_var, vs);
      var->type = glsl_get_array_element(ms_var->type);
      if (var->data.per_primitive) {
         var->data.per_primitive = false;
         var->data.interpolation = INTERP_MODE_FLAT;
      }
      if (var->data.location == VARYING_SLOT_PRIMITIVE_ID)
         var->data.location = TU_MESH_PRIMITIVE_ID_SLOT;
      nir_shader_add_variable(vs, var);

      nir_def *base = ms_var->data.per_primitive ? prim_rec : vertex_rec;
      unsigned offset = var_record_offset(io, ms_var) +
                        (ms_var->data.per_primitive ? io->prim_offset : 0);
      nir_deref_instr *src =
         nir_build_deref_cast_with_alignment(
            b, addr_add(b, base, nir_imm_int(b, offset)), nir_var_mem_global,
            io_explicit_type(var->type, var->data.compact), 0, 16, offset % 16);
      nir_deref_instr *dst = nir_build_deref_var(b, var);

      if (var->data.location == VARYING_SLOT_POS) {
         nir_def *pos = nir_load_deref(b, src);
         nir_store_deref(b, dst,
                         nir_bcsel(b, dead, nir_imm_vec4(b, 2.0, 2.0, 2.0, 1.0),
                                   pos), 0xf);
         writes_pos = true;
      } else {
         copy_output(b, dst, src);
      }
   }

   if (!writes_pos) {
      nir_variable *pos = nir_variable_create(vs, nir_var_shader_out,
                                              glsl_vec4_type(), "gl_Position");
      pos->data.location = VARYING_SLOT_POS;
      nir_store_var(b, pos, nir_imm_vec4(b, 2.0, 2.0, 2.0, 1.0), 0xf);
   }

   nir_shader_gather_info(vs, nir_shader_get_entrypoint(vs));
   return vs;
}

struct table_sysvals {
   nir_intrinsic_instr *hw_workgroup_id;
   nir_intrinsic_instr *hw_base_workgroup_id;
   nir_def *l;
   nir_def *workgroup_id;
   nir_def *num_workgroups;
   nir_def *draw_id;
   nir_def *task_slot;
};

struct lower_sysvals_state {
   struct table_sysvals table;
   nir_variable *counts;
   nir_def *task_header;
};

static bool
lower_sysval_intrinsic(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   struct lower_sysvals_state *state = (struct lower_sysvals_state *) data;
   struct table_sysvals *s = &state->table;
   nir_def *value;

   if (intr == s->hw_workgroup_id || intr == s->hw_base_workgroup_id)
      return false;

   b->cursor = nir_before_instr(&intr->instr);

   switch (intr->intrinsic) {
   case nir_intrinsic_load_workgroup_id:
      value = s->workgroup_id;
      break;
   case nir_intrinsic_load_base_workgroup_id:
      value = nir_imm_zero(b, 3, intr->def.bit_size);
      break;
   case nir_intrinsic_load_num_workgroups:
      value = s->num_workgroups;
      break;
   case nir_intrinsic_load_draw_id:
      value = s->draw_id;
      break;
   case nir_intrinsic_load_global_invocation_id: {
      const uint16_t *size = b->shader->info.workgroup_size;
      value = nir_iadd(b, nir_imul(b, s->workgroup_id,
                                   nir_imm_ivec3(b, size[0], size[1], size[2])),
                       nir_load_local_invocation_id(b));
      break;
   }
   case nir_intrinsic_set_vertex_and_primitive_count: {
      nir_deref_instr *counts = nir_build_deref_var(b, state->counts);
      nir_store_deref(b, nir_build_deref_array_imm(b, counts, 0),
                      intr->src[0].ssa, 0x1);
      nir_store_deref(b, nir_build_deref_array_imm(b, counts, 1),
                      intr->src[1].ssa, 0x1);
      nir_instr_remove(&intr->instr);
      return true;
   }
   case nir_intrinsic_launch_mesh_workgroups: {
      nir_push_if(b, nir_ieq_imm(b, nir_load_local_invocation_index(b), 0));
      nir_store_global(b, nir_vec4(b, nir_channel(b, intr->src[0].ssa, 0),
                                   nir_channel(b, intr->src[0].ssa, 1),
                                   nir_channel(b, intr->src[0].ssa, 2),
                                   s->draw_id),
                       state->task_header, .align_mul = 16);
      nir_pop_if(b, NULL);
      nir_instr_remove(&intr->instr);
      return true;
   }
   case nir_intrinsic_barrier: {
      const nir_variable_mode ring_modes =
         nir_var_shader_out | nir_var_mem_task_payload;
      nir_variable_mode modes = nir_intrinsic_memory_modes(intr);
      if (!(modes & ring_modes))
         return false;
      nir_intrinsic_set_memory_modes(
         intr, (nir_variable_mode)((modes & ~ring_modes) | nir_var_mem_global));
      return true;
   }
   default:
      return false;
   }

   nir_def_replace(&intr->def, nir_trim_vector(b, value, intr->def.num_components));
   return true;
}

/* Finds the table entry holding linear workgroup l: the last entry whose
 * first workgroup is not above l.
 */
static nir_def *
find_table_entry(nir_builder *b, nir_def *ring, unsigned table_offset,
                 nir_def *l)
{
   nir_def *table = addr_add(b, ring, nir_imm_int(b, table_offset));
   nir_def *count = nir_load_global(b, 1, 32, table, .align_mul = 16);

   nir_variable *lo = nir_local_variable_create(b->impl, glsl_uint_type(), "lo");
   nir_variable *hi = nir_local_variable_create(b->impl, glsl_uint_type(), "hi");
   nir_store_var(b, lo, nir_imm_int(b, 0), 0x1);
   nir_store_var(b, hi, nir_iadd_imm(b, nir_umax(b, count, nir_imm_int(b, 1)), -1), 0x1);

   nir_loop *loop = nir_push_loop(b);
   {
      nir_def *lo_v = nir_load_var(b, lo);
      nir_def *hi_v = nir_load_var(b, hi);
      nir_break_if(b, nir_uge(b, lo_v, hi_v));

      nir_def *mid = nir_ushr_imm(b, nir_iadd_imm(b, nir_iadd(b, lo_v, hi_v), 1), 1);
      nir_def *start = nir_load_global(
         b, 1, 32,
         addr_add(b, table,
                  nir_iadd_imm(b, nir_imul_imm(b, mid, TU_MESH_TABLE_ENTRY_SIZE),
                               TU_MESH_TABLE_ENTRIES)),
         .align_mul = 16);
      nir_def *take = nir_uge(b, l, start);
      nir_store_var(b, lo, nir_bcsel(b, take, mid, lo_v), 0x1);
      nir_store_var(b, hi, nir_bcsel(b, take, hi_v, nir_iadd_imm(b, mid, -1)), 0x1);
   }
   nir_pop_loop(b, loop);

   return addr_add(b, table,
                   nir_iadd_imm(b, nir_imul_imm(b, nir_load_var(b, lo),
                                                TU_MESH_TABLE_ENTRY_SIZE),
                                TU_MESH_TABLE_ENTRIES));
}

/* Decodes the API workgroup of this dispatch from the table. The chunk's
 * first linear workgroup is passed as the base workgroup.
 */
static void
load_table_sysvals(nir_builder *b, nir_def *ring, unsigned table_offset,
                   struct table_sysvals *s)
{
   nir_def *hw_id = nir_load_workgroup_id(b);
   nir_def *hw_base = nir_load_base_workgroup_id(b, 32);
   s->hw_workgroup_id = nir_def_as_intrinsic(hw_id);
   s->hw_base_workgroup_id = nir_def_as_intrinsic(hw_base);
   s->l = nir_iadd(b, nir_channel(b, hw_id, 0), nir_channel(b, hw_base, 0));

   nir_def *entry = find_table_entry(b, ring, table_offset, s->l);
   nir_def *e0 = nir_load_global(b, 4, 32, entry, .align_mul = 16);
   nir_def *e1 = nir_load_global(b, 4, 32, addr_add(b, entry, nir_imm_int(b, 16)),
                                 .align_mul = 16);

   nir_def *gx = nir_channel(b, e0, 1), *gy = nir_channel(b, e0, 2);
   nir_def *local = nir_isub(b, s->l, nir_channel(b, e0, 0));
   nir_def *row = nir_udiv(b, local, gx);

   s->workgroup_id = nir_vec3(b, nir_umod(b, local, gx), nir_umod(b, row, gy),
                              nir_udiv(b, row, gy));
   s->num_workgroups = nir_channels(b, e0, 0xe);
   s->draw_id = nir_channel(b, e1, 0);
   s->task_slot = nir_channel(b, e1, 1);
}

static void
emit_workgroup_loop(nir_builder *b, unsigned count, unsigned wg_size,
                    void (*body)(nir_builder *, nir_def *, void *), void *data)
{
   nir_variable *i = nir_local_variable_create(b->impl, glsl_uint_type(), "i");
   nir_store_var(b, i, nir_load_local_invocation_index(b), 0x1);

   nir_loop *loop = nir_push_loop(b);
   {
      nir_def *i_v = nir_load_var(b, i);
      nir_break_if(b, nir_uge_imm(b, i_v, count));
      body(b, i_v, data);
      nir_store_var(b, i, nir_iadd_imm(b, i_v, wg_size), 0x1);
   }
   nir_pop_loop(b, loop);
}

static void
emit_workgroup_barrier(nir_builder *b)
{
   nir_barrier(b, .execution_scope = SCOPE_WORKGROUP,
               .memory_scope = SCOPE_WORKGROUP,
               .memory_semantics = NIR_MEMORY_ACQ_REL,
               .memory_modes = (nir_variable_mode)(nir_var_mem_shared |
                                                   nir_var_mem_global));
}

struct ms_epilogue {
   const struct tu_mesh_io *io;
   nir_def *rec;
   nir_def *prim_count;
   bool has_cull;
};

static void
clear_cull(nir_builder *b, nir_def *p, void *data)
{
   struct ms_epilogue *e = (struct ms_epilogue *) data;
   nir_store_global(b, nir_imm_int(b, 0),
                    addr_add(b, e->rec, nir_iadd_imm(b, nir_imul_imm(b, p, 4),
                                                     e->io->cull_offset)),
                    .write_mask = 0x1, .align_mul = 4);
}

static void
write_indices(nir_builder *b, nir_def *p, void *data)
{
   struct ms_epilogue *e = (struct ms_epilogue *) data;
   const struct tu_mesh_io *io = e->io;

   nir_def *live = nir_ult(b, p, e->prim_count);
   if (e->has_cull) {
      nir_def *cull = nir_load_global(
         b, 1, 32,
         addr_add(b, e->rec, nir_iadd_imm(b, nir_imul_imm(b, p, 4),
                                          io->cull_offset)),
         .align_mul = 4);
      live = nir_iand(b, live, nir_ieq_imm(b, cull, 0));
   }

   nir_def *addr =
      addr_add(b, e->rec,
               nir_iadd_imm(b, nir_imul_imm(b, p, io->verts_per_prim * 4),
                            io->index_offset));
   nir_def *index = nir_load_global(b, io->verts_per_prim, 32, addr,
                                    .align_mul = 4);
   index = nir_umin(b, index, nir_imm_int(b, io->max_vertices - 1));
   index = nir_bcsel(b, live, index, nir_imm_int(b, TU_MESH_DEAD_INDEX));
   nir_store_global(b, index, addr,
                    .write_mask = nir_component_mask(io->verts_per_prim),
                    .align_mul = 4);
}

static nir_def *
task_slot_addr(nir_builder *b, nir_def *ring, nir_def *slot,
               unsigned payload_stride)
{
   return addr_add(b, ring,
                   nir_iadd_imm(b, nir_imul_imm(b, slot, payload_stride),
                                TU_MESH_TASK_OFFSET));
}

static nir_def *
task_payload_addr(nir_builder *b, nir_def *ring, nir_def *slot,
                  unsigned payload_stride)
{
   return addr_add(b, task_slot_addr(b, ring, slot, payload_stride),
                   nir_imm_int(b, TU_MESH_TASK_HEADER_SIZE));
}

static bool
lower_task_payload_access(nir_builder *b, nir_intrinsic_instr *intr,
                          void *data)
{
   nir_def *base = (nir_def *) data;

   b->cursor = nir_before_instr(&intr->instr);

   switch (intr->intrinsic) {
   case nir_intrinsic_load_task_payload: {
      nir_def *addr = addr_add(b, base, nir_iadd_imm(b, intr->src[0].ssa,
                                                     nir_intrinsic_base(intr)));
      nir_def_replace(&intr->def,
                      nir_load_global(b, intr->def.num_components,
                                      intr->def.bit_size, addr,
                                      .align_mul = nir_intrinsic_align_mul(intr),
                                      .align_offset = nir_intrinsic_align_offset(intr)));
      return true;
   }
   case nir_intrinsic_store_task_payload: {
      nir_def *addr = addr_add(b, base, nir_iadd_imm(b, intr->src[1].ssa,
                                                     nir_intrinsic_base(intr)));
      nir_store_global(b, intr->src[0].ssa, addr,
                       .write_mask = nir_intrinsic_write_mask(intr),
                       .align_mul = nir_intrinsic_align_mul(intr),
                       .align_offset = nir_intrinsic_align_offset(intr));
      nir_instr_remove(&intr->instr);
      return true;
   }
   case nir_intrinsic_task_payload_atomic:
   case nir_intrinsic_task_payload_atomic_swap: {
      bool swap = intr->intrinsic == nir_intrinsic_task_payload_atomic_swap;
      nir_def *addr = addr_add(b, base, nir_iadd_imm(b, intr->src[0].ssa,
                                                     nir_intrinsic_base(intr)));
      nir_def *result =
         swap ? nir_global_atomic_swap(b, intr->def.bit_size, addr,
                                       intr->src[1].ssa, intr->src[2].ssa,
                                       .atomic_op = nir_intrinsic_atomic_op(intr))
              : nir_global_atomic(b, intr->def.bit_size, addr, intr->src[1].ssa,
                                  .atomic_op = nir_intrinsic_atomic_op(intr));
      nir_def_replace(&intr->def, result);
      return true;
   }
   default:
      return false;
   }
}

static void
lower_task_payload_vars(nir_shader *nir)
{
   NIR_PASS(_, nir, nir_lower_vars_to_explicit_types, nir_var_mem_task_payload,
            glsl_get_natural_size_align_bytes);
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_task_payload,
            nir_address_format_32bit_offset);
}

static void
become_compute(nir_shader *nir)
{
   nir->info.stage = MESA_SHADER_COMPUTE;
   nir->info.next_stage = MESA_SHADER_NONE;
   memset(&nir->info.cs, 0, sizeof(nir->info.cs));
   nir->info.inputs_read = 0;
   nir->info.outputs_written = 0;
   nir->info.outputs_read = 0;
   nir->info.per_primitive_outputs = 0;
   nir->info.clip_distance_array_size = 0;
   nir->info.cull_distance_array_size = 0;
}

void
tu_mesh_lower_ms(nir_shader *ms, const struct tu_mesh_io *io,
                 unsigned task_payload_stride)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(ms);
   nir_builder _b = nir_builder_at(nir_before_impl(impl));
   nir_builder *b = &_b;

   unsigned wg_size = ms->info.workgroup_size[0] * ms->info.workgroup_size[1] *
                      ms->info.workgroup_size[2];

   nir_def *ring = load_ring(b);

   struct lower_sysvals_state sysvals = {
      .counts = nir_variable_create(ms, nir_var_mem_shared,
                                    glsl_array_type(glsl_uint_type(), 2, 4),
                                    "tu_mesh_counts"),
   };
   load_table_sysvals(b, ring, TU_MESH_MS_TABLE_OFFSET, &sysvals.table);
   nir_def *l = sysvals.table.l;

   nir_def *rec =
      addr_add(b, ring,
               nir_iadd_imm(b, nir_imul_imm(b, nir_umod_imm(b, l, io->chunk_workgroups),
                                            io->stride),
                            TU_MESH_RECORD_OFFSET));

   struct ms_epilogue epilogue = { .io = io, .rec = rec };

   nir_variable *indices = NULL;
   nir_foreach_shader_out_variable (var, ms) {
      if (var->data.location == VARYING_SLOT_PRIMITIVE_INDICES)
         indices = var;
      else if (var->data.location == VARYING_SLOT_CULL_PRIMITIVE)
         epilogue.has_cull = true;
   }

   nir_push_if(b, nir_ieq_imm(b, nir_load_local_invocation_index(b), 0));
   {
      nir_store_array_var_imm(b, sysvals.counts, 0, nir_imm_int(b, 0), 0x1);
      nir_store_array_var_imm(b, sysvals.counts, 1, nir_imm_int(b, 0), 0x1);
   }
   nir_pop_if(b, NULL);
   if (epilogue.has_cull)
      emit_workgroup_loop(b, io->max_primitives, wg_size, clear_cull, &epilogue);
   emit_workgroup_barrier(b);

   nir_def *payload =
      task_payload_addr(b, ring, sysvals.table.task_slot, task_payload_stride);

   nir_shader_intrinsics_pass(ms, lower_sysval_intrinsic, nir_metadata_none,
                              &sysvals);

   nir_foreach_variable_with_modes_safe (var, ms, nir_var_shader_out) {
      nir_builder vb = nir_builder_at(nir_after_instr(nir_def_instr(rec)));
      nir_def *addr;
      const glsl_type *type;
      unsigned align_offset = 0;

      if (var == indices) {
         addr = addr_add(&vb, rec, nir_imm_int(&vb, io->index_offset));
         type = glsl_array_type(glsl_get_array_element(var->type),
                                glsl_get_length(var->type),
                                io->verts_per_prim * 4);
      } else if (var->data.location == VARYING_SLOT_CULL_PRIMITIVE) {
         addr = addr_add(&vb, rec, nir_imm_int(&vb, io->cull_offset));
         type = glsl_array_type(glsl_get_array_element(var->type),
                                glsl_get_length(var->type), 4);
      } else {
         addr = record_array_addr(&vb, rec, io, var);
         type = record_array_type(io, var);
         align_offset = var_record_offset(io, var) % 16;
      }

      retarget_var(impl, var, addr, type, align_offset);
   }

   b->cursor = nir_after_impl(impl);
   emit_workgroup_barrier(b);
   epilogue.prim_count =
      nir_umin(b, nir_load_array_var_imm(b, sysvals.counts, 1),
               nir_imm_int(b, io->max_primitives));
   emit_workgroup_loop(b, io->max_primitives, wg_size, write_indices, &epilogue);

   lower_task_payload_vars(ms);
   nir_shader_intrinsics_pass(ms, lower_task_payload_access,
                              nir_metadata_control_flow, payload);

   NIR_PASS(_, ms, nir_remove_dead_derefs);
   NIR_PASS(_, ms, nir_remove_dead_variables,
            nir_var_shader_out | nir_var_mem_task_payload, NULL);
   NIR_PASS(_, ms, nir_lower_vars_to_ssa);
   NIR_PASS(_, ms, nir_opt_dce);

   become_compute(ms);
}

unsigned
tu_mesh_task_chunk(unsigned task_payload_stride)
{
   return MIN2(TU_MESH_TASK_SIZE / task_payload_stride,
               TU_MESH_TABLE_MAX_ENTRIES);
}

unsigned
tu_mesh_lower_ts(nir_shader *ts)
{
   lower_task_payload_vars(ts);
   nir_lower_task_shader_options options = {};
   NIR_PASS(_, ts, nir_lower_task_shader, options);

   unsigned stride =
      align(TU_MESH_TASK_HEADER_SIZE + ts->info.task_payload_size, 16);

   nir_function_impl *impl = nir_shader_get_entrypoint(ts);
   nir_builder _b = nir_builder_at(nir_before_impl(impl));
   nir_builder *b = &_b;

   nir_def *ring = load_ring(b);

   struct lower_sysvals_state sysvals = {};
   load_table_sysvals(b, ring, TU_MESH_TS_TABLE_OFFSET, &sysvals.table);

   nir_def *slot = nir_umod_imm(b, sysvals.table.l, tu_mesh_task_chunk(stride));
   sysvals.task_header = task_slot_addr(b, ring, slot, stride);
   nir_def *payload = task_payload_addr(b, ring, slot, stride);

   /* A workgroup that launches nothing leaves an empty header. */
   nir_push_if(b, nir_ieq_imm(b, nir_load_local_invocation_index(b), 0));
   {
      nir_store_global(b, nir_vec4(b, nir_imm_int(b, 0), nir_imm_int(b, 0),
                                   nir_imm_int(b, 0), sysvals.table.draw_id),
                       sysvals.task_header, .align_mul = 16);
   }
   nir_pop_if(b, NULL);

   nir_shader_intrinsics_pass(ts, lower_sysval_intrinsic, nir_metadata_none,
                              &sysvals);
   nir_shader_intrinsics_pass(ts, lower_task_payload_access,
                              nir_metadata_control_flow, payload);

   NIR_PASS(_, ts, nir_remove_dead_variables, nir_var_mem_task_payload, NULL);
   NIR_PASS(_, ts, nir_lower_vars_to_ssa);
   NIR_PASS(_, ts, nir_opt_dce);

   become_compute(ts);
   return stride;
}

void
tu_mesh_lower_fs_inputs(nir_shader *fs, bool remap_primitive_id)
{
   nir_foreach_shader_in_variable (var, fs) {
      if (var->data.per_primitive) {
         var->data.per_primitive = false;
         var->data.interpolation = INTERP_MODE_FLAT;
      }

      if (remap_primitive_id &&
          var->data.location == VARYING_SLOT_PRIMITIVE_ID)
         var->data.location = TU_MESH_PRIMITIVE_ID_SLOT;
   }

   fs->info.per_primitive_inputs = 0;
}

struct setup_state {
   nir_def *ring;
   nir_def *table;
   nir_def *params[TU_MESH_PARAM_NUM];
   nir_def *count;
};

/* Returns the launch dimensions and draw id of source item i. */
static nir_def *
setup_load_item(nir_builder *b, struct setup_state *s, nir_def *i)
{
   nir_def *task = nir_ieq_imm(b, s->params[TU_MESH_PARAM_SOURCE],
                               TU_MESH_SOURCE_TASK);
   nir_def *draw = nir_iadd(b, s->params[TU_MESH_PARAM_FIRST], i);

   nir_def *indirect_value, *task_value;
   nir_push_if(b, task);
   {
      nir_def *addr =
         addr_add(b, s->ring,
                  nir_iadd_imm(b, nir_imul(b, i, s->params[TU_MESH_PARAM_STRIDE]),
                               TU_MESH_TASK_OFFSET));
      task_value = nir_load_global(b, 4, 32, addr, .align_mul = 16);
   }
   nir_push_else(b, NULL);
   {
      nir_def *src = nir_pack_64_2x32_split(b, s->params[TU_MESH_PARAM_SRC_LO],
                                            s->params[TU_MESH_PARAM_SRC_HI]);
      nir_def *addr =
         nir_iadd(b, src, nir_imul(b, nir_u2u64(b, draw),
                                   nir_u2u64(b, s->params[TU_MESH_PARAM_STRIDE])));
      nir_def *dims = nir_load_global(b, 3, 32, addr, .align_mul = 4);
      indirect_value = nir_vec4(b, nir_channel(b, dims, 0),
                                nir_channel(b, dims, 1),
                                nir_channel(b, dims, 2), draw);
   }
   nir_pop_if(b, NULL);

   return nir_if_phi(b, task_value, indirect_value);
}

static nir_def *
item_groups(nir_builder *b, nir_def *item)
{
   return nir_imul(b, nir_imul(b, nir_channel(b, item, 0),
                               nir_channel(b, item, 1)),
                   nir_channel(b, item, 2));
}

/* Runs body(i) for every item of this invocation's contiguous range. */
static void
setup_range_loop(nir_builder *b, struct setup_state *s, nir_variable *start,
                 bool write)
{
   nir_def *t = nir_load_local_invocation_index(b);
   nir_def *per_thread =
      nir_udiv_imm(b, nir_iadd_imm(b, s->count, TU_MESH_SETUP_WORKGROUP_SIZE - 1),
                   TU_MESH_SETUP_WORKGROUP_SIZE);
   nir_def *begin = nir_umin(b, nir_imul(b, t, per_thread), s->count);
   nir_def *end = nir_umin(b, nir_iadd(b, begin, per_thread), s->count);

   nir_variable *i = nir_local_variable_create(b->impl, glsl_uint_type(), "i");
   nir_store_var(b, i, begin, 0x1);

   nir_loop *loop = nir_push_loop(b);
   {
      nir_def *i_v = nir_load_var(b, i);
      nir_break_if(b, nir_uge(b, i_v, end));

      nir_def *item = setup_load_item(b, s, i_v);
      nir_def *groups = item_groups(b, item);
      nir_def *start_v = nir_load_var(b, start);

      if (write) {
         nir_def *task = nir_ieq_imm(b, s->params[TU_MESH_PARAM_SOURCE],
                                     TU_MESH_SOURCE_TASK);
         nir_def *entry =
            addr_add(b, s->table,
                     nir_iadd_imm(b, nir_imul_imm(b, i_v, TU_MESH_TABLE_ENTRY_SIZE),
                                  TU_MESH_TABLE_ENTRIES));
         nir_store_global(b, nir_vec4(b, start_v, nir_channel(b, item, 0),
                                      nir_channel(b, item, 1),
                                      nir_channel(b, item, 2)),
                          entry, .align_mul = 16);
         nir_store_global(b, nir_vec4(b, nir_channel(b, item, 3),
                                      nir_bcsel(b, task, i_v, nir_imm_int(b, 0)),
                                      nir_imm_int(b, 0), nir_imm_int(b, 0)),
                          addr_add(b, entry, nir_imm_int(b, 16)),
                          .align_mul = 16);
      }

      nir_store_var(b, start, nir_iadd(b, start_v, groups), 0x1);
      nir_store_var(b, i, nir_iadd_imm(b, i_v, 1), 0x1);
   }
   nir_pop_loop(b, loop);
}

/* Builds a table (entries sorted by first workgroup) from indirect commands
 * or task launches, and the per-chunk dispatch and draw arguments.
 */
nir_shader *
tu_mesh_build_setup_cs(const nir_shader_compiler_options *options)
{
   nir_builder _b =
      nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, options,
                                     "tu_mesh_setup");
   nir_builder *b = &_b;
   b->shader->info.workgroup_size[0] = TU_MESH_SETUP_WORKGROUP_SIZE;
   b->shader->info.workgroup_size[1] = 1;
   b->shader->info.workgroup_size[2] = 1;

   struct setup_state s = {};
   s.ring = load_ring(b);
   for (unsigned i = 0; i < TU_MESH_PARAM_NUM; i += 4) {
      nir_def *v = nir_load_global(
         b, MIN2(4, TU_MESH_PARAM_NUM - i), 32,
         addr_add(b, s.ring, nir_imm_int(b, TU_MESH_PARAMS_OFFSET + i * 4)),
         .align_mul = 16);
      for (unsigned c = 0; c < v->num_components; c++)
         s.params[i + c] = nir_channel(b, v, c);
   }
   s.table = addr_add(b, s.ring, s.params[TU_MESH_PARAM_TABLE]);

   nir_def *first = s.params[TU_MESH_PARAM_FIRST];
   nir_def *count = s.params[TU_MESH_PARAM_COUNT];
   s.count = nir_umin(b, nir_bcsel(b, nir_ult(b, first, count),
                                   nir_isub(b, count, first), nir_imm_int(b, 0)),
                      s.params[TU_MESH_PARAM_MAX_COUNT]);

   nir_variable *sums = nir_variable_create(
      b->shader, nir_var_mem_shared,
      glsl_array_type(glsl_uint_type(), TU_MESH_SETUP_WORKGROUP_SIZE + 1, 4),
      "sums");
   nir_variable *start = nir_local_variable_create(b->impl, glsl_uint_type(), "start");
   nir_def *t = nir_load_local_invocation_index(b);

   nir_store_var(b, start, nir_imm_int(b, 0), 0x1);
   setup_range_loop(b, &s, start, false);
   nir_store_array_var(b, sums, t, nir_load_var(b, start), 0x1);
   emit_workgroup_barrier(b);

   nir_push_if(b, nir_ieq_imm(b, t, 0));
   {
      nir_variable *i = nir_local_variable_create(b->impl, glsl_uint_type(), "i");
      nir_variable *sum = nir_local_variable_create(b->impl, glsl_uint_type(), "sum");
      nir_store_var(b, i, nir_imm_int(b, 0), 0x1);
      nir_store_var(b, sum, nir_imm_int(b, 0), 0x1);

      nir_loop *loop = nir_push_loop(b);
      {
         nir_def *i_v = nir_load_var(b, i);
         nir_break_if(b, nir_uge_imm(b, i_v, TU_MESH_SETUP_WORKGROUP_SIZE));
         nir_def *sum_v = nir_load_var(b, sum);
         nir_def *v = nir_load_array_var(b, sums, i_v);
         nir_store_array_var(b, sums, i_v, sum_v, 0x1);
         nir_store_var(b, sum, nir_iadd(b, sum_v, v), 0x1);
         nir_store_var(b, i, nir_iadd_imm(b, i_v, 1), 0x1);
      }
      nir_pop_loop(b, loop);

      nir_store_array_var_imm(b, sums, TU_MESH_SETUP_WORKGROUP_SIZE,
                              nir_load_var(b, sum), 0x1);
   }
   nir_pop_if(b, NULL);
   emit_workgroup_barrier(b);

   nir_store_var(b, start, nir_load_array_var(b, sums, t), 0x1);
   setup_range_loop(b, &s, start, true);

   nir_def *total = nir_load_array_var_imm(b, sums, TU_MESH_SETUP_WORKGROUP_SIZE);
   nir_push_if(b, nir_ieq_imm(b, t, 0));
   {
      nir_store_global(b, s.count, s.table, .align_mul = 16);
   }
   nir_pop_if(b, NULL);

   nir_push_if(b, nir_ult(b, t, s.params[TU_MESH_PARAM_CHUNKS]));
   {
      nir_def *chunk = s.params[TU_MESH_PARAM_CHUNK];
      nir_def *base = nir_imul(b, t, chunk);
      nir_def *left = nir_bcsel(b, nir_ult(b, base, total),
                                nir_isub(b, total, base), nir_imm_int(b, 0));
      nir_def *groups = nir_umin(b, left, chunk);
      nir_def *args =
         addr_add(b, s.table,
                  nir_iadd_imm(b, nir_imul_imm(b, t, TU_MESH_ARGS_SIZE),
                               TU_MESH_TABLE_ARGS));
      nir_store_global(b, nir_vec4(b, groups, nir_imm_int(b, 1), nir_imm_int(b, 1),
                                   nir_b2i32(b, nir_ine_imm(b, groups, 0))),
                       args, .align_mul = 16);
      nir_store_global(b, nir_vec4(b, nir_imul(b, groups,
                                               s.params[TU_MESH_PARAM_VERTICES]),
                                   nir_imm_int(b, 1), nir_imm_int(b, 0),
                                   nir_imm_int(b, 0)),
                       addr_add(b, args, nir_imm_int(b, TU_MESH_ARG_DRAW)),
                       .align_mul = 16);
   }
   nir_pop_if(b, NULL);

   NIR_PASS(_, b->shader, nir_lower_vars_to_ssa);
   return b->shader;
}
