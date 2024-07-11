// Copyright (c) 2014-2024 LG Electronics, Inc.
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

#include <assert.h>
#include <jobject.h>

#include "jobject_internal.h"
#include "jtraverse.h"

static bool jkeyvalue_traverse(jobject_key_value jref, TraverseCallbacksRef tc, void *context)
{
	if (!tc->jobj_key(context, jref.key))
		return false;
	return jvalue_traverse(jref.value, tc, context);
}

static bool jobject_traverse(jvalue_ref jref, TraverseCallbacksRef tc, void *context)
{
	if (!tc->jobj_start(context, jref))
		return false;

	jobject_iter it;
	jobject_iter_init(&it, jref);
	jobject_key_value key_value;
	while (jobject_iter_next(&it, &key_value))
	{
		if (!jkeyvalue_traverse(key_value, tc, context))
			return false;
	}

	return tc->jobj_end(context, jref);
}

static bool jarray_traverse(jvalue_ref jref, TraverseCallbacksRef tc, void *context)
{
	if (!tc->jarr_start(context, jref))
		return false;

	for (int i = 0; i < jarray_size(jref); i++)
	{
		jvalue_ref element = jarray_get(jref, i);
		if (!jvalue_traverse(element, tc, context))
			return false;
	}

	return tc->jarr_end(context, jref);
}

static bool jnumber_traverse(jvalue_ref jref, TraverseCallbacksRef tc, void *context)
{
	switch (jnum_deref(jref)->m_type)
	{
		case NUM_RAW:
			return tc->jnumber_raw(context, jref);
		case NUM_FLOAT:
			return tc->jnumber_double(context, jref);
		case NUM_INT:
			return tc->jnumber_int(context, jref);
		default:
			return false;
	}
}

bool jvalue_traverse(jvalue_ref jref, TraverseCallbacksRef tc, void *context)
{
	switch (jref->m_type)
	{
	case JV_NULL   : return tc->jnull(context, jref);
	case JV_OBJECT : return jobject_traverse(jref, tc, context);
	case JV_ARRAY  : return jarray_traverse(jref, tc, context);
	case JV_NUM    : return jnumber_traverse(jref, tc, context);
	case JV_STR    : return tc->jstring(context, jref);
	case JV_BOOL   : return tc->jbool(context, jref);
	}

	return false;
}
