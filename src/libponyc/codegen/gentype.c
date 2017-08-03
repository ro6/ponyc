#include "gentype.h"
#include "genname.h"
#include "gendesc.h"
#include "genprim.h"
#include "gentrace.h"
#include "genfun.h"
#include "genopt.h"
#include "genserialise.h"
#include "genident.h"
#include "genreference.h"
#include "../ast/id.h"
#include "../pkg/package.h"
#include "../type/cap.h"
#include "../type/reify.h"
#include "../type/subtype.h"
#include "../../libponyrt/mem/pool.h"
#include "ponyassert.h"
#include <stdlib.h>
#include <string.h>

static void compile_type_free(void* p)
{
  POOL_FREE(compile_type_t, p);
}

static size_t tbaa_metadata_hash(tbaa_metadata_t* a)
{
  return ponyint_hash_ptr(a->name);
}

static bool tbaa_metadata_cmp(tbaa_metadata_t* a, tbaa_metadata_t* b)
{
  return a->name == b->name;
}

DEFINE_HASHMAP(tbaa_metadatas, tbaa_metadatas_t, tbaa_metadata_t,
  tbaa_metadata_hash, tbaa_metadata_cmp, ponyint_pool_alloc_size,
  ponyint_pool_free_size, NULL);

tbaa_metadatas_t* tbaa_metadatas_new()
{
  tbaa_metadatas_t* tbaa_mds = POOL_ALLOC(tbaa_metadatas_t);
  tbaa_metadatas_init(tbaa_mds, 64);
  return tbaa_mds;
}

void tbaa_metadatas_free(tbaa_metadatas_t* tbaa_mds)
{
  tbaa_metadatas_destroy(tbaa_mds);
  POOL_FREE(tbaa_metadatas_t, tbaa_mds);
}

LLVMValueRef tbaa_metadata_for_type(compile_t* c, ast_t* type)
{
  pony_assert(ast_id(type) == TK_NOMINAL);

  const char* name = genname_type_and_cap(type);
  tbaa_metadata_t k;
  k.name = name;
  size_t index = HASHMAP_UNKNOWN;
  tbaa_metadata_t* md = tbaa_metadatas_get(c->tbaa_mds, &k, &index);
  if(md != NULL)
    return md->metadata;

  md = POOL_ALLOC(tbaa_metadata_t);
  md->name = name;
  // didn't find it in the map but index is where we can put the
  // new one without another search
  tbaa_metadatas_putindex(c->tbaa_mds, md, index);

  LLVMValueRef params[3];
  params[0] = LLVMMDStringInContext(c->context, name, (unsigned)strlen(name));

  token_id cap = cap_single(type);
  switch(cap)
  {
    case TK_TRN:
    case TK_REF:
    {
      ast_t* tcap = ast_childidx(type, 3);
      ast_setid(tcap, TK_BOX);
      params[1] = tbaa_metadata_for_type(c, type);
      ast_setid(tcap, cap);
      break;
    }
    default:
      params[1] = c->tbaa_root;
      break;
  }

  md->metadata = LLVMMDNodeInContext(c->context, params, 2);
  return md->metadata;
}

LLVMValueRef tbaa_metadata_for_box_type(compile_t* c, const char* box_name)
{
  tbaa_metadata_t k;
  k.name = box_name;
  size_t index = HASHMAP_UNKNOWN;
  tbaa_metadata_t* md = tbaa_metadatas_get(c->tbaa_mds, &k, &index);
  if(md != NULL)
    return md->metadata;

  md = POOL_ALLOC(tbaa_metadata_t);
  md->name = box_name;
  // didn't find it in the map but index is where we can put the
  // new one without another search
  tbaa_metadatas_putindex(c->tbaa_mds, md, index);

  LLVMValueRef params[2];
  params[0] = LLVMMDStringInContext(c->context, box_name,
    (unsigned)strlen(box_name));
  params[1] = c->tbaa_root;

  md->metadata = LLVMMDNodeInContext(c->context, params, 2);
  return md->metadata;
}

