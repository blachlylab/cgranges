#include <stdio.h>
#include "cgranges.h"
#include "khash.h"
#include "ksort.h"

/*********************
 * Convenient macros *
 *********************/

#ifndef kroundup32
#define kroundup32(x) (--(x), (x)|=(x)>>1, (x)|=(x)>>2, (x)|=(x)>>4, (x)|=(x)>>8, (x)|=(x)>>16, ++(x))
#endif

#define CALLOC(type, len) ((type*)calloc((len), sizeof(type)))
#define REALLOC(ptr, len) ((ptr) = (__typeof__(ptr))realloc((ptr), (len) * sizeof(*(ptr))))

#define EXPAND(a, m) do { \
		(m) = (m)? (m) + ((m)>>1) : 16; \
		REALLOC((a), (m)); \
	} while (0)

/********************
 * Basic operations *
 ********************/

#define cr_intv_key(r) ((r).x)
KRADIX_SORT_INIT(cr_intv, cr_intv_t, cr_intv_key, 8)

KHASH_MAP_INIT_STR(str, int32_t)
typedef khash_t(str) strhash_t;

cgranges_t *cr_init(void)
{
	cgranges_t *cr;
	cr = CALLOC(cgranges_t, 1);
	cr->hc = kh_init(str);
	return cr;
}

void cr_destroy(cgranges_t *cr)
{
	int32_t i;
	if (cr == 0) return;
	for (i = 0; i < cr->n_ctg; ++i)
		free(cr->ctg[i].name);
	free(cr->ctg);
	kh_destroy(str, (strhash_t*)cr->hc);
	free(cr);
}

int32_t cr_add_ctg(cgranges_t *cr, const char *ctg, int32_t len)
{
	int absent;
	khint_t k;
	strhash_t *h = (strhash_t*)cr->hc;
	k = kh_put(str, h, ctg, &absent);
	if (absent) {
		cr_ctg_t *p;
		if (cr->n_ctg == cr->m_ctg)
			EXPAND(cr->ctg, cr->m_ctg);
		kh_val(h, k) = cr->n_ctg;
		p = &cr->ctg[cr->n_ctg++];
		p->name = strdup(ctg);
		kh_key(h, k) = p->name;
		p->len = len;
		p->n = 0, p->off = -1;
	}
	if (len > cr->ctg[kh_val(h, k)].len)
		cr->ctg[kh_val(h, k)].len = len;
	return kh_val(h, k);
}

int32_t cr_get_ctg(const cgranges_t *cr, const char *ctg)
{
	khint_t k;
	strhash_t *h = (strhash_t*)cr->hc;
	k = kh_get(str, h, ctg);
	return k == kh_end(h)? -1 : kh_val(h, k);
}

cr_intv_t *cr_add_intv(cgranges_t *cr, const char *ctg, int32_t st, int32_t en, int32_t label_int)
{
	cr_intv_t *p;
	int32_t k;
	if (st > en) return 0;
	k = cr_add_ctg(cr, ctg, 0);
	if (cr->n_r == cr->m_r)
		EXPAND(cr->r, cr->m_r);
	p = &cr->r[cr->n_r++];
	p->x = (uint64_t)k << 32 | st;
	p->y = en;
	p->label = label_int;
	if (cr->ctg[k].len < en)
		cr->ctg[k].len = en;
	return p;
}

void cr_sort(cgranges_t *cr)
{
	if (cr->n_ctg == 0 || cr->n_r == 0) return;
	radix_sort_cr_intv(cr->r, cr->r + cr->n_r);
}

int32_t cr_is_sorted(const cgranges_t *cr)
{
	uint64_t i;
	for (i = 1; i < cr->n_r; ++i)
		if (cr->r[i-1].x > cr->r[i].x)
			break;
	return (i == cr->n_r);
}

/************
 * Indexing *
 ************/

