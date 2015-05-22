/* Copyright 2014-2015 Samsung Electronics Co., Ltd.
 * Copyright 2015 University of Szeged.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "ecma-helpers.h"
#include "jrt-libc-includes.h"
#include "mem-heap.h"
#include "re-compiler.h"
#include "stdio.h"

#define REGEXP_BYTECODE_BLOCK_SIZE 256UL
#define BYTECODE_LEN(bc_ctx_p) (static_cast<uint32_t> (bc_ctx_p->current_p - bc_ctx_p->block_start_p))

void
regexp_dump_bytecode (bytecode_ctx_t *bc_ctx);

static regexp_bytecode_t*
realloc_regexp_bytecode_block (bytecode_ctx_t *bc_ctx)
{
  JERRY_ASSERT (bc_ctx->block_end_p - bc_ctx->block_start_p >= 0);
  size_t old_size = static_cast<size_t> (bc_ctx->block_end_p - bc_ctx->block_start_p);
  JERRY_ASSERT (!bc_ctx->current_p && !bc_ctx->block_end_p && !bc_ctx->block_start_p);

  size_t new_block_size = old_size + REGEXP_BYTECODE_BLOCK_SIZE;
  JERRY_ASSERT (bc_ctx->current_p - bc_ctx->block_start_p >= 0);
  size_t current_ptr_offset = static_cast<size_t> (bc_ctx->current_p - bc_ctx->block_start_p);

  regexp_bytecode_t *new_block_start_p = (regexp_bytecode_t *) mem_heap_alloc_block (new_block_size,
                                                                                     MEM_HEAP_ALLOC_SHORT_TERM);
  if (bc_ctx->current_p)
  {
    memcpy (new_block_start_p, bc_ctx->block_start_p, static_cast<size_t> (current_ptr_offset));
    mem_heap_free_block (bc_ctx->block_start_p);
  }
  bc_ctx->block_start_p = new_block_start_p;
  bc_ctx->block_end_p = new_block_start_p + new_block_size;
  bc_ctx->current_p = new_block_start_p + current_ptr_offset;

  return bc_ctx->current_p;
} /* realloc_regexp_bytecode_block */

static void
bytecode_list_append (bytecode_ctx_t *bc_ctx, regexp_bytecode_t bytecode)
{
  regexp_bytecode_t *current_p = bc_ctx->current_p;
  if (current_p  + sizeof (regexp_bytecode_t) > bc_ctx->block_end_p)
  {
    current_p = realloc_regexp_bytecode_block (bc_ctx);
  }

  *current_p = bytecode;
  bc_ctx->current_p += sizeof (regexp_bytecode_t);
} /* bytecode_list_append */

static void
bytecode_list_insert (bytecode_ctx_t *bc_ctx, regexp_bytecode_t bytecode, size_t offset)
{
  regexp_bytecode_t *current_p = bc_ctx->current_p;
  if (current_p  + sizeof (regexp_bytecode_t) > bc_ctx->block_end_p)
  {
    realloc_regexp_bytecode_block (bc_ctx);
  }

  regexp_bytecode_t *src = bc_ctx->block_start_p + offset;
  regexp_bytecode_t *dest = src + sizeof (regexp_bytecode_t);

  regexp_bytecode_t *tmp_block_start_p = (regexp_bytecode_t *) mem_heap_alloc_block ((BYTECODE_LEN (bc_ctx) - offset),
                                                                                     MEM_HEAP_ALLOC_SHORT_TERM);
  memcpy (tmp_block_start_p, src, (size_t) (BYTECODE_LEN (bc_ctx) - offset));
  memcpy (dest, tmp_block_start_p, (size_t) (BYTECODE_LEN (bc_ctx) - offset));
  mem_heap_free_block (tmp_block_start_p);

  *src = bytecode;
  bc_ctx->current_p += sizeof (regexp_bytecode_t);
} /* bytecode_list_insert  */

static void
append_opcode (bytecode_ctx_t *bc_ctx,
               regexp_opcode_t opcode)
{
  bytecode_list_append (bc_ctx, (regexp_bytecode_t) opcode);
}

static void
append_u32 (bytecode_ctx_t *bc_ctx,
            uint32_t value)
{
  bytecode_list_append (bc_ctx, (regexp_bytecode_t) value);
}

static void
insert_opcode (bytecode_ctx_t *bc_ctx,
               uint32_t offset,
               regexp_opcode_t opcode)
{
  bytecode_list_insert (bc_ctx, (regexp_bytecode_t) opcode, offset);
}

