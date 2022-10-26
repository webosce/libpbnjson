// Copyright (c) 2009-2022 LG Electronics, Inc.
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

#include <stddef.h>
#include <unistd.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <stdarg.h>
#include <compiler/inline_attribute.h>
#include <compiler/nonnull_attribute.h>
#include <compiler/builtins.h>
#include <math.h>
#include <inttypes.h>

#include <jobject.h>

#include <sys_malloc.h>
#include <sys/mman.h>
#include "jobject_internal.h"
#include "jerror_internal.h"
#include "jvalue/num_conversion.h"
#include "liblog.h"
#include "key_dictionary.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <unistd.h>

#include "dom_string_memory_pool.h"

#ifdef DBG_C_MEM
#define PJ_LOG_MEM(...) PJ_LOG_INFO(__VA_ARGS__)
#else
#define PJ_LOG_MEM(...) do { } while (0)
#endif

#ifndef PJSON_EXPORT
#error "Compiling with the wrong options"
#endif

#include "gen_stream.h"

#include "liblog.h"

#define CONST_C_STRING(string) (string), sizeof(string)
#define VAR_C_STRING(string) (string), strlen(string)

#ifndef NDEBUG
static int s_inGdb = 0;
#define NUM_TERM_NULL 1
#else
#define NUM_TERM_NULL 0
#endif

#define TRACE_REF(format, pointer, ...) PJ_LOG_TRACE("TRACE JVALUE_REF: %p " format, pointer, ##__VA_ARGS__)

#define J_CSTR_TO_BUF_EMPLACE(string) { (string), (sizeof(string) - 1) }
#define J_INVALID_VALUE -50

jvalue JNULL = {
	.m_type = JV_NULL,
	.m_refCnt = 1,
	.m_string = {
		J_CSTR_TO_BUF_EMPLACE("null"),
		NULL
	}
};

jvalue JINVALID = {
	.m_type = JV_NULL,
	.m_refCnt = 1,
	.m_string = {
		J_CSTR_TO_BUF_EMPLACE("null /* invalid */"),
		NULL
	}
};

static jbool JTRUE = {
	.m_value = {
		.m_type = JV_BOOL,
		.m_refCnt = 1,
		.m_string = {
			.buffer = J_CSTR_TO_BUF_EMPLACE("true"),
			.destructor = NULL
		}
	},
	.value = true
};

static jbool JFALSE = {
	.m_value = {
		.m_type = JV_BOOL,
		.m_refCnt = 1,
		.m_string = {
			.buffer = J_CSTR_TO_BUF_EMPLACE("false"),
			.destructor = NULL
		}
	},
	.value = false
};

static jstring JEMPTY_STR = {
	.m_value = {
		.m_type = JV_STR,
		.m_refCnt = 1,
		.m_string = {
			J_CSTR_TO_BUF_EMPLACE(""),
			NULL
		}
	},
	.m_dealloc = NULL,
	.m_data = {
		.m_str = "",
		.m_len = 0,
	}
};

static jvalue_ref jnumber_duplicate (jvalue_ref num) NON_NULL(1);
static bool jstring_equal_internal(jvalue_ref str, jvalue_ref other) NON_NULL(1, 2);
static inline bool jstring_equal_internal2(jvalue_ref str, raw_buffer *other) NON_NULL(1, 2);
static bool jstring_equal_internal3(raw_buffer *str, raw_buffer *other) NON_NULL(1, 2);

static bool jis_const(jvalue_ref val)
{
	assert( val->m_type != JV_NULL || val == &JNULL || val == &JINVALID );
	assert( val->m_type != JV_BOOL || val == &JTRUE.m_value || val == &JFALSE.m_value );

	switch (val->m_type)
	{
	case JV_NULL:
	case JV_BOOL:
		return true;
	default:
		return UNLIKELY(val == &JEMPTY_STR.m_value);
	}
}

bool jbuffer_equal(raw_buffer buffer1, raw_buffer buffer2)
{
	return buffer1.m_len == buffer2.m_len &&
			memcmp(buffer1.m_str, buffer2.m_str, buffer1.m_len) == 0;
}

void _jbuffer_munmap(_jbuffer *buf)
{
	munmap((void *)buf->buffer.m_str, buf->buffer.m_len);
	SANITY_KILL_POINTER(buf->buffer.m_str);
	buf->destructor = NULL;
}

void _jbuffer_free(_jbuffer *buf)
{
	free((void *)buf->buffer.m_str);
	SANITY_KILL_POINTER(buf->buffer.m_str);
	buf->destructor = NULL;
}

/**
 * NOTE: The function initializes new JSON value by the given type
 * @param val  The reference to a valid, structure
 * @param type The type of JSON value
 */
void jvalue_init (jvalue_ref val, JValueType type)
{
	val->m_refCnt = 1;
	val->m_type = type;
}

jvalue_ref jvalue_copy (jvalue_ref val)
{
	if (val == NULL) return NULL;

	SANITY_CHECK_POINTER(val);
	assert(s_inGdb || val->m_refCnt > 0);

	if (jis_const(val)) return val;

	g_atomic_int_inc(&val->m_refCnt);
	return val;
}

jvalue_ref jvalue_duplicate (jvalue_ref val)
{
	jvalue_ref result = val;
	SANITY_CHECK_POINTER(val);

	if (jis_const(val)) return result;

	if (jis_object (val)) {
		result = jobject_create_hint (jobject_size (val));
		jobject_iter it;
		jobject_key_value pair = {};

		jobject_iter_init(&it, val);
		while (jobject_iter_next(&it, &pair))
		{
			jvalue_ref valueCopy = jvalue_duplicate (pair.value);
			if (!jobject_put (result, jvalue_copy (pair.key), valueCopy)) {
				j_release (&result);
				result = NULL;
				break;
			}
		}
	} else if (jis_array (val)) {
		ssize_t arrSize = jarray_size (val);
		result = jarray_create_hint (NULL, arrSize);
		for (ssize_t i = 0; i < arrSize; ++i) {
			if (!jarray_append (result, jvalue_duplicate (jarray_get (val, i)))) {
				j_release (&result);
				result = NULL;
				break;
			}
		}
		return result;
	} else {
		// string, number, & boolean are immutable, so no need to do an actual duplication
		if (jis_string(val)) {
			result = jstring_create_copy(jstring_get_fast(val));
		} else if (jis_number(val)) {
			result = jnumber_duplicate(val);
		} else
			result = jboolean_create(jboolean_deref_to_value(val));
	}

	return result;
}

static bool jarray_equal(jvalue_ref arr, jvalue_ref other) NON_NULL(1, 2);
static bool jobject_equal(jvalue_ref obj, jvalue_ref other) NON_NULL(1, 2);
static int jstring_compare(const jvalue_ref str1, const jvalue_ref str2) NON_NULL(1, 2);
static int jarray_compare(const jvalue_ref arr1, const jvalue_ref arr2) NON_NULL(1, 2);
static int jobject_compare(const jvalue_ref obj1, const jvalue_ref obj2) NON_NULL(1, 2);

bool jvalue_equal(jvalue_ref val1, jvalue_ref val2)
{
	SANITY_CHECK_POINTER(val1);
	SANITY_CHECK_POINTER(val2);

	if (val1 == val2)
		return true;

	if (val1->m_type != val2->m_type)
		return false;

	switch (val1->m_type) {
		case JV_NULL:
			return true;
		case JV_BOOL:
			return jboolean_deref_to_value(val1) == jboolean_deref_to_value(val2);
		case JV_NUM:
			return jnumber_compare(val1, val2) == 0;
		case JV_STR:
			return jstring_equal(val1, val2);
		case JV_ARRAY:
			return jarray_equal(val1, val2);
		case JV_OBJECT:
			return jobject_equal(val1, val2);
	}

	return false;
}

int jvalue_compare(const jvalue_ref val1, const jvalue_ref val2)
{
	SANITY_CHECK_POINTER(val1);
	SANITY_CHECK_POINTER(val2);

	if (UNLIKELY(val1 == val2))
		return 0;

	int type_diff = (int)val1->m_type - (int)val2->m_type;
	if (type_diff != 0)
		return type_diff;

	switch (val1->m_type) {
		case JV_NULL:
			return (int)jis_valid(val1) - (int)jis_valid(val2);
		case JV_BOOL:
			return (int)jboolean_deref(val1)->value - (int)jboolean_deref(val2)->value;
		case JV_NUM:
			return jnumber_compare(val1, val2);
		case JV_STR:
			return jstring_compare(val1, val2);
		case JV_ARRAY:
			return jarray_compare(val1, val2);
		case JV_OBJECT:
			return jobject_compare(val1, val2);
	}

	PJ_LOG_ERR("Unknown type - corruption?");
	assert(false);
	return 0;
}

static void j_destroy_object (jvalue_ref obj) NON_NULL(1);
static void j_destroy_array (jvalue_ref arr) NON_NULL(1);
static void j_destroy_string (jvalue_ref str) NON_NULL(1);
static void j_destroy_number (jvalue_ref num) NON_NULL(1);