static LLVMValueRef make_tbaa_root(LLVMContextRef ctx)
{
  const char str[] = "Pony TBAA";
  LLVMValueRef mdstr = LLVMMDStringInContext(ctx, str, sizeof(str) - 1);
  return LLVMMDNodeInContext(ctx, &mdstr, 1);
}

static LLVMValueRef make_tbaa_descriptor(LLVMContextRef ctx, LLVMValueRef root)
{
  const char str[] = "Type descriptor";
  LLVMValueRef params[3];
  params[0] = LLVMMDStringInContext(ctx, str, sizeof(str) - 1);
  params[1] = root;
  params[2] = LLVMConstInt(LLVMInt64TypeInContext(ctx), 1, false);
  return LLVMMDNodeInContext(ctx, params, 3);
}

static LLVMValueRef make_tbaa_descptr(LLVMContextRef ctx, LLVMValueRef root)
{
  const char str[] = "Descriptor pointer";
  LLVMValueRef params[2];
  params[0] = LLVMMDStringInContext(ctx, str, sizeof(str) - 1);
  params[1] = root;
  return LLVMMDNodeInContext(ctx, params, 2);
}

static void allocate_compile_types(compile_t* c)
{
  size_t i = HASHMAP_BEGIN;
  reach_type_t* t;

  while((t = reach_types_next(&c->reach->types, &i)) != NULL)
  {
    compile_type_t* c_t = POOL_ALLOC(compile_type_t);
    memset(c_t, 0, sizeof(compile_type_t));
    c_t->free_fn = compile_type_free;
    t->c_type = (compile_opaque_t*)c_t;

    genfun_allocate_compile_methods(c, t);
  }
}

static bool make_opaque_struct(compile_t* c, reach_type_t* t)
{
  compile_type_t* c_t = (compile_type_t*)t->c_type;

  switch(ast_id(t->ast))
  {
    case TK_NOMINAL:
    {
      switch(t->underlying)
      {
        case TK_INTERFACE:
        case TK_TRAIT:
          c_t->use_type = c->object_ptr;
          return true;

        default: {}
      }

      // Find the primitive type, if there is one.
      AST_GET_CHILDREN(t->ast, pkg, id);
      const char* package = ast_name(pkg);
      const char* name = ast_name(id);

      bool ilp32 = target_is_ilp32(c->opt->triple);
      bool llp64 = target_is_llp64(c->opt->triple);
      bool lp64 = target_is_lp64(c->opt->triple);

      if(package == c->str_builtin)
      {
        if(name == c->str_Bool)
          c_t->primitive = c->ibool;
        else if(name == c->str_I8)
          c_t->primitive = c->i8;
        else if(name == c->str_U8)
          c_t->primitive = c->i8;
        else if(name == c->str_I16)
          c_t->primitive = c->i16;
        else if(name == c->str_U16)
          c_t->primitive = c->i16;
        else if(name == c->str_I32)
          c_t->primitive = c->i32;
        else if(name == c->str_U32)
          c_t->primitive = c->i32;
        else if(name == c->str_I64)
          c_t->primitive = c->i64;
        else if(name == c->str_U64)
          c_t->primitive = c->i64;
        else if(name == c->str_I128)
          c_t->primitive = c->i128;
        else if(name == c->str_U128)
          c_t->primitive = c->i128;
        else if(ilp32 && name == c->str_ILong)
          c_t->primitive = c->i32;
        else if(ilp32 && name == c->str_ULong)
          c_t->primitive = c->i32;
        else if(ilp32 && name == c->str_ISize)
          c_t->primitive = c->i32;
        else if(ilp32 && name == c->str_USize)
          c_t->primitive = c->i32;
        else if(lp64 && name == c->str_ILong)
          c_t->primitive = c->i64;
        else if(lp64 && name == c->str_ULong)
          c_t->primitive = c->i64;
        else if(lp64 && name == c->str_ISize)
          c_t->primitive = c->i64;
        else if(lp64 && name == c->str_USize)
          c_t->primitive = c->i64;
        else if(llp64 && name == c->str_ILong)
          c_t->primitive = c->i32;
        else if(llp64 && name == c->str_ULong)
          c_t->primitive = c->i32;
        else if(llp64 && name == c->str_ISize)
          c_t->primitive = c->i64;
        else if(llp64 && name == c->str_USize)
          c_t->primitive = c->i64;
        else if(name == c->str_F32)
          c_t->primitive = c->f32;
        else if(name == c->str_F64)
          c_t->primitive = c->f64;
        else if(name == c->str_Pointer)
        {
          c_t->use_type = c->void_ptr;
          return true;
        }
        else if(name == c->str_Maybe)
        {
          c_t->use_type = c->void_ptr;
          return true;
        }
      }

      if(t->bare_method == NULL)
      {
        c_t->structure = LLVMStructCreateNamed(c->context, t->name);
        c_t->structure_ptr = LLVMPointerType(c_t->structure, 0);

        if(c_t->primitive != NULL)
          c_t->use_type = c_t->primitive;
        else
          c_t->use_type = c_t->structure_ptr;
      } else {
        c_t->structure = c->void_ptr;
        c_t->structure_ptr = c->void_ptr;
        c_t->use_type = c->void_ptr;
      }

      return true;
    }

    case TK_TUPLETYPE:
      c_t->primitive = LLVMStructCreateNamed(c->context, t->name);
      c_t->use_type = c_t->primitive;
      return true;

    case TK_UNIONTYPE:
    case TK_ISECTTYPE:
      // Just a raw object pointer.
      c_t->use_type = c->object_ptr;
      return true;

    default: {}
  }

  pony_assert(0);
  return false;
}