static void
insert_u32 (bytecode_ctx_t *bc_ctx,
            uint32_t offset,
            uint32_t value)
{
  bytecode_list_insert (bc_ctx, (regexp_bytecode_t) value, offset);
}

regexp_opcode_t
get_opcode (regexp_bytecode_t **bc_p)
{
  regexp_bytecode_t bytecode = **bc_p;
  (*bc_p)++;
  return (regexp_opcode_t) bytecode;
}

uint32_t
get_value (regexp_bytecode_t **bc_p)
{
  /* FIXME: Read 32bit! */
  regexp_bytecode_t bytecode = **bc_p;
  (*bc_p)++;
  return (uint32_t) bytecode;
}

static void
insert_simple_iterator (regexp_compiler_ctx *re_ctx_p,
                        uint32_t new_atom_start_offset)
{
  uint32_t atom_code_length;
  uint32_t offset;
  uint32_t qmin, qmax;

  qmin = re_ctx_p->current_token_p->qmin;
  qmax = re_ctx_p->current_token_p->qmax;
  JERRY_ASSERT (qmin <= qmax);

  /* FIXME: optimize bytecode lenght. Store 0 rather than INF */

  append_opcode (re_ctx_p->bytecode_ctx_p, RE_OP_MATCH);   /* complete 'sub atom' */
  uint32_t bytecode_length = BYTECODE_LEN (re_ctx_p->bytecode_ctx_p);
  atom_code_length = (uint32_t) (bytecode_length - new_atom_start_offset);

  offset = new_atom_start_offset;
  insert_u32 (re_ctx_p->bytecode_ctx_p, offset, atom_code_length);
  insert_u32 (re_ctx_p->bytecode_ctx_p, offset, qmax);
  insert_u32 (re_ctx_p->bytecode_ctx_p, offset, qmin);
  if (re_ctx_p->current_token_p->greedy)
  {
    insert_opcode (re_ctx_p->bytecode_ctx_p, offset, RE_OP_GREEDY_ITERATOR);
  }
  else
  {
    insert_opcode (re_ctx_p->bytecode_ctx_p, offset, RE_OP_NON_GREEDY_ITERATOR);
  }
} /* insert_simple_iterator */

static void
parse_alternative (regexp_compiler_ctx *re_ctx_p,
                   bool expect_eof)
{
  re_token_t re_tok;
  re_ctx_p->current_token_p = &re_tok;
  ecma_char_t *pattern_p = re_ctx_p->pattern_p;
  bytecode_ctx_t *bc_ctx_p = re_ctx_p->bytecode_ctx_p;

  uint32_t alterantive_offset = BYTECODE_LEN (re_ctx_p->bytecode_ctx_p);

  while (true)
  {
    re_tok = re_parse_next_token (&pattern_p);
    uint32_t new_atom_start_offset = BYTECODE_LEN (re_ctx_p->bytecode_ctx_p);

    switch (re_tok.type)
    {
      case RE_TOK_CHAR:
      {
        JERRY_DDLOG ("Compile character token: %c, qmin: %d, qmax: %d\n",
                     re_tok.value, re_tok.qmin, re_tok.qmax);
        append_opcode (bc_ctx_p, RE_OP_CHAR);
        append_u32 (bc_ctx_p, re_tok.value);
        if ((re_tok.qmin != 1) || (re_tok.qmax != 1))
        {
          insert_simple_iterator (re_ctx_p, new_atom_start_offset);
        }
        break;
      }
      case RE_TOK_PERIOD:
      {
        JERRY_DDLOG ("Compile a period\n");
        append_opcode (bc_ctx_p, RE_OP_PERIOD);
        break;
      }
      case RE_TOK_ALTERNATIVE:
      {
        insert_u32 (bc_ctx_p, alterantive_offset, BYTECODE_LEN (re_ctx_p->bytecode_ctx_p) - alterantive_offset);
        append_opcode (bc_ctx_p, RE_OP_ALTERNATIVE);
        alterantive_offset = BYTECODE_LEN (re_ctx_p->bytecode_ctx_p);
        break;
      }
      case RE_TOK_END_GROUP:
      {
        if (expect_eof)
        {
          // FIXME: throw and error!
        }
        insert_u32 (bc_ctx_p, alterantive_offset, BYTECODE_LEN (re_ctx_p->bytecode_ctx_p) - alterantive_offset);
        return;
      }
      case RE_TOK_EOF:
      {
        if (!expect_eof)
        {
          // FIXME: throw and error!
        }
        insert_u32 (bc_ctx_p, alterantive_offset, BYTECODE_LEN (re_ctx_p->bytecode_ctx_p) - alterantive_offset);
        return;
      }
      default:
      {
        break;
      }
    }
  }
} /* parse_alternative */

