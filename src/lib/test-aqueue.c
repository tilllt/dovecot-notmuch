/* Copyright (c) 2007-2014 Dovecot authors, see the included COPYING file */

#include "test-lib.h"
#include "array.h"
#include "aqueue.h"

static bool aqueue_is_ok(struct aqueue *aqueue, unsigned int deleted_n)
{
	const unsigned int *p;
	unsigned int n, i, count;

	count = aqueue_count(aqueue);
	for (i = 0, n = 1; i < count; i++, n++) {
		p = array_idx_i(aqueue->arr, aqueue_idx(aqueue, i));
		if (i == deleted_n)
			n++;
		if (*p != n)
			return FALSE;
	}
	return TRUE;
}

static const unsigned int aqueue_input[] = { 1, 2, 3, 4, 5, 6 };
static const char *test_aqueue2(unsigned int initial_size)
{
	ARRAY(unsigned int) aqueue_array;
	unsigned int i, j, k;

	for (i = 0; i < N_ELEMENTS(aqueue_input); i++) {
		for (k = 0; k < N_ELEMENTS(aqueue_input); k++) {
			struct aqueue *aqueue;

			t_array_init(&aqueue_array, initial_size);
			aqueue = aqueue_init(&aqueue_array.arr);
			aqueue->head = aqueue->tail = initial_size - 1;
			for (j = 0; j < k; j++) {
				aqueue_append(aqueue, &aqueue_input[j]);
				if (aqueue_count(aqueue) != j + 1) {
					return t_strdup_printf("Wrong count after append %u vs %u)",
							       aqueue_count(aqueue), j + 1);
				}
				if (!aqueue_is_ok(aqueue, UINT_MAX))
					return "Invalid data after append";
			}

			if (k != 0 && i < k) {
				aqueue_delete(aqueue, i);
				if (aqueue_count(aqueue) != k - 1)
					return "Wrong count after delete";
				if (!aqueue_is_ok(aqueue, i))
					return "Invalid data after delete";
			}
			aqueue_clear(aqueue);
			if (aqueue_count(aqueue) != 0)
				return "aqueue_clear() broken";
			aqueue_deinit(&aqueue);
		}
	}
	return NULL;
}

void test_aqueue(void)
{
	unsigned int i;
	const char *reason = NULL;

	for (i = 1; i <= N_ELEMENTS(aqueue_input) + 1 && reason == NULL; i++) {
		T_BEGIN {
			reason = test_aqueue2(i);
		} T_END;
	}
	test_out_reason("aqueue", reason == NULL, reason);
}