static void make_debug_basic(compile_t* c, reach_type_t* t)
{
  compile_type_t* c_t = (compile_type_t*)t->c_type;
  uint64_t size = LLVMABISizeOfType(c->target_data, c_t->primitive);
  uint64_t align = LLVMABIAlignmentOfType(c->target_data, c_t->primitive);
  unsigned encoding;

  if(is_bool(t->ast))
  {
    encoding = DW_ATE_boolean;
  } else if(is_float(t->ast)) {
    encoding = DW_ATE_float;
  } else if(is_signed(t->ast)) {
    encoding = DW_ATE_signed;
  } else {
    encoding = DW_ATE_unsigned;
  }

  c_t->di_type = LLVMDIBuilderCreateBasicType(c->di, t->name,
    8 * size, 8 * align, encoding);
}

static void make_debug_prototype(compile_t* c, reach_type_t* t)
{
  compile_type_t* c_t = (compile_type_t*)t->c_type;
  c_t->di_type = LLVMDIBuilderCreateReplaceableStruct(c->di,
    t->name, c->di_unit, c_t->di_file, (unsigned)ast_line(t->ast));

  if(t->underlying != TK_TUPLETYPE)
  {
    c_t->di_type_embed = c_t->di_type;
    c_t->di_type = LLVMDIBuilderCreatePointerType(c->di, c_t->di_type_embed, 0,
      0);
  }
}

static void make_debug_info(compile_t* c, reach_type_t* t)
{
  ast_t* def = (ast_t*)ast_data(t->ast);
  source_t* source;

  if(def != NULL)
    source = ast_source(def);
  else
    source = ast_source(t->ast);

  const char* file = source->file;
  if(file == NULL)
    file = "";

  compile_type_t* c_t = (compile_type_t*)t->c_type;
  c_t->di_file = LLVMDIBuilderCreateFile(c->di, file);

  switch(t->underlying)
  {
    case TK_TUPLETYPE:
    case TK_STRUCT:
      make_debug_prototype(c, t);
      return;

    case TK_PRIMITIVE:
    {
      if(c_t->primitive != NULL)
        make_debug_basic(c, t);
      else
        make_debug_prototype(c, t);
      return;
    }

    case TK_UNIONTYPE:
    case TK_ISECTTYPE:
    case TK_INTERFACE:
    case TK_TRAIT:
    case TK_CLASS:
    case TK_ACTOR:
      make_debug_prototype(c, t);
      return;

    default: {}
  }

  pony_assert(0);
}

