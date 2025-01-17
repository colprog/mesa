/*
 * Copyright © 2016 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 */

#ifndef SHADER_INFO_H
#define SHADER_INFO_H

#include "shader_enums.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct shader_info {
   const char *name;

   /* Descriptive name provided by the client; may be NULL */
   const char *label;

   /* Number of textures used by this shader */
   unsigned num_textures;
   /* Number of uniform buffers used by this shader */
   unsigned num_ubos;
   /* Number of atomic buffers used by this shader */
   unsigned num_abos;
   /* Number of shader storage buffers used by this shader */
   unsigned num_ssbos;
   /* Number of images used by this shader */
   unsigned num_images;

   /* Which inputs are actually read */
   uint64_t inputs_read;
   /* Which inputs are actually read and are double */
   uint64_t double_inputs_read;
   /* Which outputs are actually written */
   uint64_t outputs_written;
   /* Which outputs are actually read */
   uint64_t outputs_read;
   /* Which system values are actually read */
   uint64_t system_values_read;

   /* Which patch inputs are actually read */
   uint32_t patch_inputs_read;
   /* Which patch outputs are actually written */
   uint32_t patch_outputs_written;

   /* Whether or not this shader ever uses textureGather() */
   bool uses_texture_gather;

   /* The size of the gl_ClipDistance[] array, if declared. */
   unsigned clip_distance_array_size;

   /* The size of the gl_CullDistance[] array, if declared. */
   unsigned cull_distance_array_size;

   /* Whether or not separate shader objects were used */
   bool separate_shader;

   /** Was this shader linked with any transform feedback varyings? */
   bool has_transform_feedback_varyings;

   union {
      struct {
         /** The number of vertices recieves per input primitive */
         unsigned vertices_in;

         /** The output primitive type (GL enum value) */
         unsigned output_primitive;

         /** The input primitive type (GL enum value) */
         unsigned input_primitive;

         /** The maximum number of vertices the geometry shader might write. */
         unsigned vertices_out;

         /** 1 .. MAX_GEOMETRY_SHADER_INVOCATIONS */
         unsigned invocations;

         /** Whether or not this shader uses EndPrimitive */
         bool uses_end_primitive;

         /** Whether or not this shader uses non-zero streams */
         bool uses_streams;
      } gs;

      struct {
         bool uses_discard;

         /**
          * Whether any inputs are declared with the "sample" qualifier.
          */
         bool uses_sample_qualifier;

         /**
          * Whether early fragment tests are enabled as defined by
          * ARB_shader_image_load_store.
          */
         bool early_fragment_tests;

         /** gl_FragDepth layout for ARB_conservative_depth. */
         enum gl_frag_depth_layout depth_layout;
      } fs;

      struct {
         unsigned local_size[3];

         /**
          * Size of shared variables accessed by the compute shader.
          */
         unsigned shared_size;
      } cs;

      struct {
         /** The number of vertices in the TCS output patch. */
         unsigned vertices_out;
      } tcs;

      struct {
         uint32_t primitive_mode; /* GL_TRIANGLES, GL_QUADS or GL_ISOLINES */
         uint32_t spacing;        /* GL_EQUAL, GL_FRACTIONAL_EVEN, GL_FRACTIONAL_ODD */
         uint32_t vertex_order;   /* GL_CW or GL_CCW */
         bool point_mode;
      } tes;
   };
} shader_info;

#ifdef __cplusplus
}
#endif

#endif /* SHADER_INFO_H */