void j_release (jvalue_ref *val)
{
	SANITY_CHECK_POINTER(val);
	CHECK_POINTER(val);
	if (UNLIKELY(*val == NULL)) {
		SANITY_KILL_POINTER(*val);
		return;
	}
	if (UNLIKELY(jis_const(*val))) {
		SANITY_KILL_POINTER(*val);
		return;
	}

	assert((*val)->m_refCnt > 0);

	if (g_atomic_int_dec_and_test(&(*val)->m_refCnt)) {
		TRACE_REF("freeing because refcnt is 0: %s", *val, jvalue_tostring(*val, jschema_all()));
		_jbuffer *str = &(*val)->m_string;
		if (str->destructor) {
			PJ_LOG_MEM("Freeing string representation of jvalue %p", str->buffer.m_str);
			str->destructor(str);
		}

		_jbuffer *buf = &(*val)->m_file;
		if (buf->destructor)
			buf->destructor(buf);

		PJ_LOG_MEM("Freeing %p", *val);
		switch ((*val)->m_type) {
			case JV_OBJECT:
				j_destroy_object (*val);
				g_slice_free1(sizeof(jobject), *val);
				break;
			case JV_ARRAY:
				j_destroy_array (*val);
				g_slice_free1(sizeof(jarray), *val);
				break;
			case JV_STR:
				j_destroy_string (*val);
				free(*val);
				break;
			case JV_NUM:
				j_destroy_number (*val);
				g_slice_free1(sizeof(jnum), *val);
				break;
			case JV_BOOL:
			case JV_NULL:
				PJ_LOG_ERR("Invalid program state - should've already returned from j_release before this point");
				assert(false);
				break;
		}
	} else if (UNLIKELY((*val)->m_refCnt < 0)) {
		PJ_LOG_ERR("reference counter messed up - memory corruption and/or random crashes are possible");
		assert(false);
	}
	SANITY_KILL_POINTER(*val);
}

jvalue_ref jinvalid ()
{ return &JINVALID; }

static bool jis_valid_unsafe (jvalue_ref val)
{ return val != &JINVALID && val != NULL; }

bool jis_valid (jvalue_ref val)
{
	SANITY_CHECK_POINTER(val);
	assert( val == NULL
	     || val->m_type != JV_NULL
	     || val == &JNULL
	     || val == &JINVALID
	);
	return jis_valid_unsafe(val);
}

JValueType jget_type(jvalue_ref val)
{
	SANITY_CHECK_POINTER(val);
	assert(val);
	return val->m_type;
}

bool jis_null (jvalue_ref val)
{
	SANITY_CHECK_POINTER(val);
	assert( val == NULL
	     || val->m_type != JV_NULL
	     || val == &JNULL
	     || val == &JINVALID
	);
	return val == &JNULL || !jis_valid_unsafe(val);
}

jvalue_ref jnull ()
{
	return &JNULL;
}

/************************* JSON OBJECT API **************************************/

static unsigned long key_hash_raw (raw_buffer const *str) NON_NULL(1);
static unsigned long key_hash (jvalue_ref key) NON_NULL(1);

static unsigned long key_hash_raw (raw_buffer const *str)
{
	// djb2 algorithm
	unsigned long hash = 5381;
	char const *data = str->m_str;
	int count = str->m_len;

	assert(str->m_str != NULL);
	while (count--)
	{
		hash = ((hash << 5) + hash) + (int)(*data++);  // hash * 33 + c
	}
	return hash;
}

static bool check_insert_sanity(jvalue_ref parent, jvalue_ref child)
{
	// Sanity check that parent is object or array
	assert(jis_object(parent) || jis_array(parent));

	// Verify that the parent doesn't match the child
	if (UNLIKELY(child == parent)) {
		return false;
	}

	// Then check recursively child's children (if child is an array or an object)
	if (jis_array(child)) {
		for (int i = 0; i < jarray_size(child); i++) {
			jvalue_ref arr_elem = jarray_get(child, i);
			if(!check_insert_sanity(parent, arr_elem)) {
				return false;
			}
		}
	} else if (jis_object(child)) {
		jobject_iter it;
		jobject_key_value key_value;
		jobject_iter_init(&it, child);
		while (jobject_iter_next(&it, &key_value))
		{
			if(!check_insert_sanity(parent, key_value.value)) {
				return false;
			}
		}
	}

	return true;
}

static void j_destroy_object (jvalue_ref ref)
{
	g_hash_table_destroy(jobject_deref(ref)->m_members);
}

/* Has table key routines */
guint ObjKeyHash(gconstpointer key)
{
	jvalue_ref jkey = (jvalue_ref) key;
	return key_hash(jkey);
}

gboolean ObjKeyEqual(gconstpointer a, gconstpointer b)
{
	jvalue_ref ja = (jvalue_ref) a;
	jvalue_ref jb = (jvalue_ref) b;
	return jstring_equal_internal(ja, jb);
}

static void _ObjKeyValDestroy(gpointer data)
{
	jvalue_ref jdata = (jvalue_ref) data;
	j_release(&jdata);
}

jvalue_ref jobject_create ()
{
	jobject *new_obj = g_slice_new0(jobject);
	CHECK_ALLOC_RETURN_NULL(new_obj);
	jvalue_init((jvalue_ref)new_obj, JV_OBJECT);
	new_obj->m_members = g_hash_table_new_full(ObjKeyHash, ObjKeyEqual,
	                                           _ObjKeyValDestroy, _ObjKeyValDestroy);
	if (!new_obj->m_members)
	{
		g_slice_free1 (sizeof(jobject),new_obj);
		return NULL;
	}
	TRACE_REF("created", new_obj);
	return (jvalue_ref)new_obj;
}

static jvalue_ref jobject_put_keyvalue(jvalue_ref obj, jobject_key_value item)
{
	assert(jis_string(item.key));
	assert(item.value != NULL);

	if (UNLIKELY(!jis_valid(obj))) {
		j_release(&item.key);
		j_release(&item.value);
	}
	else if (UNLIKELY(!jobject_put(obj, item.key, item.value))) {
		PJ_LOG_ERR("Failed to insert requested key/value into new object");
		j_release (&obj);
		obj = jinvalid();
	}
	return obj;
}

jvalue_ref jobject_create_var (jobject_key_value item, ...)
{
	va_list ap;
	jvalue_ref new_object = jobject_create ();

	if (!new_object)
		new_object = jinvalid();

	if (item.key != NULL) {
		new_object = jobject_put_keyvalue(new_object, item);

		va_start (ap, item);
		while ((item = va_arg (ap, jobject_key_value)).key != NULL) {
			new_object = jobject_put_keyvalue(new_object, item);
		}
		va_end (ap);
	}

	return new_object;
}

jvalue_ref jobject_create_hint (int capacityHint)
{
	return jobject_create();
}

bool jis_object (jvalue_ref val)
{
	SANITY_CHECK_POINTER(val);
	CHECK_POINTER_RETURN_VALUE(val, false);
	assert((s_inGdb || val->m_refCnt > 0) && "val is garbage");

	return val->m_type == JV_OBJECT;
}

static bool jobject_equal(jvalue_ref obj, jvalue_ref other)
{
	SANITY_CHECK_POINTER(obj);
	SANITY_CHECK_POINTER(other);

	assert(jis_object(obj));
	assert(jis_object(other));

	if (jobject_size(obj) != jobject_size(other))
		return false;

	jobject_iter it;
	jobject_key_value pair = {};
	jobject_iter_init(&it, obj);
	while (jobject_iter_next(&it, &pair))
	{
		jvalue_ref val = NULL;
		if (!jobject_get_exists2(other, pair.key, &val))
			return false;

		if (!jvalue_equal(pair.value, val))
			return false;
	}

	return true;
}

static int qsort_helper(const void* p1, const void* p2)
{
	return jstring_compare(* (const jvalue_ref const *)p1, * (const jvalue_ref const *)p2);
}

static int jobject_compare(const jvalue_ref obj1, const jvalue_ref obj2)
{
	SANITY_CHECK_POINTER(obj1);
	SANITY_CHECK_POINTER(obj2);

	assert(jis_object(obj1));
	assert(jis_object(obj2));

	const ssize_t obj1_size = jobject_size(obj1);
	const ssize_t obj2_size = jobject_size(obj2);
	jvalue_ref obj1_keys[obj1_size];
	jvalue_ref obj2_keys[obj2_size];

	GHashTableIter iter;
	g_hash_table_iter_init (&iter, jobject_deref(obj1)->m_members);

	gpointer key;
	for (ssize_t i = 0; i < obj1_size; ++i)
	{
		(void) g_hash_table_iter_next(&iter, &key, NULL);
		obj1_keys[i] = (jvalue_ref)key;
	}

	g_hash_table_iter_init (&iter, jobject_deref(obj2)->m_members);
	for (ssize_t i = 0; i < obj2_size; ++i)
	{
		(void) g_hash_table_iter_next(&iter, &key, NULL);
		obj2_keys[i] = (jvalue_ref)key;
	}

	qsort(obj1_keys, obj1_size, sizeof(jvalue_ref), qsort_helper);
	qsort(obj2_keys, obj2_size, sizeof(jvalue_ref), qsort_helper);
	ssize_t size = obj1_size < obj2_size ? obj1_size : obj2_size;

	for (ssize_t i = 0; i < size; ++i)
	{
		int result = jstring_compare(obj1_keys[i], obj2_keys[i]);
		if (result != 0)
			return result;

		result = jvalue_compare((jvalue_ref)g_hash_table_lookup(jobject_deref(obj1)->m_members, obj1_keys[i]),
							   (jvalue_ref)g_hash_table_lookup(jobject_deref(obj2)->m_members, obj2_keys[i]));

		if (result != 0)
			return result;
	}

	return obj1_size - obj2_size;
}