static void make_box_type(compile_t* c, reach_type_t* t)
{
  compile_type_t* c_t = (compile_type_t*)t->c_type;

  if(c_t->primitive == NULL)
    return;

  const char* box_name = genname_box(t->name);
  c_t->structure = LLVMStructCreateNamed(c->context, box_name);

  LLVMTypeRef elements[2];
  elements[0] = LLVMPointerType(c_t->desc_type, 0);
  elements[1] = c_t->primitive;
  LLVMStructSetBody(c_t->structure, elements, 2, false);

  c_t->structure_ptr = LLVMPointerType(c_t->structure, 0);
}

static void make_global_instance(compile_t* c, reach_type_t* t)
{
  // Not a primitive type.
  if(t->underlying != TK_PRIMITIVE)
    return;

  compile_type_t* c_t = (compile_type_t*)t->c_type;

  // No instance for machine word types.
  if(c_t->primitive != NULL)
    return;

  if(t->bare_method != NULL)
    return;

  // Create a unique global instance.
  const char* inst_name = genname_instance(t->name);
  LLVMValueRef value = LLVMConstNamedStruct(c_t->structure, &c_t->desc, 1);
  c_t->instance = LLVMAddGlobal(c->module, c_t->structure, inst_name);
  LLVMSetInitializer(c_t->instance, value);
  LLVMSetGlobalConstant(c_t->instance, true);
  LLVMSetLinkage(c_t->instance, LLVMPrivateLinkage);
}

static void make_dispatch(compile_t* c, reach_type_t* t)
{
  // Do nothing if we're not an actor.
  if(t->underlying != TK_ACTOR)
    return;

  // Create a dispatch function.
  compile_type_t* c_t = (compile_type_t*)t->c_type;
  const char* dispatch_name = genname_dispatch(t->name);
  c_t->dispatch_fn = codegen_addfun(c, dispatch_name, c->dispatch_type);
  LLVMSetFunctionCallConv(c_t->dispatch_fn, LLVMCCallConv);
  LLVMSetLinkage(c_t->dispatch_fn, LLVMExternalLinkage);
  codegen_startfun(c, c_t->dispatch_fn, NULL, NULL, false);

  LLVMBasicBlockRef unreachable = codegen_block(c, "unreachable");

  // Read the message ID.
  LLVMValueRef msg = LLVMGetParam(c_t->dispatch_fn, 2);
  LLVMValueRef id_ptr = LLVMBuildStructGEP(c->builder, msg, 1, "");
  LLVMValueRef id = LLVMBuildLoad(c->builder, id_ptr, "id");

  // Store a reference to the dispatch switch. When we build behaviours, we
  // will add cases to this switch statement based on message ID.
  c_t->dispatch_switch = LLVMBuildSwitch(c->builder, id, unreachable, 0);

  // Mark the default case as unreachable.
  LLVMPositionBuilderAtEnd(c->builder, unreachable);
  LLVMBuildUnreachable(c->builder);
  codegen_finishfun(c);
}

