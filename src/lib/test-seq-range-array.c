/* Copyright (c) 2007-2014 Dovecot authors, see the included COPYING file */

#include "test-lib.h"
#include "array.h"
#include "seq-range-array.h"

#include <stdlib.h>

static void
boundaries_permute(uint32_t *input, unsigned int i, unsigned int count)
{
	ARRAY_TYPE(seq_range) range;
	const struct seq_range *seqs;
	unsigned int seqs_count;
	uint32_t tmp;
	unsigned int j;

	if (i+1 < count) {
		for (j = i; j < count; j++) {
			tmp = input[i]; input[i] = input[j]; input[j] = tmp;
			boundaries_permute(input, i+1, count);
			tmp = input[i]; input[i] = input[j]; input[j] = tmp;
		}
		return;
	}
	t_array_init(&range, 4);
	for (i = 0; i < count; i++)
		seq_range_array_add(&range, input[i]);
	seqs = array_get(&range, &seqs_count);
	test_assert(seqs_count == 2);
	test_assert(seqs[0].seq1 == 0);
	test_assert(seqs[0].seq2 == 1);
	test_assert(seqs[1].seq1 == (uint32_t)-2);
	test_assert(seqs[1].seq2 == (uint32_t)-1);
}

static void test_seq_range_array_add_boundaries(void)
{
	static uint32_t input[] = { 0, 1, (uint32_t)-2, (uint32_t)-1 };

	boundaries_permute(input, 0, N_ELEMENTS(input));
}

static void test_seq_range_array_add_merge(void)
{
	ARRAY_TYPE(seq_range) range;

	test_begin("seq_range_array_add() merging");
	t_array_init(&range, 8);
	seq_range_array_add(&range, 4);
	seq_range_array_add(&range, 1);
	seq_range_array_add(&range, 2);
	test_assert(array_count(&range) == 2);
	test_end();
}

static void test_seq_range_array_remove_nth(void)
{
	ARRAY_TYPE(seq_range) range;
	const struct seq_range *r;

	test_begin("seq_range_array_remove_nth()");
	t_array_init(&range, 8);
	seq_range_array_add_range(&range, 1, 5);
	seq_range_array_add(&range, 7);
	seq_range_array_add_range(&range, 10,20);
	test_assert(array_count(&range) == 3);

	seq_range_array_remove_nth(&range, 0, 2);
	r = array_idx(&range, 0); test_assert(r->seq1 == 3 && r->seq2 == 5);

	seq_range_array_remove_nth(&range, 1, 4);
	r = array_idx(&range, 0); test_assert(r->seq1 == 3 && r->seq2 == 3);
	r = array_idx(&range, 1); test_assert(r->seq1 == 11 && r->seq2 == 20);

	seq_range_array_remove_nth(&range, 5, (uint32_t)-1);
	r = array_idx(&range, 1); test_assert(r->seq1 == 11 && r->seq2 == 14);

	test_assert(array_count(&range) == 2);
	test_end();
}