size_t jobject_size(jvalue_ref obj)
{
	SANITY_CHECK_POINTER(obj);

	CHECK_CONDITION_RETURN_VALUE(!jis_object(obj), 0, "Attempt to retrieve size from something not an object %p", obj);

	if (!jobject_deref(obj)->m_members)
		return 0;
	return g_hash_table_size(jobject_deref(obj)->m_members);
}

bool jobject_get_exists (jvalue_ref obj, raw_buffer key, jvalue_ref *value)
{
	jstring jkey =
	{
		.m_value = {
			.m_refCnt = 1,
			.m_type = JV_STR,
		},
		.m_data = {
			.m_str = key.m_str,
			.m_len = key.m_len,
		},
	};

	return jobject_get_exists2(obj, &jkey.m_value, value);
}

bool jobject_get_exists2 (jvalue_ref obj, jvalue_ref key, jvalue_ref *value)
{
	jvalue_ref result;

	CHECK_CONDITION_RETURN_VALUE(jis_null(obj), false, "Attempt to cast null %p to object", obj);
	CHECK_CONDITION_RETURN_VALUE(!jis_object(obj), false, "Attempt to cast type %d to object (%d)", obj->m_type, JV_OBJECT);

	if (!jobject_deref(obj)->m_members)
		return false;

	result = g_hash_table_lookup(jobject_deref(obj)->m_members, key);
	if (!result)
		return false;

	if (value)
		*value = result;
	return true;
}

jvalue_ref jobject_get (jvalue_ref obj, raw_buffer key)
{
	jvalue_ref result = NULL;

	assert(key.m_str != NULL);
	if (jobject_get_exists (obj, key, &result))
		return result;
	return jinvalid();
}

jvalue_ref jobject_get_nested(jvalue_ref obj, ...)
{
	const char *key;
	va_list iter;

	va_start(iter, obj);
	while ((key = va_arg(iter, const char *))) {
		if (!jobject_get_exists(obj, j_cstr_to_buffer(key), &obj)) {
			obj = &JINVALID;
			break;
		}
	}
	va_end(iter);

	return obj;
}

bool jobject_remove (jvalue_ref obj, raw_buffer key)
{
	SANITY_CHECK_POINTER(obj);

	CHECK_CONDITION_RETURN_VALUE(jis_null(obj), false, "Attempt to cast null %p to object", obj);
	CHECK_CONDITION_RETURN_VALUE(!jis_object(obj), false, "Attempt to cast type %d to object (%d)", obj->m_type, JV_OBJECT);

	if (!jobject_deref(obj)->m_members)
		return false;

	jstring jkey =
	{
		.m_value = {
			.m_refCnt = 1,
			.m_type = JV_STR,
		},
		.m_data = {
			.m_str = key.m_str,
			.m_len = key.m_len,
		},
	};

	return g_hash_table_remove(jobject_deref(obj)->m_members, &jkey.m_value);
}

bool jobject_set (jvalue_ref obj, raw_buffer key, jvalue_ref val)
{
	jvalue_ref newKey, newVal;

	if (!jobject_deref(obj)->m_members)
		return false;

	newVal = jvalue_copy (val);
	//CHECK_CONDITION_RETURN_VALUE(jis_null(newVal) && !jis_null(val), false, "Failed to create a copy of the value")

	newKey = jstring_create_copy (key);
	if (!jis_valid_unsafe (newKey)) {
		PJ_LOG_ERR("Failed to create a copy of %.*s", (int)key.m_len, key.m_str);
		j_release (&newVal);
		return false;
	}

	return jobject_put(obj, newKey, newVal);
}

bool jobject_set2(jvalue_ref obj, jvalue_ref key, jvalue_ref val)
{
	jvalue_ref new_key = jvalue_copy (key);
	if (UNLIKELY(!new_key))
	{
		PJ_LOG_ERR("Failed to create a copy of key %p", key);
		return false;
	}

	jvalue_ref new_val = jvalue_copy (val);
	if (UNLIKELY(!new_val))
	{
		PJ_LOG_ERR("Failed to create a copy of val %p", val);
		j_release(&new_key);
		return false;
	}

	return jobject_put(obj, new_key, new_val);
}

bool jobject_put (jvalue_ref obj, jvalue_ref key, jvalue_ref val)
{
	SANITY_CHECK_POINTER(obj);
	SANITY_CHECK_POINTER(key);
	SANITY_CHECK_POINTER(val);

	assert(val != NULL);

	do {
		if (UNLIKELY(!jis_object(obj))) {
			PJ_LOG_ERR("%p is %d not an object (%d)", obj, obj->m_type, JV_OBJECT);
			break;
		}

		if (!jobject_deref(obj)->m_members) {
			break;
		}

		if (UNLIKELY(key == NULL)) {
			PJ_LOG_ERR("Invalid API use: null pointer");
			break;
		}

		if (UNLIKELY(!jis_string(key))) {
			PJ_LOG_ERR("%p is %d not a string (%d)", key, key->m_type, JV_STR);
			break;
		}

		if (UNLIKELY(jstring_size(key) == 0)) {
			PJ_LOG_ERR("Object instance name is the empty string");
			break;
		}

		if (val == NULL) {
			PJ_LOG_WARN("Please don't pass in NULL - use jnull() instead");
			val = jnull ();
		}

		if (!jis_valid(val)) {
			PJ_LOG_WARN("Passed invalid value converted to jnull()");
			val = jnull ();
		}

		if (!check_insert_sanity(obj, val)) {
			PJ_LOG_ERR("Error in object hierarchy. Inserting jvalue would create an illegal cyclic dependency");
			break;
		}

		g_hash_table_replace(jobject_deref(obj)->m_members, key, val);
		return true;
	} while (false);

	j_release(&key);
	j_release(&val);

	return false;
}

// JSON Object iterators
bool jobject_iter_init(jobject_iter *iter, jvalue_ref obj)
{
	SANITY_CHECK_POINTER(obj);

	CHECK_CONDITION_RETURN_VALUE(!jis_object(obj), false, "Cannot iterate over non-object");
	CHECK_CONDITION_RETURN_VALUE(!jobject_deref(obj)->m_members, false, "The object isn't iterable");

	g_hash_table_iter_init(&iter->m_iter, jobject_deref(obj)->m_members);
	return true;
}

bool jobject_iter_next(jobject_iter *iter, jobject_key_value *keyval)
{
	return g_hash_table_iter_next(&iter->m_iter,
	                              (gpointer *)&keyval->key, (gpointer *)&keyval->value);
}

/************************* JSON OBJECT API **************************************/

/************************* JSON ARRAY API  *************************************/

static bool jarray_put_unsafe (jvalue_ref arr, ssize_t index, jvalue_ref val) NON_NULL(1, 3);
static inline ssize_t jarray_size_unsafe (jvalue_ref arr) NON_NULL(1);
static inline void jarray_size_increment_unsafe (jvalue_ref arr) NON_NULL(1);
static inline void jarray_size_decrement_unsafe (jvalue_ref arr) NON_NULL(1);
static jvalue_ref* jarray_get_unsafe (jvalue_ref arr, ssize_t index) NON_NULL(1);
static inline void jarray_size_set_unsafe (jvalue_ref arr, ssize_t newSize) NON_NULL(1);
static bool jarray_expand_capacity_unsafe (jvalue_ref arr, ssize_t newSize) NON_NULL(1);
static void jarray_remove_unsafe (jvalue_ref arr, ssize_t index) NON_NULL(1);

static bool valid_index_bounded (jvalue_ref arr, ssize_t index) NON_NULL(1);
static bool valid_index_bounded (jvalue_ref arr, ssize_t index)
{
	SANITY_CHECK_POINTER(arr);
	CHECK_CONDITION_RETURN_VALUE(arr->m_type != JV_ARRAY, false, "Trying to test index bounds on non-array %p", arr);
	CHECK_CONDITION_RETURN_VALUE(index < 0, false, "Negative array index %zd", index);

	CHECK_CONDITION_RETURN_VALUE(index >= jarray_size_unsafe(arr), false, "Index %zd out of bounds of array size %zd", index, jarray_size(arr));

	return true;
}

static void j_destroy_array (jvalue_ref arr)
{
	SANITY_CHECK_POINTER(arr);
	SANITY_CHECK_POINTER(jarray_deref(arr)->m_bigBucket);
	assert(arr->m_type == JV_ARRAY);

#ifdef DEBUG_FREED_POINTERS
	for (ssize_t i = jarray_size_unsafe(arr); i < jarray_deref(arr)->m_capacity; i++) {
		jvalue_ref *outsideValue = jarray_get_unsafe(arr, i);
		assert(*outsideValue == NULL || *outsideValue == FREED_POINTER);
	}
#endif

	assert(jarray_size_unsafe(arr) >= 0);

	for (int i = jarray_size_unsafe(arr) - 1; i >= 0; i--)
		jarray_remove_unsafe(arr, i);

	assert(jarray_size_unsafe(arr) == 0);

	PJ_LOG_MEM("Destroying array bucket at %p", jarray_deref(arr)->m_bigBucket);
	SANITY_FREE(free, jvalue_ref *, jarray_deref(arr)->m_bigBucket, (size_t)(jarray_deref(arr)->m_capacity - ARRAY_BUCKET_SIZE));
}