static bool make_struct(compile_t* c, reach_type_t* t)
{
  compile_type_t* c_t = (compile_type_t*)t->c_type;
  LLVMTypeRef type;
  int extra = 0;
  bool packed = false;

  if(t->bare_method != NULL)
    return true;

  switch(t->underlying)
  {
    case TK_UNIONTYPE:
    case TK_ISECTTYPE:
    case TK_INTERFACE:
    case TK_TRAIT:
      return true;

    case TK_TUPLETYPE:
      type = c_t->primitive;
      break;

    case TK_STRUCT:
    {
      // Pointer and Maybe will have no structure.
      if(c_t->structure == NULL)
        return true;

      type = c_t->structure;
      ast_t* def = (ast_t*)ast_data(t->ast);
      if(ast_has_annotation(def, "packed"))
        packed = true;

      break;
    }

    case TK_PRIMITIVE:
      // Machine words will have a primitive.
      if(c_t->primitive != NULL)
      {
        // The ABI size for machine words and tuples is the boxed size.
        c_t->abi_size = (size_t)LLVMABISizeOfType(c->target_data,
          c_t->structure);
        return true;
      }

      extra = 1;
      type = c_t->structure;
      break;

    case TK_CLASS:
      extra = 1;
      type = c_t->structure;
      break;

    case TK_ACTOR:
      extra = 2;
      type = c_t->structure;
      break;

    default:
      pony_assert(0);
      return false;
  }

  size_t buf_size = (t->field_count + extra) * sizeof(LLVMTypeRef);
  LLVMTypeRef* elements = (LLVMTypeRef*)ponyint_pool_alloc_size(buf_size);

  // Create the type descriptor as element 0.
  if(extra > 0)
    elements[0] = LLVMPointerType(c_t->desc_type, 0);

  // Create the actor pad as element 1.
  if(extra > 1)
    elements[1] = c->actor_pad;

  for(uint32_t i = 0; i < t->field_count; i++)
  {
    compile_type_t* f_c_t = (compile_type_t*)t->fields[i].type->c_type;

    if(t->fields[i].embed)
      elements[i + extra] = f_c_t->structure;
    else
      elements[i + extra] = f_c_t->use_type;

    if(elements[i + extra] == NULL)
    {
      pony_assert(0);
      return false;
    }
  }

  LLVMStructSetBody(type, elements, t->field_count + extra, packed);
  ponyint_pool_free_size(buf_size, elements);
  return true;
}

static LLVMMetadataRef make_debug_field(compile_t* c, reach_type_t* t,
  uint32_t i)
{
  const char* name;
  char buf[32];
  unsigned flags = 0;
  uint64_t offset = 0;
  ast_t* ast;
  compile_type_t* c_t = (compile_type_t*)t->c_type;

  if(t->underlying != TK_TUPLETYPE)
  {
    ast_t* def = (ast_t*)ast_data(t->ast);
    ast_t* members = ast_childidx(def, 4);
    ast = ast_childidx(members, i);
    name = ast_name(ast_child(ast));

    if(is_name_private(name))
      flags |= DW_FLAG_Private;

    uint32_t extra = 0;

    if(t->underlying != TK_STRUCT)
      extra++;

    if(t->underlying == TK_ACTOR)
      extra++;

    offset = LLVMOffsetOfElement(c->target_data, c_t->structure, i + extra);
  } else {
    snprintf(buf, 32, "_%d", i + 1);
    name = buf;
    ast = t->ast;
    offset = LLVMOffsetOfElement(c->target_data, c_t->primitive, i);
  }

  LLVMTypeRef type;
  LLVMMetadataRef di_type;
  compile_type_t* f_c_t = (compile_type_t*)t->fields[i].type->c_type;

  if(t->fields[i].embed)
  {
    type = f_c_t->structure;
    di_type = f_c_t->di_type_embed;
  } else {
    type = f_c_t->use_type;
    di_type = f_c_t->di_type;
  }

  uint64_t size = LLVMABISizeOfType(c->target_data, type);
  uint64_t align = LLVMABIAlignmentOfType(c->target_data, type);

  return LLVMDIBuilderCreateMemberType(c->di, c->di_unit, name, c_t->di_file,
    (unsigned)ast_line(ast), 8 * size, 8 * align, 8 * offset, flags, di_type);
}