/**
 * Compilation of RegExp bytecode
 */
void
regexp_compile_bytecode (ecma_property_t *bytecode, /**< bytecode */
                         ecma_string_t *pattern __attr_unused___) /**< pattern */
{
  regexp_compiler_ctx re_ctx;
  bytecode_ctx_t bc_ctx;
  bc_ctx.block_start_p = NULL;
  bc_ctx.block_end_p = NULL;
  bc_ctx.current_p = NULL;

  re_ctx.bytecode_ctx_p = &bc_ctx;

  int32_t chars = ecma_string_get_length (pattern);
  MEM_DEFINE_LOCAL_ARRAY (pattern_start_p, chars + 1, ecma_char_t);
  ssize_t zt_str_size = (ssize_t) sizeof (ecma_char_t) * (chars + 1);
  ecma_string_to_zt_string (pattern, pattern_start_p, zt_str_size);
  re_ctx.pattern_p = pattern_start_p;

  /* 1. Add extra informations for bytecode header */
  append_u32 (&bc_ctx, 0);
  append_u32 (&bc_ctx, 2); // FIXME: Count the number of capture groups
  append_u32 (&bc_ctx, 0); // FIXME: Count the number of non-capture groups

  /* 2. Parse RegExp pattern */
  append_opcode (&bc_ctx, RE_OP_SAVE_AT_START);
  parse_alternative (&re_ctx, true);
  append_opcode (&bc_ctx, RE_OP_SAVE_AND_MATCH);
  append_opcode (&bc_ctx, RE_OP_EOF);

  MEM_FINALIZE_LOCAL_ARRAY (pattern_start_p);

  ECMA_SET_POINTER (bytecode->u.internal_property.value, bc_ctx.block_start_p);

#ifdef JERRY_ENABLE_LOG
  regexp_dump_bytecode (&bc_ctx);
#endif
} /* regexp_compile_bytecode */

#ifdef JERRY_ENABLE_LOG
/**
 * RegExp bytecode dumper
 */
void
regexp_dump_bytecode (bytecode_ctx_t *bc_ctx)
{
  regexp_bytecode_t *bytecode_p = bc_ctx->block_start_p;
  JERRY_DLOG ("%d %d %d | ",
              get_value (&bytecode_p),
              get_value (&bytecode_p),
              get_value (&bytecode_p));

  regexp_opcode_t op;
  while ((op = get_opcode (&bytecode_p)))
  {
    switch (op)
    {
      case RE_OP_MATCH:
      {
        JERRY_DLOG ("MATCH, ");
        break;
      }
      case RE_OP_CHAR:
      {
        JERRY_DLOG ("CHAR ");
        JERRY_DLOG ("%c, ", (char) get_value (&bytecode_p));
        break;
      }
      case RE_OP_SAVE_AT_START:
      {
        JERRY_DLOG ("RE_START ");
        JERRY_DLOG ("%d, ", get_value (&bytecode_p));
        break;
      }
      case RE_OP_SAVE_AND_MATCH:
      {
        JERRY_DLOG ("RE_END, ");
        break;
      }
      case RE_OP_GREEDY_ITERATOR:
      {
        JERRY_DLOG ("RE_OP_GREEDY_ITERATOR ");
        JERRY_DLOG ("%d ", get_value (&bytecode_p));
        JERRY_DLOG ("%d ", get_value (&bytecode_p));
        JERRY_DLOG ("%d, ", get_value (&bytecode_p));
        break;
      }
      case RE_OP_NON_GREEDY_ITERATOR:
      {
        JERRY_DLOG ("RE_OP_NON_GREEDY_ITERATOR ");
        JERRY_DLOG ("%d, ", get_value (&bytecode_p));
        JERRY_DLOG ("%d, ", get_value (&bytecode_p));
        JERRY_DLOG ("%d, ", get_value (&bytecode_p));
        break;
      }
      case RE_OP_PERIOD:
      {
        JERRY_DLOG ("RE_OP_PERIOD ");
        break;
      }
      case RE_OP_ALTERNATIVE:
      {
        JERRY_DLOG ("RE_OP_ALTERNATIVE ");
        JERRY_DLOG ("%d, ", get_value (&bytecode_p));
        break;
      }
      default:
      {
        JERRY_DLOG ("UNKNOWN(%d), ", (uint32_t) op);
        break;
      }
    }
  }
  JERRY_DLOG ("EOF\n");
} /* regexp_dump_bytecode */
#endif