jvalue_ref jarray_create (jarray_opts opts)
{
	jarray *new_array = g_slice_new0(jarray);
	CHECK_ALLOC_RETURN_NULL(new_array);
	jvalue_init((jvalue_ref)new_array, JV_ARRAY);

	new_array->m_capacity = ARRAY_BUCKET_SIZE;
	TRACE_REF("created", new_array);
	return (jvalue_ref)new_array;
}

jvalue_ref jarray_create_var (jarray_opts opts, ...)
{
	// jarray_create_hint will take care of the capacity for us
	jvalue_ref new_array = jarray_create_hint (opts, 1);
	jvalue_ref element;

	CHECK_POINTER_RETURN_NULL(new_array);

	va_list iter;

	va_start (iter, opts);
	while ( (element = va_arg (iter, jvalue_ref)) != NULL) {
		jarray_put_unsafe (new_array, jarray_size_unsafe (new_array), element);
	}
	va_end (iter);

	return new_array;
}

/**
 * Create an empty array with the specified properties and the hint that the array will eventually contain capacityHint elements.
 *
 * @param opts The options for the array (currently unspecified).  NULL indicates use default options.
 * @param capacityHint A guess-timate of the eventual size of the array (implementation is free to ignore this).
 * @return A reference to the created array value.  The caller has ownership.
 */
jvalue_ref jarray_create_hint (jarray_opts opts, size_t capacityHint)
{
	jvalue_ref new_array = jarray_create (opts);
	if (LIKELY(new_array != NULL)) {
		jarray_expand_capacity_unsafe (new_array, capacityHint);
	}

	return new_array;
}

/**
 * Determine whether or not the reference to the JSON value represents an array.
 *
 * @param val The reference to test
 * @return True if it is an array, false otherwise.
 */
bool jis_array (jvalue_ref val)
{
	SANITY_CHECK_POINTER(val);
	CHECK_POINTER_RETURN_VALUE(val, false);
	assert(s_inGdb || val->m_refCnt > 0);

	return val->m_type == JV_ARRAY;
}

static bool jarray_equal(jvalue_ref arr, jvalue_ref other)
{
	SANITY_CHECK_POINTER(arr);
	SANITY_CHECK_POINTER(other);

	assert(jis_array(arr));
	assert(jis_array(other));

	ssize_t size = jarray_size(arr);
	if (size != jarray_size(other))
		return false;

	for (ssize_t i = 0; i < size; ++i)
	{
		if (!jvalue_equal(jarray_get(arr, i), jarray_get(other, i)))
			return false;
	}

	return true;
}

static int jarray_compare(const jvalue_ref arr1, const jvalue_ref arr2)
{
	SANITY_CHECK_POINTER(arr1);
	SANITY_CHECK_POINTER(arr2);

	assert(jis_array(arr1));
	assert(jis_array(arr2));

	ssize_t arr1_size = jarray_size(arr1);
	ssize_t arr2_size = jarray_size(arr2);
	ssize_t size = arr1_size < arr2_size ? arr1_size : arr2_size;

	for (ssize_t i = 0; i < size; ++i)
	{
		int result = jvalue_compare(jarray_get(arr1, i), jarray_get(arr2, i));
		if (result != 0)
			return result;
	}

	return arr1_size - arr2_size;
}

ssize_t jarray_size (jvalue_ref arr)
{
	SANITY_CHECK_POINTER(arr);
	CHECK_CONDITION_RETURN_VALUE(!jis_array(arr), 0, "Attempt to get array size of non-array %p", arr);
	return jarray_size_unsafe (arr);
}

static inline ssize_t jarray_size_unsafe (jvalue_ref arr)
{
	assert(arr != NULL);
	assert(arr->m_type == JV_ARRAY);

	return jarray_deref(arr)->m_size;
}

static inline void jarray_size_increment_unsafe (jvalue_ref arr)
{
	assert(jis_array(arr));

	++jarray_deref(arr)->m_size;

	assert(jarray_size_unsafe(arr) <= jarray_deref(arr)->m_capacity);
}

static inline void jarray_size_decrement_unsafe (jvalue_ref arr)
{
	assert(arr != NULL);
	assert(arr->m_type == JV_ARRAY);

	--jarray_deref(arr)->m_size;

	assert(jarray_size_unsafe(arr) >= 0);
}

static inline void jarray_size_set_unsafe (jvalue_ref arr, ssize_t newSize)
{
	assert(jis_array(arr));
	assert(newSize <= jarray_deref(arr)->m_capacity);

	jarray_deref(arr)->m_size = newSize;
}

static jvalue_ref* jarray_get_unsafe (jvalue_ref arr, ssize_t index)
{
	assert(arr != NULL);
	assert(arr->m_type == JV_ARRAY);
	assert(index >= 0);
	assert(index < jarray_deref(arr)->m_capacity);

	if (UNLIKELY(OUTSIDE_ARR_BUCKET_RANGE(index))) {
		return &jarray_deref(arr)->m_bigBucket [index - ARRAY_BUCKET_SIZE];
	}
	assert(index < ARRAY_BUCKET_SIZE);
	return &jarray_deref(arr)->m_smallBucket [index];
}

jvalue_ref jarray_get (jvalue_ref arr, ssize_t index)
{
	jvalue_ref result;

	CHECK_CONDITION_RETURN_VALUE(!valid_index_bounded(arr, index), jinvalid(), "Attempt to get array element from %p with out-of-bounds index value %zd", arr, index);

	result = * (jarray_get_unsafe (arr, index));
	if (result == NULL)
	// need to fix up in case we haven't assigned anything to that space - it's initialized to NULL (JSON undefined)
	result = jinvalid ();
	return result;
}

static void jarray_remove_unsafe (jvalue_ref arr, ssize_t index)
{
	ssize_t i;
	jvalue_ref *hole, *toMove;
	ssize_t array_size;

	assert(valid_index_bounded(arr, index));

	hole = jarray_get_unsafe (arr, index);
	assert (hole != NULL);
	j_release (hole);

	array_size = jarray_size_unsafe (arr);

	// Shift down all elements
	for (i = index + 1; i < array_size; i++) {
		toMove = jarray_get_unsafe (arr, i);
		*hole = *toMove;
		hole = toMove;
	}

	jarray_size_decrement_unsafe (arr);

	// This is necessary because someone else might reference this position, and
	// they need to know that it's empty (in case they need to free it).
	*hole = NULL;
}

bool jarray_remove (jvalue_ref arr, ssize_t index)
{
	CHECK_CONDITION_RETURN_VALUE(!valid_index_bounded(arr, index), false, "Attempt to get array element from %p with out-of-bounds index value %zd", arr, index);

	jarray_remove_unsafe (arr, index);

	return true;
}

static bool jarray_expand_capacity_unsafe (jvalue_ref arr, ssize_t newSize)
{
	assert(jis_array(arr));
	assert(newSize >= 0);

	if (newSize > jarray_deref(arr)->m_capacity) {
		// m_capacity is always a minimum of the bucket size
		assert(OUTSIDE_ARR_BUCKET_RANGE(newSize));
		assert(newSize > ARRAY_BUCKET_SIZE);
		jvalue_ref *newBigBucket = realloc (jarray_deref(arr)->m_bigBucket, sizeof(jvalue_ref) * (newSize - ARRAY_BUCKET_SIZE));
		if (UNLIKELY(newBigBucket == NULL)) {
			assert(false);
			return false;
		}

		PJ_LOG_MEM("Resized %p from %zu bytes to %p with %zu bytes", jarray_deref(arr)->m_bigBucket, sizeof(jvalue_ref)*(jarray_deref(arr)->m_capacity - ARRAY_BUCKET_SIZE), newBigBucket, sizeof(jvalue_ref)*(newSize - ARRAY_BUCKET_SIZE));

		for (ssize_t x = jarray_deref(arr)->m_capacity - ARRAY_BUCKET_SIZE; x < newSize - ARRAY_BUCKET_SIZE; x++)
			newBigBucket[x] = NULL;

		jarray_deref(arr)->m_bigBucket = newBigBucket;
		jarray_deref(arr)->m_capacity = newSize;
	}

	return true;
}

static bool jarray_put_unsafe (jvalue_ref arr, ssize_t index, jvalue_ref val)
{
	jvalue_ref *old;
	SANITY_CHECK_POINTER(arr);
	assert(jis_array(arr));

	if (!check_insert_sanity(arr, val)) {
		PJ_LOG_ERR("Error in object hierarchy. Inserting jvalue would create an illegal cyclic dependency");
		return false;
	}

	if (!jarray_expand_capacity_unsafe (arr, index + 1)) {
		PJ_LOG_WARN("Failed to expand array to allocate element - memory allocation problem?");
		return false;
	}

	old = jarray_get_unsafe(arr, index);
	j_release(old);
	*old = val;

	if (index >= jarray_size_unsafe (arr)) jarray_size_set_unsafe (arr, index + 1);

	return true;
}

bool jarray_set (jvalue_ref arr, ssize_t index, jvalue_ref val)
{
	jvalue_ref arr_val;

	CHECK_CONDITION_RETURN_VALUE(!jis_array(arr), false, "Attempt to get array size of non-array %p", arr);
	CHECK_CONDITION_RETURN_VALUE(index < 0, false, "Attempt to set array element for %p with negative index value %zd", arr, index);

	if (UNLIKELY(val == NULL)) {
		PJ_LOG_WARN("incorrect API use - please pass an actual reference to a JSON null if that's what you want - assuming that's what you meant");
		val = jnull ();
	}

	arr_val = jvalue_copy (val);
	CHECK_ALLOC_RETURN_VALUE(arr_val, false);

	return jarray_put_unsafe (arr, index, arr_val);
}