static void make_debug_fields(compile_t* c, reach_type_t* t)
{
  LLVMMetadataRef fields = NULL;

  if(t->field_count > 0)
  {
    size_t buf_size = t->field_count * sizeof(LLVMMetadataRef);
    LLVMMetadataRef* data = (LLVMMetadataRef*)ponyint_pool_alloc_size(
      buf_size);

    for(uint32_t i = 0; i < t->field_count; i++)
      data[i] = make_debug_field(c, t, i);

    fields = LLVMDIBuilderGetOrCreateArray(c->di, data, t->field_count);
    ponyint_pool_free_size(buf_size, data);
  }

  LLVMTypeRef type;
  compile_type_t* c_t = (compile_type_t*)t->c_type;

  if(t->underlying != TK_TUPLETYPE)
    type = c_t->structure;
  else
    type = c_t->primitive;

  uint64_t size = 0;
  uint64_t align = 0;

  if(type != NULL)
  {
    size = LLVMABISizeOfType(c->target_data, type);
    align = LLVMABIAlignmentOfType(c->target_data, type);
  }

  LLVMMetadataRef di_type = LLVMDIBuilderCreateStructType(c->di, c->di_unit,
    t->name, c_t->di_file, (unsigned) ast_line(t->ast), 8 * size, 8 * align,
    fields);

  if(t->underlying != TK_TUPLETYPE)
  {
    LLVMMetadataReplaceAllUsesWith(c_t->di_type_embed, di_type);
    c_t->di_type_embed = di_type;
  } else {
    LLVMMetadataReplaceAllUsesWith(c_t->di_type, di_type);
    c_t->di_type = di_type;
  }
}

static void make_debug_final(compile_t* c, reach_type_t* t)
{
  switch(t->underlying)
  {
    case TK_UNIONTYPE:
    case TK_ISECTTYPE:
    case TK_TUPLETYPE:
    case TK_INTERFACE:
    case TK_TRAIT:
    case TK_STRUCT:
    case TK_CLASS:
    case TK_ACTOR:
      make_debug_fields(c, t);
      return;

    case TK_PRIMITIVE:
    {
      compile_type_t* c_t = (compile_type_t*)t->c_type;
      if(c_t->primitive == NULL)
        make_debug_fields(c, t);
      return;
    }

    default: {}
  }

  pony_assert(0);
}

static void make_intrinsic_methods(compile_t* c, reach_type_t* t)
{
  if(t->can_be_boxed)
  {
    gen_digestof_fun(c, t);
    if(ast_id(t->ast) == TK_TUPLETYPE)
      gen_is_tuple_fun(c, t);
  }

  if(ast_id(t->ast) != TK_NOMINAL)
    return;

  // Find the primitive type, if there is one.
  AST_GET_CHILDREN(t->ast, pkg, id);
  const char* package = ast_name(pkg);
  const char* name = ast_name(id);

  if(package == c->str_builtin)
  {
    if(name == c->str_Pointer)
      genprim_pointer_methods(c, t);
    else if(name == c->str_Maybe)
      genprim_maybe_methods(c, t);
    else if(name == c->str_DoNotOptimise)
      genprim_donotoptimise_methods(c, t);
    else if(name == c->str_Platform)
      genprim_platform_methods(c, t);
  }
}

static bool make_trace(compile_t* c, reach_type_t* t)
{
  compile_type_t* c_t = (compile_type_t*)t->c_type;

  if(c_t->trace_fn == NULL)
    return true;

  if(t->underlying == TK_CLASS)
  {
    // Special case the array trace function.
    AST_GET_CHILDREN(t->ast, pkg, id);
    const char* package = ast_name(pkg);
    const char* name = ast_name(id);

    if((package == c->str_builtin) && (name == c->str_Array))
    {
      genprim_array_trace(c, t);
      return true;
    }
  }

  // Generate the trace function.
  codegen_startfun(c, c_t->trace_fn, NULL, NULL, false);
  LLVMSetFunctionCallConv(c_t->trace_fn, LLVMCCallConv);
  LLVMSetLinkage(c_t->trace_fn, LLVMExternalLinkage);

  LLVMValueRef ctx = LLVMGetParam(c_t->trace_fn, 0);
  LLVMValueRef arg = LLVMGetParam(c_t->trace_fn, 1);
  LLVMValueRef object = LLVMBuildBitCast(c->builder, arg, c_t->structure_ptr,
    "object");

  int extra = 0;

  // Non-structs have a type descriptor.
  if(t->underlying != TK_STRUCT)
    extra++;

  // Actors have a pad.
  if(t->underlying == TK_ACTOR)
    extra++;

  for(uint32_t i = 0; i < t->field_count; i++)
  {
    LLVMValueRef field = LLVMBuildStructGEP(c->builder, object, i + extra, "");

    if(!t->fields[i].embed)
    {
      // Call the trace function indirectly depending on rcaps.
      LLVMValueRef value = LLVMBuildLoad(c->builder, field, "");
      ast_t* field_type = t->fields[i].ast;
      gentrace(c, ctx, value, value, field_type, field_type);
    } else {
      // Call the trace function directly without marking the field.
      compile_type_t* f_c_t = (compile_type_t*)t->fields[i].type->c_type;
      LLVMValueRef trace_fn = f_c_t->trace_fn;

      if(trace_fn != NULL)
      {
        LLVMValueRef args[2];
        args[0] = ctx;
        args[1] = LLVMBuildBitCast(c->builder, field, c->object_ptr, "");

        LLVMBuildCall(c->builder, trace_fn, args, 2, "");
      }
    }
  }

  LLVMBuildRetVoid(c->builder);
  codegen_finishfun(c);
  return true;
}

