#ifndef CODEGEN_GENTYPE_H
#define CODEGEN_GENTYPE_H

#include <platform.h>
#include "codegen.h"

PONY_EXTERN_C_BEGIN

typedef struct compile_type_t
{
  compile_opaque_free_fn free_fn;

  size_t abi_size;

  LLVMTypeRef structure;
  LLVMTypeRef structure_ptr;
  LLVMTypeRef primitive;
  LLVMTypeRef use_type;

  LLVMTypeRef desc_type;
  LLVMValueRef desc;
  LLVMValueRef instance;
  LLVMValueRef trace_fn;
  LLVMValueRef serialise_trace_fn;
  LLVMValueRef serialise_fn;
  LLVMValueRef deserialise_fn;
  LLVMValueRef custom_serialise_space_fn;
  LLVMValueRef custom_serialise_fn;
  LLVMValueRef custom_deserialise_fn;
  LLVMValueRef final_fn;
  LLVMValueRef dispatch_fn;
  LLVMValueRef dispatch_switch;

  LLVMMetadataRef di_file;
  LLVMMetadataRef di_type;
  LLVMMetadataRef di_type_embed;
} compile_type_t;

typedef struct tbaa_metadata_t
{
  const char* name;
  LLVMValueRef metadata;
} tbaa_metadata_t;

DECLARE_HASHMAP(tbaa_metadatas, tbaa_metadatas_t, tbaa_metadata_t);

tbaa_metadatas_t* tbaa_metadatas_new();

void tbaa_metadatas_free(tbaa_metadatas_t* tbaa_metadatas);

LLVMValueRef tbaa_metadata_for_type(compile_t* c, ast_t* type);

LLVMValueRef tbaa_metadata_for_box_type(compile_t* c, const char* box_name);

bool gentypes(compile_t* c);

PONY_EXTERN_C_END

#endif