bool jarray_put (jvalue_ref arr, ssize_t index, jvalue_ref val)
{
	do {
		if (!jis_array(arr)) {
			PJ_LOG_ERR("Attempt to insert into non-array %p", arr);
			break;
		}

		if (index < 0) {
			PJ_LOG_ERR("Attempt to insert array element for %p with negative index value %zd", arr, index);
			break;
		}

		if (UNLIKELY(val == NULL)) {
			PJ_LOG_WARN("incorrect API use - please pass an actual reference to a JSON null if that's what you want - assuming that's the case");
			val = jnull ();
		}

		if (!jarray_put_unsafe (arr, index, val)) {
			break;
		}

		return true;
	} while (false);

	j_release(&val);

	return false;
}

bool jarray_append (jvalue_ref arr, jvalue_ref val)
{
	SANITY_CHECK_POINTER(val);
	SANITY_CHECK_POINTER(arr);

	CHECK_CONDITION_RETURN_VALUE(!jis_array(arr), false, "Attempt to append into non-array %p", arr);

	if (UNLIKELY(val == NULL)) {
		PJ_LOG_WARN("incorrect API use - please pass an actual reference to a JSON null if that's what you want - assuming that's the case");
		val = jnull ();
	}

	return jarray_put_unsafe (arr, jarray_size_unsafe (arr), val);
}

/**
 * Insert the value into the array before the specified position.
 *
 * arr[index] now contains val, with all elements shifted appropriately.
 *
 * NOTE: It is unspecified behaviour to modify val after passing it to this array
 *
 * @param arr
 * @param index
 * @param val
 *
 * @see jarray_append
 * @see jarray_put
 */
bool jarray_insert(jvalue_ref arr, ssize_t index, jvalue_ref val)
{
	SANITY_CHECK_POINTER(arr);

	CHECK_CONDITION_RETURN_VALUE(!jis_array(arr), false, "Array to insert into isn't a valid reference to a JSON DOM node: %p", arr);
	CHECK_CONDITION_RETURN_VALUE(index < 0, false, "Invalid index - must be >= 0: %zd", index);

	if (!check_insert_sanity(arr, val)) {
		PJ_LOG_ERR("Error in object hierarchy. Inserting jvalue would create an illegal cyclic dependency");
		return false;
	}

	{
		jvalue_ref *toMove, *hole;
		// we increment the size of the array
		jarray_put_unsafe(arr, jarray_size_unsafe(arr), jinvalid());

		// stopping at the first jis_null as an optimization is actually
		// wrong because we change the array structure.  we have to move up
		// all the elements.
		hole = jarray_get_unsafe(arr, jarray_size_unsafe(arr) - 1);

		for (ssize_t j = jarray_size_unsafe(arr) - 1; j > index; j--, hole = toMove) {
			toMove = jarray_get_unsafe(arr, j - 1);
			*hole = *toMove;
		}

		*hole = val;
	}

	return true;
}

// Helper function to check insert sanity for jarray_splice
static bool jarray_splice_check_insert_sanity(jvalue_ref arr, jvalue_ref arr2)
{
	assert(jis_array(arr));
	assert(jis_array(arr2));

	for (int i = 0; i < jarray_size(arr2); i++) {
		jvalue_ref arr_elem = jarray_get(arr2, i);
		if (!check_insert_sanity(arr, arr_elem)) {
			return false;
		}
	}

	return true;
}

bool jarray_splice (jvalue_ref array, ssize_t index, ssize_t toRemove, jvalue_ref array2, ssize_t begin, ssize_t end, JSpliceOwnership ownership)
{
	ssize_t i, j;
	size_t removable = toRemove;
	jvalue_ref *valueInOtherArray;
	jvalue_ref valueToInsert;

	if (LIKELY(removable)) {
		CHECK_CONDITION_RETURN_VALUE(!valid_index_bounded(array, index), false, "Splice index is invalid");
		CHECK_CONDITION_RETURN_VALUE(!valid_index_bounded(array, index + toRemove - 1), false, "To remove amount is out of bounds of array");
	} else {
		SANITY_CHECK_POINTER(array);
		CHECK_CONDITION_RETURN_VALUE(!jis_array(array), false, "Array isn't valid %p", array);
		if (index < 0) index = 0;
	}
	CHECK_CONDITION_RETURN_VALUE(begin >= end, false, "Invalid range to copy from second array: [%zd, %zd)", begin, end); // set notation
	CHECK_CONDITION_RETURN_VALUE(!valid_index_bounded(array2, begin), false, "Start index is invalid for second array");
	CHECK_CONDITION_RETURN_VALUE(!valid_index_bounded(array2, end - 1), false, "End index is invalid for second array");
	CHECK_CONDITION_RETURN_VALUE(toRemove < 0, false, "Invalid amount %zd to remove during splice", toRemove);

	if (!jarray_splice_check_insert_sanity(array, array2)) {
		PJ_LOG_ERR("Error in object hierarchy. Splicing array would create an illegal cyclic dependency");
		return false;
	}

	for (i = index, j = begin; removable && j < end; i++, removable--, j++) {
		assert(valid_index_bounded(array, i));
		assert(valid_index_bounded(array2, j));
		valueInOtherArray = jarray_get_unsafe(array2, j);
		valueToInsert = *valueInOtherArray;
		assert(valueInOtherArray != NULL);
		switch (ownership) {
			case SPLICE_TRANSFER:
				*valueInOtherArray = NULL;
				jarray_size_decrement_unsafe (array2);
				break;
			case SPLICE_NOCHANGE:
				break;
			case SPLICE_COPY:
				valueToInsert = jvalue_copy(valueToInsert);
				break;
		}
		jarray_put_unsafe (array, i, valueToInsert);
	}

	if (removable != 0) {
		assert (j == end);
		assert (toRemove > end - begin);

		while (removable) {
			jarray_remove_unsafe (array, i);
			removable--;
		}
	} else if (j < end) {
		assert (toRemove < end - begin);
		assert (removable == 0);

		jarray_expand_capacity_unsafe (array, jarray_size_unsafe (array) + (end - j));

		// insert any remaining elements that don't overlap the amount to remove
		for (; j < end; j++, i++) {
			assert(valid_index_bounded(array2, j));

			valueInOtherArray = jarray_get_unsafe(array2, j);
			valueToInsert = *valueInOtherArray;
			assert(valueInOtherArray != NULL);
			switch (ownership) {
				case SPLICE_TRANSFER:
					*valueInOtherArray = NULL;
					jarray_size_decrement_unsafe (array2);
					break;
				case SPLICE_NOCHANGE:
					break;
				case SPLICE_COPY:
					valueToInsert = jvalue_copy(valueToInsert);
					break;
			}
			if (UNLIKELY(!jarray_insert(array, i, valueToInsert))) {
				PJ_LOG_ERR("How did this happen? Failed to insert %zd from second array into %zd of first array", j, i);
				return false;
			}
		}
	} else {
		assert (toRemove == end - begin);
	}
	return true;
}

bool jarray_splice_inject (jvalue_ref array, ssize_t index, jvalue_ref arrayToInject, JSpliceOwnership ownership)
{
	return jarray_splice (array, index, 0, arrayToInject, 0, jarray_size (arrayToInject), ownership);
}

bool jarray_splice_append (jvalue_ref array, jvalue_ref arrayToAppend, JSpliceOwnership ownership)
{
	return jarray_splice (array, jarray_size (array) - 1, 0, arrayToAppend, 0, jarray_size (arrayToAppend), ownership);
}

bool jarray_has_duplicates(jvalue_ref arr)
{
	SANITY_CHECK_POINTER(arr);

	assert(jis_array(arr));

	ssize_t size = jarray_size(arr);

	for (ssize_t i = 0; i < size - 1; ++i)
	{
		jvalue_ref jvali = *jarray_get_unsafe(arr, i);
		for (ssize_t j = i + 1; j < size; ++j)
		{
			if (jvalue_equal(jvali, *jarray_get_unsafe(arr, j)))
				return true;
		}
	}

	return false;
}


/****************************** JSON STRING API ************************/
#define SANITY_CHECK_JSTR_BUFFER(jval)					\
	do {								\
		SANITY_CHECK_POINTER(jval);				\
		SANITY_CHECK_POINTER(jstring_deref(jval)->m_data.m_str);	\
		SANITY_CHECK_MEMORY(jstring_deref(jval)->m_data.m_str, jstring_deref(jval)->m_data.m_len);	\
		SANITY_CHECK_POINTER(jstring_deref(jval)->m_dealloc);	\
	} while (0)

static void j_destroy_string (jvalue_ref str)
{
	SANITY_CHECK_POINTER(str);
	assert(jstring_deref(str) != &JEMPTY_STR);
	SANITY_CHECK_JSTR_BUFFER(str);
#ifdef _DEBUG
	if (str == NULL) {
		PJ_LOG_ERR("Internal error - string reference to release the string buffer for is NULL");
		return;
	}
#endif
	if (jstring_deref(str)->m_dealloc) {
		PJ_LOG_MEM("Destroying string %p", jstring_deref(str)->m_data.m_str);
		jstring_deref(str)->m_dealloc((char*)jstring_deref(str)->m_data.m_str);
	}
	PJ_LOG_MEM("Changing string %p to NULL for %p", jstring_deref(str)->m_data.m_str, str);
	SANITY_KILL_POINTER(jstring_deref(str)->m_data.m_str);
	SANITY_CLEAR_VAR(jstring_deref(str)->m_data.m_len, -1);
}

