// Copyright (c) 2009-2024 LG Electronics, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// SPDX-License-Identifier: Apache-2.0

#include "gen_stream.h"
#include "jobject.h"
#include "jcallbacks.h"
#include "jschema_types_internal.h"
#include "jparse_stream_internal.h"

#include <yajl/yajl_gen.h>
#include "yajl_compat.h"

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <inttypes.h>

#include <compiler/malloc_attribute.h>
#include <compiler/unused_attribute.h>

#include "liblog.h"

typedef struct PJSON_LOCAL {
	struct __JStream stream;
	TopLevelType opened;
	yajl_gen handle;
	StreamStatus error;
} ActualStream;

#define JGEN_DEFAULT_INDENT "  "

#define CHECK_HANDLE(stream) 							\
	do {									\
		if (stream->error != GEN_OK || stream->handle == NULL) {	\
			if (stream->error == GEN_OK) {				\
				stream->error = GEN_GENERIC_ERROR;		\
			}							\
			return stream;						\
		}								\
	} while(0)

static ActualStream* begin_object(ActualStream* stream)
{
	SANITY_CHECK_POINTER(stream);
	CHECK_HANDLE(stream);
	yajl_gen_map_open(stream->handle);
	return stream;
}

static ActualStream* key_object(ActualStream* stream, raw_buffer buf)
{
	SANITY_CHECK_POINTER(stream);
	CHECK_HANDLE(stream);
	SANITY_CHECK_POINTER(buf.m_str);
	yajl_gen_string(stream->handle, (const unsigned char *)buf.m_str, buf.m_len);
	return stream;
}

static ActualStream* end_object(ActualStream* stream)
{
	SANITY_CHECK_POINTER(stream);
	CHECK_HANDLE(stream);
	yajl_gen_map_close(stream->handle);
	return stream;
}

static ActualStream* begin_array(ActualStream* stream)
{
	SANITY_CHECK_POINTER(stream);
	CHECK_HANDLE(stream);
	yajl_gen_array_open(stream->handle);
	return stream;
}

static ActualStream* end_array(ActualStream* stream)
{
	SANITY_CHECK_POINTER(stream);
	CHECK_HANDLE(stream);
	yajl_gen_array_close(stream->handle);
	return stream;
}

static ActualStream* val_num(ActualStream* stream, raw_buffer numstr)
{
	SANITY_CHECK_POINTER(stream);
	SANITY_CHECK_POINTER(numstr.m_str);
	assert (numstr.m_str != NULL);
	CHECK_HANDLE(stream);
	yajl_gen_number(stream->handle, numstr.m_str, numstr.m_len);
	return stream;
}

static ActualStream* val_int(ActualStream* stream, int64_t number)
{
	SANITY_CHECK_POINTER(stream);
	CHECK_HANDLE(stream);
	char buf[24];
	int printed = snprintf(buf, sizeof(buf), "%" PRId64, number);
	yajl_gen_number(stream->handle, buf, printed);
	return stream;
}

static ActualStream* val_dbl(ActualStream* stream, double number)
{
	SANITY_CHECK_POINTER(stream);
	CHECK_HANDLE(stream);
	// yajl_gen_double doesn't print properly (%g doesn't seem to do what it claims to
	// do or something - fails for 42323.0234234)
	// let's work around it with the raw interface by
	char buf[24];
	int len = snprintf(buf, sizeof(buf), "%.14lg", number);
	yajl_gen_number(stream->handle, buf, len);
	return stream;
}

static ActualStream* val_str(ActualStream* stream, raw_buffer str)
{
	SANITY_CHECK_POINTER(stream);
	SANITY_CHECK_POINTER(str.m_str);
	assert(str.m_str != NULL);
	CHECK_HANDLE(stream);
	yajl_gen_string(stream->handle, (const unsigned char *)str.m_str, str.m_len);
	return stream;
}