void cr_index_prepare(cgranges_t *cr)
{
	int64_t i, st;
	if (!cr_is_sorted(cr)) cr_sort(cr);
	for (st = 0, i = 1; i <= cr->n_r; ++i) {
		if (i == cr->n_r || cr->r[i].x>>32 != cr->r[st].x>>32) {
			int32_t ctg = cr->r[st].x>>32;
			cr->ctg[ctg].off = st;
			cr->ctg[ctg].n = i - st;
			st = i;
		}
	}
	for (i = 0; i < cr->n_r; ++i) {
		cr_intv_t *r = &cr->r[i];
		r->x = r->x<<32 | r->y;
		r->y = 0;
	}
}

int32_t cr_index1(cr_intv_t *a, int64_t n)
{
	int64_t i, last_i;
	int32_t last, k;
	if (n <= 0) return -1;
	for (i = 0; i < n; i += 2) last_i = i, last = a[i].y = (int32_t)a[i].x;
	for (k = 1; 1LL<<k <= n; ++k) {
		int64_t x = 1LL<<(k-1), i0 = (x<<1) - 1, step = x<<2;
		for (i = i0; i < n; i += step) {
			int32_t el = a[i - x].y;
			int32_t er = i + x < n? a[i + x].y : last;
			int32_t e = (int32_t)a[i].x;
			e = e > el? e : el;
			e = e > er? e : er;
			a[i].y = e;
		}
		last_i = last_i>>k&1? last_i - x : last_i + x;
		if (last_i < n && a[last_i].y > last)
			last = a[last_i].y;
	}
	return k - 1;
}

void cr_index(cgranges_t *cr)
{
	int32_t i;
	cr_index_prepare(cr);
	for (i = 0; i < cr->n_ctg; ++i)
		cr->ctg[i].root_k = cr_index1(&cr->r[cr->ctg[i].off], cr->ctg[i].n);
}

/*********
 * Query *
 *********/

typedef struct {
	int64_t x;
	int32_t k, w;
} istack_t;

int64_t cr_overlap_int(const cgranges_t *cr, int32_t ctg_id, int32_t st, int32_t en, int64_t **b_, int64_t *m_b_)
{
	int32_t t = 0;
	const cr_ctg_t *c;
	const cr_intv_t *r;
	int64_t *b = *b_, m_b = *m_b_, n = 0;
	istack_t stack[64], *p;

	if (ctg_id < 0 || ctg_id >= cr->n_ctg) return 0;
	c = &cr->ctg[ctg_id];
	r = &cr->r[c->off];
	p = &stack[t++];
	p->k = c->root_k, p->x = (1LL<<p->k) - 1, p->w = 0; // push the root into the stack
	while (t) { // stack is not empyt
		istack_t z = stack[--t];
		if (z.k <= 2) { // the subtree is no larger than (1<<(z.k+1))-1; do a linear scan
			int64_t i, i0 = z.x >> z.k << z.k, i1 = i0 + (1LL<<(z.k+1)) - 1;
			if (i1 >= c->n) i1 = c->n;
			for (i = i0; i < i1; ++i)
				if (cr_st(&r[i]) < en && st < cr_en(&r[i])) {
					if (n == m_b) EXPAND(b, m_b);
					b[n++] = c->off + i;
				}
		} else if (z.w == 0) { // if left child not processed
			int64_t y = z.x - (1LL<<(z.k-1));
			p = &stack[t++];
			p->k = z.k, p->x = z.x, p->w = 1;
			if (y >= c->n || r[y].y > st) {
				p = &stack[t++];
				p->k = z.k - 1, p->x = y, p->w = 0; // push the left child to the stack
			}
		} else if (z.x < c->n && cr_st(&r[z.x]) < en) {
			if (st < cr_en(&r[z.x])) { // then z.x overlaps the query; write to the output array
				if (n == m_b) EXPAND(b, m_b);
				b[n++] = c->off + z.x;
			}
			p = &stack[t++];
			p->k = z.k - 1, p->x = z.x + (1LL<<(z.k-1)), p->w = 0; // push the right child
		}
	}
	*b_ = b, *m_b_ = m_b;
	return n;
}

int64_t cr_overlap(const cgranges_t *cr, const char *ctg, int32_t st, int32_t en, int64_t **b_, int64_t *m_b_)
{
	return cr_overlap_int(cr, cr_get_ctg(cr, ctg), st, en, b_, m_b_);
}