bool jis_string_unsafe (jvalue_ref str)
{ return str->m_type == JV_STR; }

static unsigned long key_hash (jvalue_ref key)
{
	assert(jis_string_unsafe(key));
	return key_hash_raw (&jstring_deref(key)->m_data);
}

jvalue_ref jstring_empty ()
{
	return &JEMPTY_STR.m_value;
}

jvalue_ref jstring_create (const char *cstring)
{
	return jstring_create_utf8 (cstring, strlen (cstring));
}

jvalue_ref jstring_create_utf8 (const char *cstring, ssize_t length)
{
	if (length < 0) length = strlen (cstring);
	return jstring_create_copy (j_str_to_buffer (cstring, length));
}

jvalue_ref jstring_create_copy (raw_buffer str)
{
	// size include 1 byte for ASCII and UTF-8 terminator
	jstring_inline *new_str = (jstring_inline*) calloc (1, sizeof(jstring_inline) + str.m_len + 1);
	CHECK_POINTER_RETURN_NULL(new_str);
	jvalue_init((jvalue_ref)new_str, JV_STR);

	memcpy(new_str->m_buf, str.m_str, str.m_len);
	new_str->m_header.m_dealloc = NULL;
	new_str->m_header.m_data = j_str_to_buffer(new_str->m_buf, str.m_len);

	return (jvalue_ref)new_str;
}

bool jis_string (jvalue_ref str)
{
#ifdef DEBUG_FREED_POINTERS
	if (str->m_type == JV_STR)
		SANITY_CHECK_JSTR_BUFFER(str);
#endif
	CHECK_POINTER_RETURN_VALUE(str, false);
	assert(s_inGdb || str->m_refCnt > 0);

	return jis_string_unsafe(str);
}

jvalue_ref jstring_create_from_pool_internal(dom_string_memory_pool* pool, const char *data, size_t len)
{
	jstring *string = calloc(1, sizeof(jstring));
	CHECK_POINTER_RETURN_NULL(string);

	char *buffer = dom_string_memory_pool_alloc(pool, len + 1);
	memcpy(buffer, data, len);
	buffer[len] = '\0';

	jvalue_init((jvalue_ref)string, JV_STR);

	string->m_dealloc = dom_string_memory_pool_mark_as_free;
	string->m_data = j_str_to_buffer(buffer, len);

	return (jvalue_ref)string;
}

jvalue_ref jnumber_create_from_pool_internal(dom_string_memory_pool* pool, const char *data, size_t len)
{
	assert(data != NULL && len > 0);

	jnum *new_number = g_slice_new0(jnum);
	CHECK_ALLOC_RETURN_NULL(new_number);
	jvalue_init((jvalue_ref)new_number, JV_NUM);

	char *buffer = dom_string_memory_pool_alloc(pool, len + 1);
	memcpy(buffer, data, len);
	buffer[len] = '\0';

	new_number->m_type = NUM_RAW;
	new_number->value.raw = j_str_to_buffer(buffer, len);
	new_number->m_rawDealloc = dom_string_memory_pool_mark_as_free;

	TRACE_REF("created", new_number);
	return (jvalue_ref)new_number;
}

jvalue_ref jstring_create_nocopy (raw_buffer val)
{
	return jstring_create_nocopy_full (val, NULL);
}

jvalue_ref jstring_create_nocopy_full (raw_buffer val, jdeallocator buffer_dealloc)
{
	SANITY_CHECK_POINTER(val.m_str);
	SANITY_CHECK_MEMORY(val.m_str, val.m_len);
	CHECK_CONDITION_RETURN_VALUE(val.m_str == NULL, jinvalid(), "Invalid string to set JSON string to NULL");
	if (val.m_len == 0) {
		if (buffer_dealloc) buffer_dealloc((void *)val.m_str);
		return &JEMPTY_STR.m_value;
	}

	jstring *new_string = (jstring *) calloc(1, sizeof(jstring));
	CHECK_ALLOC_RETURN_NULL(new_string);
	jvalue_init((jvalue_ref)new_string, JV_STR);

	new_string->m_dealloc = buffer_dealloc;
	new_string->m_data = val;

	SANITY_CHECK_JSTR_BUFFER((jvalue_ref)new_string);

	TRACE_REF("created", new_string);
	return (jvalue_ref)new_string;
}

ssize_t jstring_size (jvalue_ref str)
{
	SANITY_CHECK_JSTR_BUFFER(str);
	CHECK_CONDITION_RETURN_VALUE(!jis_string(str), 0, "Invalid parameter - %d is not a string (%d)", str->m_type, JV_STR);

	assert(jstring_deref(str)->m_data.m_str);

	return jstring_deref(str)->m_data.m_len;
}

raw_buffer jstring_get (jvalue_ref str)
{
	SANITY_CHECK_JSTR_BUFFER(str);
	char *str_copy;

	// performs the error checking for us as well
	raw_buffer raw_str = jstring_get_fast (str);
	if (UNLIKELY(raw_str.m_str == NULL)) return raw_str;

	str_copy = calloc (raw_str.m_len + 1, sizeof(char));
	if (str_copy == NULL) {
		return j_str_to_buffer (NULL, 0);
	}

	memcpy (str_copy, raw_str.m_str, raw_str.m_len);

	return j_str_to_buffer (str_copy, raw_str.m_len);
}

raw_buffer jstring_get_fast (jvalue_ref str)
{
	SANITY_CHECK_JSTR_BUFFER(str);
	CHECK_CONDITION_RETURN_VALUE(!jis_string(str), j_str_to_buffer(NULL, 0), "Invalid API use - attempting to get string buffer for non JSON string %p", str);

	return jstring_deref(str)->m_data;
}

static bool jstring_equal_internal(jvalue_ref str, jvalue_ref other)
{
	SANITY_CHECK_JSTR_BUFFER(str);
	SANITY_CHECK_JSTR_BUFFER(other);
	return str == other ||
			jstring_equal_internal2(str, &jstring_deref(other)->m_data);
}

static inline bool jstring_equal_internal2(jvalue_ref str, raw_buffer *other)
{
	SANITY_CHECK_JSTR_BUFFER(str);
	SANITY_CHECK_MEMORY(other->m_str, other->m_len);
	return jstring_equal_internal3(&jstring_deref(str)->m_data, other);
}

static bool jstring_equal_internal3(raw_buffer *str, raw_buffer *other)
{
	SANITY_CHECK_MEMORY(str->m_str, str->m_len);
	SANITY_CHECK_MEMORY(other->m_str, other->m_len);
	return str->m_str == other->m_str ||
			(
					str->m_len == other->m_len &&
					memcmp(str->m_str, other->m_str, str->m_len) == 0
			);
}

bool jstring_equal (jvalue_ref str, jvalue_ref other)
{
	SANITY_CHECK_JSTR_BUFFER(str);
	SANITY_CHECK_JSTR_BUFFER(other);

	if (UNLIKELY(!jis_string(str) || !jis_string(other))) {
		PJ_LOG_WARN("attempting to check string equality but not using a JSON string");
		return false;
	}

	return jstring_equal_internal(str, other);
}

bool jstring_equal2 (jvalue_ref str, raw_buffer other)
{
	if (UNLIKELY(!jis_string(str))) {
		PJ_LOG_WARN("attempting to check string equality but not a JSON string");
		return false;
	}

	return jstring_equal_internal2(str, &other);
}

static int jstring_compare(const jvalue_ref str1, const jvalue_ref str2)
{
	SANITY_CHECK_JSTR_BUFFER(str1);
	SANITY_CHECK_JSTR_BUFFER(str2);

	ssize_t str1_size = jstring_size(str1);
	ssize_t str2_size = jstring_size(str2);
	ssize_t size = str1_size < str2_size ? str1_size : str2_size;

	int result = memcmp(jstring_deref(str1)->m_data.m_str, jstring_deref(str2)->m_data.m_str, size);
	if (result != 0)
		return result;

	return str1_size - str2_size;
}

/******************************** JSON NUMBER API **************************************/

static void j_destroy_number (jvalue_ref num)
{
	SANITY_CHECK_POINTER(num);
	assert(num->m_type == JV_NUM);

	if (jnum_deref(num)->m_type != NUM_RAW) {
		return;
	}

	assert(jnum_deref(num)->value.raw.m_str != NULL);
	SANITY_CHECK_POINTER(jnum_deref(num)->value.raw.m_str);

	if (jnum_deref(num)->m_rawDealloc) {
		PJ_LOG_MEM("Destroying raw numeric string %p", jnum_deref(num)->value.raw.m_str);
		jnum_deref(num)->m_rawDealloc ((char *)jnum_deref(num)->value.raw.m_str);
	}
	PJ_LOG_MEM("Clearing raw numeric string from %p to NULL for %p", jnum_deref(num)->value.raw.m_str, num);
	SANITY_KILL_POINTER(jnum_deref(num)->value.raw.m_str);
	SANITY_CLEAR_VAR(jnum_deref(num)->value.raw.m_len, 0);
}