static ActualStream* val_bool(ActualStream* stream, bool boolean)
{
	SANITY_CHECK_POINTER(stream);
	CHECK_HANDLE(stream);
	yajl_gen_bool(stream->handle, boolean);
	return stream;
}

static ActualStream* val_null(ActualStream* stream)
{
	SANITY_CHECK_POINTER(stream);
	CHECK_HANDLE(stream);
	yajl_gen_null(stream->handle);
	return stream;
}

static StreamStatus convert_error_code(yajl_gen_status raw_code)
{
	switch (raw_code) {
		case yajl_gen_generation_complete:
		case yajl_gen_status_ok:
			return GEN_OK;
		case yajl_gen_keys_must_be_strings:
			return GEN_KEYS_MUST_BE_STRINGS;
		case yajl_max_depth_exceeded:
		case yajl_gen_in_error_state:
		default:
			return GEN_GENERIC_ERROR;
	}
}

static void destroy_stream(ActualStream* stream)
{
	if(stream->handle)
		yajl_gen_free(stream->handle);
	SANITY_KILL_POINTER(stream->handle);

	if(stream)
		free(stream);
	SANITY_KILL_POINTER(stream);
}

static char* finish_stream(ActualStream* stream, StreamStatus *error_code)
{
	char *buf = NULL;
	yajl_size_t len;
	yajl_gen_status result;

	SANITY_CHECK_POINTER(stream);
	SANITY_CHECK_POINTER(error_code);

	switch (stream->opened) {
		case TOP_None:
			break;
		case TOP_Object:
			end_object(stream);
			break;
		case TOP_Array:
			end_array(stream);
			break;
		default:
			if (error_code) *error_code = GEN_GENERIC_ERROR;
			goto stream_error;
	}

	if (!stream->handle) {
		if (error_code) *error_code = GEN_GENERIC_ERROR;
		goto stream_error;
	}

	if (stream->error == GEN_OK) {
		const unsigned char *yajlBuf;
		result = yajl_gen_get_buf(stream->handle, &yajlBuf, &len);
		if (error_code) {
			*error_code = convert_error_code(result);
		}
		if (result != yajl_gen_status_ok && result != yajl_gen_generation_complete) {
			buf = NULL;
		} else {
			buf = calloc(len + 1, sizeof(char));
			if (LIKELY(buf != NULL)) {
				memcpy(buf, yajlBuf, len);
			}
		}
	} else if (error_code) {
		*error_code = stream->error;
	}

	destroy_stream(stream);

	return buf;

stream_error:
	destroy_stream(stream);

	return NULL;
}

static struct __JStream yajl_stream_generator =
{
	(jObjectBegin)begin_object,
	(jObjectKey)key_object,
	(jObjectEnd)end_object,
	(jArrayBegin)begin_array,
	(jArrayEnd)end_array,
	(jNumber)val_num,
	(jNumberI)val_int,
	(jNumberF)val_dbl,
	(jString)val_str,
	(jBoolean)val_bool,
	(jNull)val_null,
	(jFinish)finish_stream
};

JStreamRef jstreamInternal(TopLevelType type, const char *indent)
{
	ActualStream* stream = (ActualStream*)calloc(1, sizeof(ActualStream));
	if (UNLIKELY(stream == NULL)) {
		return NULL;
	}

	memcpy(&stream->stream, &yajl_stream_generator, sizeof(struct __JStream));

#if YAJL_VERSION < 20000
	yajl_gen_config conf = {1, indent};
	stream->handle = yajl_gen_alloc(indent ? &conf : NULL, NULL);
#else
	stream->handle = yajl_gen_alloc(NULL);
	if (indent) {
		yajl_gen_config(stream->handle, yajl_gen_beautify, 1);
		if (!yajl_gen_config(stream->handle, yajl_gen_indent_string, indent)) {
			yajl_gen_config(stream->handle, yajl_gen_indent_string, JGEN_DEFAULT_INDENT);
		}
	}
#endif

	stream->opened = type;
	stream->error = GEN_OK;

	return (JStreamRef)stream;
}

