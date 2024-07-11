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

#pragma once

#include "validator.h"
#include <stddef.h>
#include <glib.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _ArrayItems ArrayItems;

/**
 * Array validator for {"type": "array"}
 */
typedef struct _ArrayValidator
{
	/** @brief Base class is Validator */
	Validator base;

	/** @brief Reference count */
	unsigned ref_count;

	/** @brief Items of the array from "items": [...]. */
	ArrayItems *items;

	/** @brief Additional items of the array from "additionalItems". */
	Validator *additional_items;

	/** @brief Maximal count of items in the array. */
	int max_items;

	/** @brief Minimal count of items in the array. */
	int min_items;

	/** @brief Is array can't contain duplicate items.
	 *
	 * NOTE: Featule will work only if has_array_duplicates() is provided
	 */
	bool unique_items;

	/** @brief Default value attached to this validator */
	jvalue_ref def_value;
} ArrayValidator;

//_Static_assert(offsetof(ArrayValidator, base) == 0, "");

/** @brief Generic array validator. Checks only value type. */
Validator* array_validator_instance();

/** @brief Constructor: allocate and initialize an array validator. */
ArrayValidator* array_validator_new();

/** @brief Destroy the array validator. */
void array_validator_release(ArrayValidator *v);

/** @brief Set maximal item count in the array. */
void array_validator_set_max_items(ArrayValidator *a, size_t max);

/** @brief Set minimal item count in the array. */
void array_validator_set_min_items(ArrayValidator *a, size_t min);

#ifdef __cplusplus
}
#endif