jvalue_ref jnumber_duplicate (jvalue_ref num)
{
	assert (jis_number(num));

	switch (jnum_deref(num)->m_type) {
	case NUM_RAW:
		return jnumber_create(jnum_deref(num)->value.raw);
	case NUM_FLOAT:
		return jnumber_create_f64(jnum_deref(num)->value.floating);
	case NUM_INT:
		return jnumber_create_i64(jnum_deref(num)->value.integer);
	}
	assert(false);
	return jinvalid();
}

jvalue_ref jnumber_create (raw_buffer str)
{
	char *createdBuffer = NULL;
	jvalue_ref new_number;

	assert(str.m_str != NULL);
	assert(str.m_len > 0);

	CHECK_POINTER_RETURN_VALUE(str.m_str, jinvalid());
	CHECK_CONDITION_RETURN_VALUE(str.m_len <= 0, jinvalid(), "Invalid length parameter for numeric string %s", str.m_str);

	createdBuffer = (char *) calloc (str.m_len + NUM_TERM_NULL, sizeof(char));
	CHECK_ALLOC_RETURN_VALUE(createdBuffer, jinvalid());

	memcpy (createdBuffer, str.m_str, str.m_len);
	str.m_str = createdBuffer;
	new_number = jnumber_create_unsafe(str, free);
	if (!jis_valid_unsafe(new_number))
		free(createdBuffer);

	return new_number;
}

jvalue_ref jnumber_create_unsafe (raw_buffer str, jdeallocator strFree)
{
	assert(str.m_str != NULL);
	assert(str.m_len > 0);

	CHECK_POINTER_RETURN_VALUE(str.m_str, jinvalid());
	CHECK_CONDITION_RETURN_VALUE(str.m_len == 0, jinvalid(), "Invalid length parameter for numeric string %s", str.m_str);

	jnum *new_number = g_slice_new0(jnum);
	CHECK_ALLOC_RETURN_NULL(new_number);
	jvalue_init((jvalue_ref)new_number, JV_NUM);

	new_number->m_type = NUM_RAW;
	new_number->value.raw = str;
	new_number->m_rawDealloc = strFree;

	TRACE_REF("created", new_number);
	return (jvalue_ref)new_number;
}

jvalue_ref jnumber_create_f64 (double number)
{
	CHECK_CONDITION_RETURN_VALUE(isnan(number), jinvalid(), "NaN has no representation in JSON");
	CHECK_CONDITION_RETURN_VALUE(isinf(number), jinvalid(), "Infinity has no representation in JSON");

	jnum *new_number = g_slice_new0(jnum);
	CHECK_ALLOC_RETURN_NULL(new_number);
	jvalue_init((jvalue_ref)new_number, JV_NUM);

	new_number->m_type = NUM_FLOAT;
	new_number->value.floating = number;

	TRACE_REF("created", new_number);
	return (jvalue_ref)new_number;
}

jvalue_ref jnumber_create_i32 (int32_t number)
{
	return jnumber_create_i64 (number);
}

jvalue_ref jnumber_create_i64 (int64_t number)
{
	jnum *new_number = g_slice_new0(jnum);
	CHECK_ALLOC_RETURN_NULL(new_number);
	jvalue_init((jvalue_ref)new_number, JV_NUM);

	new_number->m_type = NUM_INT;
	new_number->value.integer = number;

	TRACE_REF("created", new_number);
	return (jvalue_ref)new_number;
}

jvalue_ref jnumber_create_converted(raw_buffer raw)
{
	jnum *new_number = g_slice_new0(jnum);
	CHECK_ALLOC_RETURN_NULL(new_number);
	jvalue_init((jvalue_ref)new_number, JV_NUM);

	if (CONV_OK != jstr_to_i64(&raw, &new_number->value.integer)) {
		new_number->m_error = jstr_to_double(&raw, &new_number->value.floating);
		if (new_number->m_error != CONV_OK) {
			PJ_LOG_ERR("Number '%.*s' doesn't convert perfectly to a native type",
					(int)raw.m_len, raw.m_str);
			assert(false);
		}
	}

	TRACE_REF("created", new_number);
	return (jvalue_ref)new_number;
}

int jnumber_compare(jvalue_ref number, jvalue_ref toCompare)
{
	SANITY_CHECK_POINTER(number);
	SANITY_CHECK_POINTER(toCompare);

	assert(jis_number(number));
	assert(jis_number(toCompare));

	switch (jnum_deref(toCompare)->m_type) {
		case NUM_FLOAT:
			return jnumber_compare_f64(number, jnum_deref(toCompare)->value.floating);
		case NUM_INT:
			return jnumber_compare_i64(number, jnum_deref(toCompare)->value.integer);
		case NUM_RAW:
		{
			int64_t asInt;
			double asFloat;
			if (CONV_OK == jstr_to_i64(&jnum_deref(toCompare)->value.raw, &asInt))
				return jnumber_compare_i64(number, asInt);
			if (CONV_OK != jstr_to_double(&jnum_deref(toCompare)->value.raw, &asFloat)) {
				PJ_LOG_ERR("Comparing against something that can't be represented as a float: '%.*s'",
						(int)jnum_deref(toCompare)->value.raw.m_len, jnum_deref(toCompare)->value.raw.m_str);
			}
			return jnumber_compare_f64(number, asFloat);
		}
		default:
			PJ_LOG_ERR("Unknown type for toCompare - corruption?");
			assert(false);
			return J_INVALID_VALUE;
	}
}

int jnumber_compare_i64(jvalue_ref number, int64_t toCompare)
{
	SANITY_CHECK_POINTER(number);
	assert(jis_number(number));

	switch (jnum_deref(number)->m_type) {
		case NUM_FLOAT:
			return jnum_deref(number)->value.floating > toCompare ? 1 :
				(jnum_deref(number)->value.floating < toCompare ? -1 : 0);
		case NUM_INT:
			return jnum_deref(number)->value.integer > toCompare ? 1 :
				(jnum_deref(number)->value.integer < toCompare ? -1 : 0);
		case NUM_RAW:
		{
			int64_t asInt;
			if (CONV_OK == jstr_to_i64(&jnum_deref(number)->value.raw, &asInt)) {
				return asInt > toCompare ? 1 :
						(asInt < toCompare ? -1 : 0);
			}
			double asFloat;
			if (CONV_OK != jstr_to_double(&jnum_deref(number)->value.raw, &asFloat)) {
				PJ_LOG_ERR("Comparing '%"PRId64 "' against something that can't be represented as a float: '%.*s'",
						toCompare, (int)jnum_deref(number)->value.raw.m_len, jnum_deref(number)->value.raw.m_str);
			}
			return asFloat > toCompare ? 1 : (asFloat < toCompare ? -1 : 0);
		}
		default:
			PJ_LOG_ERR("Unknown type - corruption?");
			assert(false);
			return J_INVALID_VALUE;
	}
}

int jnumber_compare_f64(jvalue_ref number, double toCompare)
{
	SANITY_CHECK_POINTER(number);
	assert(jis_number(number));

	switch (jnum_deref(number)->m_type) {
		case NUM_FLOAT:
			return jnum_deref(number)->value.floating > toCompare ? 1 :
				(jnum_deref(number)->value.floating < toCompare ? -1 : 0);
		case NUM_INT:
			return jnum_deref(number)->value.integer > toCompare ? 1 :
				(jnum_deref(number)->value.integer < toCompare ? -1 : 0);
		case NUM_RAW:
		{
			int64_t asInt;
			if (CONV_OK == jstr_to_i64(&jnum_deref(number)->value.raw, &asInt)) {
				return asInt > toCompare ? 1 :
						(asInt < toCompare ? -1 : 0);
			}
			double asFloat;
			if (CONV_OK != jstr_to_double(&jnum_deref(number)->value.raw, &asFloat)) {
				PJ_LOG_ERR("Comparing '%lf' against something that can't be represented as a float: '%.*s'",
						toCompare, (int)jnum_deref(number)->value.raw.m_len, jnum_deref(number)->value.raw.m_str);
			}
			return asFloat > toCompare ? 1 : (asFloat < toCompare ? -1 : 0);
		}
		default:
			PJ_LOG_ERR("Unknown type - corruption?");
			assert(false);
			return J_INVALID_VALUE;
	}
}

bool jnumber_has_error (jvalue_ref number)
{
	return jnum_deref(number)->m_error != CONV_OK;
}

bool jis_number (jvalue_ref num)
{
	SANITY_CHECK_POINTER(num);
	CHECK_POINTER_RETURN_VALUE(num, false);
	assert(s_inGdb || num->m_refCnt > 0);

	return num->m_type == JV_NUM;
}

int64_t jnumber_deref_i64(jvalue_ref num)
{
	int64_t result;
	ConversionResultFlags fail;
	if (CONV_OK != (fail = jnumber_get_i64(num, &result))) {
		PJ_LOG_WARN("Converting JSON value to a 64-bit integer but ignoring the conversion error: %d", fail);
	}
	return result;
}


raw_buffer jnumber_deref_raw(jvalue_ref num)
{
	// initialized to 0 just to get around compiler warning for
	// now - it is really up to the caller to ensure they do not
	// call this on something that is not a raw number.
	raw_buffer result = { 0 };
	jnumber_get_raw(num, &result);
	return result;
}