static void test_seq_range_array_random(void)
{
#define SEQ_RANGE_TEST_BUFSIZE 20
#define SEQ_RANGE_TEST_COUNT 10000
	unsigned char shadowbuf[SEQ_RANGE_TEST_BUFSIZE];
	ARRAY_TYPE(seq_range) range;
	const struct seq_range *seqs;
	uint32_t seq1, seq2;
	unsigned int i, j, ret, ret2, count;
	int test = -1;

	ret = ret2 = 0;
	i_array_init(&range, 1);
	memset(shadowbuf, 0, sizeof(shadowbuf));
	for (i = 0; i < SEQ_RANGE_TEST_COUNT; i++) {
		seq1 = rand() % SEQ_RANGE_TEST_BUFSIZE;
		seq2 = seq1 + rand() % (SEQ_RANGE_TEST_BUFSIZE - seq1);
		test = rand() % 4;
		switch (test) {
		case 0:
			seq_range_array_add(&range, seq1);
			shadowbuf[seq1] = 1;
			break;
		case 1:
			seq_range_array_add_range(&range, seq1, seq2);
			memset(shadowbuf + seq1, 1, seq2 - seq1 + 1);
			break;
		case 2:
			ret = seq_range_array_remove(&range, seq1) ? 1 : 0;
			ret2 = shadowbuf[seq1] != 0 ? 1 : 0;
			shadowbuf[seq1] = 0;
			break;
		case 3:
			ret = seq_range_array_remove_range(&range, seq1, seq2);
			for (ret2 = 0; seq1 <= seq2; seq1++) {
				if (shadowbuf[seq1] != 0) {
					ret2++;
					shadowbuf[seq1] = 0;
				}
			}
			break;
		}
		if (ret != ret2)
			break;

		seqs = array_get(&range, &count);
		for (j = 0, seq1 = 0; j < count; j++) {
			if (j > 0 && seqs[j-1].seq2+1 >= seqs[j].seq1)
				goto fail;
			for (; seq1 < seqs[j].seq1; seq1++) {
				if (shadowbuf[seq1] != 0)
					goto fail;
			}
			for (; seq1 <= seqs[j].seq2; seq1++) {
				if (shadowbuf[seq1] == 0)
					goto fail;
			}
		}
		i_assert(seq1 <= SEQ_RANGE_TEST_BUFSIZE);
		for (; seq1 < SEQ_RANGE_TEST_BUFSIZE; seq1++) {
			if (shadowbuf[seq1] != 0)
				goto fail;
		}
	}
fail:
	if (i == SEQ_RANGE_TEST_COUNT)
		test_out("seq_range_array random", TRUE);
	else {
		test_out_reason("seq_range_array random", FALSE,
			t_strdup_printf("round %u test %d failed", i, test));
	}
	array_free(&range);
}

static void test_seq_range_array_invert(void)
{
	static const unsigned int input_min = 1, input_max = 5;
	static const unsigned int input[] = {
		1, 2, 3, 4, 5, UINT_MAX,
		2, 3, 4, UINT_MAX,
		1, 2, 4, 5, UINT_MAX,
		1, 3, 5, UINT_MAX,
		1, UINT_MAX,
		5, UINT_MAX,
		UINT_MAX
	};
	ARRAY_TYPE(seq_range) range = ARRAY_INIT;
	unsigned int i, j, seq, start, num;
	bool old_exists, success;

	for (i = num = 0; input[i] != UINT_MAX; num++, i++) {
		success = TRUE;
		start = i;
		for (; input[i] != UINT_MAX; i++) {
			seq_range_array_add_with_init(&range, 32, input[i]);
			for (j = start; j < i; j++) {
				if (!seq_range_exists(&range, input[j]))
					success = FALSE;
			}
		}

		seq_range_array_invert(&range, input_min, input_max);
		for (seq = input_min; seq <= input_max; seq++) {
			for (j = start; input[j] != UINT_MAX; j++) {
				if (input[j] == seq)
					break;
			}
			old_exists = input[j] != UINT_MAX;
			if (seq_range_exists(&range, seq) == old_exists)
				success = FALSE;
		}
		test_out(t_strdup_printf("seq_range_array_invert(%u)", num),
			 success);
		array_free(&range);
	}
}

static void test_seq_range_create(ARRAY_TYPE(seq_range) *array, uint8_t byte)
{
	unsigned int i;

	array_clear(array);
	for (i = 0; i < 8; i++) {
		if ((byte & (1 << i)) != 0)
			seq_range_array_add(array, i + 1);
	}
}

static void test_seq_range_array_have_common(void)
{
	ARRAY_TYPE(seq_range) arr1, arr2;
	unsigned int i, j;
	bool ret1, ret2, success = TRUE;

	t_array_init(&arr1, 8);
	t_array_init(&arr2, 8);
	for (i = 0; i < 256; i++) {
		test_seq_range_create(&arr1, i);
		for (j = 0; j < 256; j++) {
			test_seq_range_create(&arr2, j);
			ret1 = seq_range_array_have_common(&arr1, &arr2);
			ret2 = (i & j) != 0;
			if (ret1 != ret2)
				success = FALSE;
		}
	}
	test_out("seq_range_array_have_common()", success);
}

void test_seq_range_array(void)
{
	test_seq_range_array_add_boundaries();
	test_seq_range_array_add_merge();
	test_seq_range_array_remove_nth();
	test_seq_range_array_invert();
	test_seq_range_array_have_common();
	test_seq_range_array_random();
}