bool gentypes(compile_t* c)
{
  reach_type_t* t;
  size_t i;

  if(target_is_ilp32(c->opt->triple))
    c->trait_bitmap_size = ((c->reach->trait_type_count + 31) & ~31) >> 5;
  else
    c->trait_bitmap_size = ((c->reach->trait_type_count + 63) & ~63) >> 6;

  c->tbaa_root = make_tbaa_root(c->context);
  c->tbaa_descriptor = make_tbaa_descriptor(c->context, c->tbaa_root);
  c->tbaa_descptr = make_tbaa_descptr(c->context, c->tbaa_root);

  allocate_compile_types(c);
  genprim_builtins(c);

  if(c->opt->verbosity >= VERBOSITY_INFO)
    fprintf(stderr, " Data prototypes\n");

  i = HASHMAP_BEGIN;

  while((t = reach_types_next(&c->reach->types, &i)) != NULL)
  {
    if(!make_opaque_struct(c, t))
      return false;

    gendesc_type(c, t);
    make_debug_info(c, t);
    make_box_type(c, t);
    make_dispatch(c, t);
    gentrace_prototype(c, t);
  }

  gendesc_table(c);

  c->numeric_sizes = gen_numeric_size_table(c);

  if(c->opt->verbosity >= VERBOSITY_INFO)
    fprintf(stderr, " Data types\n");

  i = HASHMAP_BEGIN;

  while((t = reach_types_next(&c->reach->types, &i)) != NULL)
  {
    if(!make_struct(c, t))
      return false;

    make_global_instance(c, t);
  }

  // Cache the instance of None, which is used as the return value for
  // behaviour calls.
  t = reach_type_name(c->reach, "None");
  pony_assert(t != NULL);
  compile_type_t* c_t = (compile_type_t*)t->c_type;
  c->none_instance = c_t->instance;

  if(c->opt->verbosity >= VERBOSITY_INFO)
    fprintf(stderr, " Function prototypes\n");

  i = HASHMAP_BEGIN;

  while((t = reach_types_next(&c->reach->types, &i)) != NULL)
  {
    compile_type_t* c_t = (compile_type_t*)t->c_type;

    // The ABI size for machine words and tuples is the boxed size.
    if(c_t->structure != NULL)
      c_t->abi_size = (size_t)LLVMABISizeOfType(c->target_data, c_t->structure);

    make_debug_final(c, t);
    make_intrinsic_methods(c, t);

    if(!genfun_method_sigs(c, t))
      return false;
  }

  if(c->opt->verbosity >= VERBOSITY_INFO)
    fprintf(stderr, " Functions\n");

  i = HASHMAP_BEGIN;

  while((t = reach_types_next(&c->reach->types, &i)) != NULL)
  {
    if(!genfun_method_bodies(c, t))
      return false;
  }

  genfun_primitive_calls(c);

  if(c->opt->verbosity >= VERBOSITY_INFO)
    fprintf(stderr, " Descriptors\n");

  i = HASHMAP_BEGIN;

  while((t = reach_types_next(&c->reach->types, &i)) != NULL)
  {
    if(!make_trace(c, t))
      return false;

    if(!genserialise(c, t))
      return false;

    gendesc_init(c, t);
  }

  return true;
}