ConversionResultFlags jnumber_get_i32 (jvalue_ref num, int32_t *number)
{
	SANITY_CHECK_POINTER(num);

	CHECK_POINTER_RETURN_VALUE(num, CONV_BAD_ARGS);
	CHECK_POINTER_RETURN_VALUE(number, CONV_BAD_ARGS);
	CHECK_CONDITION_RETURN_VALUE(!jis_number(num), CONV_BAD_ARGS, "Trying to access %d as a number", num->m_type);

	switch (jnum_deref(num)->m_type) {
		case NUM_FLOAT:
			return jdouble_to_i32 (jnum_deref(num)->value.floating, number) | jnum_deref(num)->m_error;
		case NUM_INT:
			return ji64_to_i32 (jnum_deref(num)->value.integer, number) | jnum_deref(num)->m_error;
		case NUM_RAW:
			assert(jnum_deref(num)->value.raw.m_str != NULL);
			assert(jnum_deref(num)->value.raw.m_len > 0);
			return jstr_to_i32 (&jnum_deref(num)->value.raw, number) | jnum_deref(num)->m_error;
		default:
			PJ_LOG_ERR("internal error - numeric type is unrecognized (%d)", (int)jnum_deref(num)->m_type);
			assert(false);
			return CONV_GENERIC_ERROR;
	}
}

ConversionResultFlags jnumber_get_i64 (jvalue_ref num, int64_t *number)
{
	SANITY_CHECK_POINTER(num);

	CHECK_POINTER_RETURN_VALUE(num, CONV_BAD_ARGS);
	CHECK_POINTER_RETURN_VALUE(number, CONV_BAD_ARGS);
	CHECK_CONDITION_RETURN_VALUE(!jis_number(num), CONV_BAD_ARGS, "Trying to access %d as a number", num->m_type);

	switch (jnum_deref(num)->m_type) {
		case NUM_FLOAT:
			return jdouble_to_i64 (jnum_deref(num)->value.floating, number) | jnum_deref(num)->m_error;
		case NUM_INT:
			*number = jnum_deref(num)->value.integer;
			return jnum_deref(num)->m_error;
		case NUM_RAW:
			assert(jnum_deref(num)->value.raw.m_str != NULL);
			assert(jnum_deref(num)->value.raw.m_len > 0);
			return jstr_to_i64 (&jnum_deref(num)->value.raw, number) | jnum_deref(num)->m_error;
		default:
			PJ_LOG_ERR("internal error - numeric type is unrecognized (%d)", (int)jnum_deref(num)->m_type);
			assert(false);
			return CONV_GENERIC_ERROR;
	}
}

ConversionResultFlags jnumber_get_f64 (jvalue_ref num, double *number)
{
	SANITY_CHECK_POINTER(num);

	CHECK_POINTER_RETURN_VALUE(num, CONV_BAD_ARGS);
	CHECK_POINTER_RETURN_VALUE(number, CONV_BAD_ARGS);
	CHECK_CONDITION_RETURN_VALUE(!jis_number(num), CONV_BAD_ARGS, "Trying to access %d as a number", num->m_type);

	switch (jnum_deref(num)->m_type) {
		case NUM_FLOAT:
			*number = jnum_deref(num)->value.floating;
			return jnum_deref(num)->m_error;
		case NUM_INT:
			return ji64_to_double (jnum_deref(num)->value.integer, number) | jnum_deref(num)->m_error;
		case NUM_RAW:
			assert(jnum_deref(num)->value.raw.m_str != NULL);
			assert(jnum_deref(num)->value.raw.m_len > 0);
			return jstr_to_double (&jnum_deref(num)->value.raw, number) | jnum_deref(num)->m_error;
		default:
			PJ_LOG_ERR("internal error - numeric type is unrecognized (%d)", (int)jnum_deref(num)->m_type);
			assert(false);
			return CONV_GENERIC_ERROR;
	}
}

ConversionResultFlags jnumber_get_raw (jvalue_ref num, raw_buffer *result)
{
	SANITY_CHECK_POINTER(num);

	CHECK_POINTER_RETURN_VALUE(num, CONV_BAD_ARGS);
	CHECK_POINTER_RETURN_VALUE(result, CONV_BAD_ARGS);
	CHECK_CONDITION_RETURN_VALUE(!jis_number(num), CONV_BAD_ARGS, "Trying to access %d as a number", num->m_type);

	switch (jnum_deref(num)->m_type) {
		case NUM_FLOAT:
		case NUM_INT:
			return CONV_NOT_A_RAW_NUM;
		case NUM_RAW:
			assert(jnum_deref(num)->value.raw.m_str != NULL);
			assert(jnum_deref(num)->value.raw.m_len > 0);
			*result = jnum_deref(num)->value.raw;
			return CONV_OK;
		default:
			PJ_LOG_ERR("internal error - numeric type is unrecognized (%d)", (int)jnum_deref(num)->m_type);
			assert(false);
			return CONV_GENERIC_ERROR;
	}
}

/*** JSON Boolean operations ***/

bool jis_boolean (jvalue_ref jval)
{
	SANITY_CHECK_POINTER(jval);
	assert(s_inGdb || jval->m_refCnt > 0);
	assert( jval->m_type != JV_BOOL || jval == &JTRUE.m_value || jval == &JFALSE.m_value );
	return jval->m_type == JV_BOOL;
}

jvalue_ref jboolean_true()
{ return &JTRUE.m_value; }

jvalue_ref jboolean_false()
{ return &JFALSE.m_value; }

jvalue_ref jboolean_create (bool value)
{ return value ? jboolean_true() : jboolean_false(); }

bool jboolean_deref_to_value (jvalue_ref boolean)
{
	bool result;
	assert (jis_null(boolean) || CONV_OK == jboolean_get(boolean, &result));
	jboolean_get (boolean, &result);
	return result;
}

/**
 * Retrieve the native boolean representation of this reference.
 *
 * The following equivalencies are made for the various JSON types & bool:
 * NUMBERS: 0, NaN = false, everything else = true
 * STRINGS: empty = false, non-empty = true
 * NULL: false
 * ARRAY: true
 * OBJECT: true
 * @param val The reference to the JSON value
 * @param value Where to write the boolean value to.
 * @return CONV_OK if val represents a JSON boolean type, otherwise CONV_NOT_A_BOOLEAN.
 */
ConversionResultFlags jboolean_get (jvalue_ref val, bool *value)
{
	SANITY_CHECK_POINTER(val);

	if (value) *value = false;

	CHECK_POINTER_MSG_RETURN_VALUE(val, CONV_NOT_A_BOOLEAN, "Attempting to use a C NULL as a JSON value reference");
	CHECK_POINTER_MSG_RETURN_VALUE(value, (jis_boolean(val) ? CONV_OK : CONV_NOT_A_BOOLEAN), "Non-recommended API use - value is not pointing to a valid boolean");
	assert(val->m_refCnt > 0);
	assert( val->m_type != JV_BOOL || val == &JTRUE.m_value || val == &JFALSE.m_value );

	switch (val->m_type) {
		case JV_BOOL:
			if (value) *value = jboolean_deref(val)->value;
			return CONV_OK;

		case JV_NULL:
			PJ_LOG_INFO("Attempting to convert NULL to boolean");
			if (value) *value = false;
			break;
		case JV_OBJECT:
			PJ_LOG_WARN("Attempting to convert an object to a boolean - always true");
			if (value) *value = true;
			break;
		case JV_ARRAY:
			PJ_LOG_WARN("Attempting to convert an array to a boolean - always true");
			if (value) *value = true;
			break;
		case JV_STR:
			PJ_LOG_WARN("Attempt to convert a string to a boolean - testing if string is empty");
			if (value) *value = jstring_size (val) != 0;
			break;
		case JV_NUM:
		{
			double result;
			ConversionResultFlags conv_result;
			PJ_LOG_WARN("Attempting to convert a number to a boolean - testing if number is 0");
			conv_result = jnumber_get_f64 (val, &result);
			if (value) *value = (conv_result == CONV_OK && result != 0);
			break;
		}
	}

	return CONV_NOT_A_BOOLEAN;
}

bool j_fopen(const char *file, _jbuffer *buf, jerror **err)
{
	CHECK_POINTER_RETURN_VALUE(file, false);

	int fd = open(file, O_RDONLY);
	if (fd == -1) {
		jerror_set_formatted(err, JERROR_TYPE_INVALID_PARAMETERS,
		                     "Can't open file: %s", file);
		return false;
	}

	bool result = j_fopen2(fd, buf, err);

	close(fd);

	return result;
}

bool j_fopen2(int fd, _jbuffer *buf, jerror **err)
{
	struct stat finfo;
	raw_buffer input = { 0 };

	if (fstat(fd, &finfo) != 0) {
		jerror_set_formatted(err, JERROR_TYPE_INVALID_PARAMETERS,
		                     "Can't read file size: %s", strerror(errno));
		return false;
	}
	input.m_len = finfo.st_size;

	input.m_str = (char *)mmap(NULL, input.m_len, PROT_READ, MAP_PRIVATE | MAP_NORESERVE, fd, 0);
	if (input.m_str == NULL || input.m_str == MAP_FAILED) {
		jerror_set_formatted(err, JERROR_TYPE_INVALID_PARAMETERS,
		                     "Can't map file: %s",
		                     strerror(errno));
		return false;
	}
	madvise((void *)input.m_str, input.m_len, MADV_SEQUENTIAL | MADV_WILLNEED);

	buf->buffer = input;
	buf->destructor = _jbuffer_munmap;

	return true;
}
