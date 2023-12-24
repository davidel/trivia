/*    Copyright 2023 Davide Libenzi
 * 
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 * 
 *        http://www.apache.org/licenses/LICENSE-2.0
 * 
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 * 
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/*
 * Misc cut&paste from kernel code
 */
#define BITS_X_LONG	(8 * sizeof(long))

#define likely(x)	__builtin_expect(!!(x), 1)
#define unlikely(e)	__builtin_expect(!!(e), 0)

struct list_head {
	struct list_head *next, *prev;
};

#define LIST_HEAD_INIT(name) { &(name), &(name) }

#define LIST_HEAD(name) \
	struct list_head name = LIST_HEAD_INIT(name)

#define offsetof(type, member) ((long) &((type *) 0)->member)
#define container_of(ptr, type, member) ({			\
        const typeof( ((type *)0)->member ) *__mptr = (ptr);	\
        (type *)( (char *)__mptr - offsetof(type,member) );})

#define list_entry(ptr, type, member) \
	container_of(ptr, type, member)

static inline void INIT_LIST_HEAD(struct list_head *list) {
	list->next = list;
	list->prev = list;
}
static inline void __list_add(struct list_head *new,
			      struct list_head *prev,
			      struct list_head *next) {
	next->prev = new;
	new->next = next;
	new->prev = prev;
	prev->next = new;
}
static inline void list_add_tail(struct list_head *new, struct list_head *head) {
	__list_add(new, head->prev, head);
}
static inline void __list_del(struct list_head * prev, struct list_head * next) {
	next->prev = prev;
	prev->next = next;
}
static inline void list_del(struct list_head *entry) {
	__list_del(entry->prev, entry->next);
	entry->next = NULL;
	entry->prev = NULL;
}
static inline int list_empty(const struct list_head *head) {
	return head->next == head;
}
/*
 * Too much GCC versioning assembly there. Using dumb versions.
 */
static inline void __set_bit(unsigned int nr, volatile unsigned long *addr) {
	addr[nr / BITS_X_LONG] |= 1UL << (nr % BITS_X_LONG);
}
static inline void __clear_bit(unsigned int nr, volatile unsigned long *addr) {
	addr[nr / BITS_X_LONG] &= ~(1UL << (nr % BITS_X_LONG));
}
#if defined(__x86_64__)
static inline unsigned long __ffs(unsigned long word) {
	__asm__("bsfq %1,%0"
		:"=r" (word)
		:"rm" (word));
	return word;
}
#define rdtscll(val) do { \
	unsigned int __a, __d; \
	asm volatile("rdtsc" : "=a" (__a), "=d" (__d)); \
	(val) = ((unsigned long) __a) | (((unsigned long) __d) << 32); \
} while (0)
#elif defined(__i386__)
static inline unsigned long __ffs(unsigned long word) {
	__asm__("bsfl %1,%0"
		:"=r" (word)
		:"rm" (word));
	return word;
}
#define rdtscll(val) \
	__asm__ __volatile__("rdtsc" : "=A" (val))
#else
#error "it's 2007 and you're using which CPU?"
#endif



/*
 * RB-Tree code ...
 */
struct rb_node
{
	unsigned long  rb_parent_color;
#define	RB_RED		0
#define	RB_BLACK	1
	struct rb_node *rb_right;
	struct rb_node *rb_left;
} __attribute__((aligned(sizeof(long))));
/* The alignment might seem pointless, but allegedly CRIS needs it */

struct rb_root
{
	struct rb_node *rb_node;
};


#define rb_parent(r)   ((struct rb_node *)((r)->rb_parent_color & ~3))
#define rb_color(r)   ((r)->rb_parent_color & 1)
#define rb_is_red(r)   (!rb_color(r))
#define rb_is_black(r) rb_color(r)
#define rb_set_red(r)  do { (r)->rb_parent_color &= ~1; } while (0)
#define rb_set_black(r)  do { (r)->rb_parent_color |= 1; } while (0)

static inline void rb_set_parent(struct rb_node *rb, struct rb_node *p)
{
	rb->rb_parent_color = (rb->rb_parent_color & 3) | (unsigned long)p;
}
static inline void rb_set_color(struct rb_node *rb, int color)
{
	rb->rb_parent_color = (rb->rb_parent_color & ~1) | color;
}

#define RB_ROOT	(struct rb_root) { NULL, }
#define	rb_entry(ptr, type, member) container_of(ptr, type, member)

#define RB_EMPTY_ROOT(root)	((root)->rb_node == NULL)
#define RB_EMPTY_NODE(node)	(rb_parent(node) == node)
#define RB_CLEAR_NODE(node)	(rb_set_parent(node, node))

extern void rb_insert_color(struct rb_node *, struct rb_root *);
extern void rb_erase(struct rb_node *, struct rb_root *);

/* Find logical next and previous nodes in a tree */
extern struct rb_node *rb_next(struct rb_node *);
extern struct rb_node *rb_prev(struct rb_node *);
extern struct rb_node *rb_first(struct rb_root *);
extern struct rb_node *rb_last(struct rb_root *);

/* Fast replacement of a single node without remove/rebalance/add/rebalance */
extern void rb_replace_node(struct rb_node *victim, struct rb_node *new,
			    struct rb_root *root);

static inline void rb_link_node(struct rb_node * node, struct rb_node * parent,
				struct rb_node ** rb_link)
{
	node->rb_parent_color = (unsigned long )parent;
	node->rb_left = node->rb_right = NULL;

	*rb_link = node;
}

static void __rb_rotate_left(struct rb_node *node, struct rb_root *root)
{
	struct rb_node *right = node->rb_right;
	struct rb_node *parent = rb_parent(node);

	if ((node->rb_right = right->rb_left))
		rb_set_parent(right->rb_left, node);
	right->rb_left = node;

	rb_set_parent(right, parent);

	if (parent)
	{
		if (node == parent->rb_left)
			parent->rb_left = right;
		else
			parent->rb_right = right;
	}
	else
		root->rb_node = right;
	rb_set_parent(node, right);
}

static void __rb_rotate_right(struct rb_node *node, struct rb_root *root)
{
	struct rb_node *left = node->rb_left;
	struct rb_node *parent = rb_parent(node);

	if ((node->rb_left = left->rb_right))
		rb_set_parent(left->rb_right, node);
	left->rb_right = node;

	rb_set_parent(left, parent);

	if (parent)
	{
		if (node == parent->rb_right)
			parent->rb_right = left;
		else
			parent->rb_left = left;
	}
	else
		root->rb_node = left;
	rb_set_parent(node, left);
}

void rb_insert_color(struct rb_node *node, struct rb_root *root)
{
	struct rb_node *parent, *gparent;

	while ((parent = rb_parent(node)) && rb_is_red(parent))
	{
		gparent = rb_parent(parent);

		if (parent == gparent->rb_left)
		{
			{
				register struct rb_node *uncle = gparent->rb_right;
				if (uncle && rb_is_red(uncle))
				{
					rb_set_black(uncle);
					rb_set_black(parent);
					rb_set_red(gparent);
					node = gparent;
					continue;
				}
			}

			if (parent->rb_right == node)
			{
				register struct rb_node *tmp;
				__rb_rotate_left(parent, root);
				tmp = parent;
				parent = node;
				node = tmp;
			}

			rb_set_black(parent);
			rb_set_red(gparent);
			__rb_rotate_right(gparent, root);
		} else {
			{
				register struct rb_node *uncle = gparent->rb_left;
				if (uncle && rb_is_red(uncle))
				{
					rb_set_black(uncle);
					rb_set_black(parent);
					rb_set_red(gparent);
					node = gparent;
					continue;
				}
			}

			if (parent->rb_left == node)
			{
				register struct rb_node *tmp;
				__rb_rotate_right(parent, root);
				tmp = parent;
				parent = node;
				node = tmp;
			}

			rb_set_black(parent);
			rb_set_red(gparent);
			__rb_rotate_left(gparent, root);
		}
	}

	rb_set_black(root->rb_node);
}
static void __rb_erase_color(struct rb_node *node, struct rb_node *parent,
			     struct rb_root *root)
{
	struct rb_node *other;

	while ((!node || rb_is_black(node)) && node != root->rb_node)
	{
		if (parent->rb_left == node)
		{
			other = parent->rb_right;
			if (rb_is_red(other))
			{
				rb_set_black(other);
				rb_set_red(parent);
				__rb_rotate_left(parent, root);
				other = parent->rb_right;
			}
			if ((!other->rb_left || rb_is_black(other->rb_left)) &&
			    (!other->rb_right || rb_is_black(other->rb_right)))
			{
				rb_set_red(other);
				node = parent;
				parent = rb_parent(node);
			}
			else
			{
				if (!other->rb_right || rb_is_black(other->rb_right))
				{
					struct rb_node *o_left;
					if ((o_left = other->rb_left))
						rb_set_black(o_left);
					rb_set_red(other);
					__rb_rotate_right(other, root);
					other = parent->rb_right;
				}
				rb_set_color(other, rb_color(parent));
				rb_set_black(parent);
				if (other->rb_right)
					rb_set_black(other->rb_right);
				__rb_rotate_left(parent, root);
				node = root->rb_node;
				break;
			}
		}
		else
		{
			other = parent->rb_left;
			if (rb_is_red(other))
			{
				rb_set_black(other);
				rb_set_red(parent);
				__rb_rotate_right(parent, root);
				other = parent->rb_left;
			}
			if ((!other->rb_left || rb_is_black(other->rb_left)) &&
			    (!other->rb_right || rb_is_black(other->rb_right)))
			{
				rb_set_red(other);
				node = parent;
				parent = rb_parent(node);
			}
			else
			{
				if (!other->rb_left || rb_is_black(other->rb_left))
				{
					register struct rb_node *o_right;
					if ((o_right = other->rb_right))
						rb_set_black(o_right);
					rb_set_red(other);
					__rb_rotate_left(other, root);
					other = parent->rb_left;
				}
				rb_set_color(other, rb_color(parent));
				rb_set_black(parent);
				if (other->rb_left)
					rb_set_black(other->rb_left);
				__rb_rotate_right(parent, root);
				node = root->rb_node;
				break;
			}
		}
	}
	if (node)
		rb_set_black(node);
}

void rb_erase(struct rb_node *node, struct rb_root *root)
{
	struct rb_node *child, *parent;
	int color;

	if (!node->rb_left)
		child = node->rb_right;
	else if (!node->rb_right)
		child = node->rb_left;
	else
	{
		struct rb_node *old = node, *left;

		node = node->rb_right;
		while ((left = node->rb_left) != NULL)
			node = left;
		child = node->rb_right;
		parent = rb_parent(node);
		color = rb_color(node);

		if (child)
			rb_set_parent(child, parent);
		if (parent == old) {
			parent->rb_right = child;
			parent = node;
		} else
			parent->rb_left = child;

		node->rb_parent_color = old->rb_parent_color;
		node->rb_right = old->rb_right;
		node->rb_left = old->rb_left;

		if (rb_parent(old))
		{
			if (rb_parent(old)->rb_left == old)
				rb_parent(old)->rb_left = node;
			else
				rb_parent(old)->rb_right = node;
		} else
			root->rb_node = node;

		rb_set_parent(old->rb_left, node);
		if (old->rb_right)
			rb_set_parent(old->rb_right, node);
		goto color;
	}

	parent = rb_parent(node);
	color = rb_color(node);

	if (child)
		rb_set_parent(child, parent);
	if (parent)
	{
		if (parent->rb_left == node)
			parent->rb_left = child;
		else
			parent->rb_right = child;
	}
	else
		root->rb_node = child;

	color:
	if (color == RB_BLACK)
		__rb_erase_color(child, parent, root);
}
/*
 * This function returns the first node (in sort order) of the tree.
 */
struct rb_node *rb_first(struct rb_root *root)
{
	struct rb_node	*n;

	n = root->rb_node;
	if (!n)
		return NULL;
	while (n->rb_left)
		n = n->rb_left;
	return n;
}
struct rb_node *rb_last(struct rb_root *root)
{
	struct rb_node	*n;

	n = root->rb_node;
	if (!n)
		return NULL;
	while (n->rb_right)
		n = n->rb_right;
	return n;
}
struct rb_node *rb_next(struct rb_node *node)
{
	struct rb_node *parent;

	if (rb_parent(node) == node)
		return NULL;

	/* If we have a right-hand child, go down and then left as far
	 as we can. */
	if (node->rb_right) {
		node = node->rb_right;
		while (node->rb_left)
			node=node->rb_left;
		return node;
	}

	/* No right-hand children.  Everything down and left is
	 smaller than us, so any 'next' node must be in the general
	 direction of our parent. Go up the tree; any time the
	 ancestor is a right-hand child of its parent, keep going
	 up. First time it's a left-hand child of its parent, said
	 parent is our 'next' node. */
	while ((parent = rb_parent(node)) && node == parent->rb_right)
		node = parent;

	return parent;
}
struct rb_node *rb_prev(struct rb_node *node)
{
	struct rb_node *parent;

	if (rb_parent(node) == node)
		return NULL;

	/* If we have a left-hand child, go down and then right as far
	 as we can. */
	if (node->rb_left) {
		node = node->rb_left;
		while (node->rb_right)
			node=node->rb_right;
		return node;
	}

	/* No left-hand children. Go up till we find an ancestor which
	 is a right-hand child of its parent */
	while ((parent = rb_parent(node)) && node == parent->rb_left)
		node = parent;

	return parent;
}
void rb_replace_node(struct rb_node *victim, struct rb_node *new,
		     struct rb_root *root)
{
	struct rb_node *parent = rb_parent(victim);

	/* Set the surrounding nodes to point to the replacement */
	if (parent) {
		if (victim == parent->rb_left)
			parent->rb_left = new;
		else
			parent->rb_right = new;
	} else {
		root->rb_node = new;
	}
	if (victim->rb_left)
		rb_set_parent(victim->rb_left, new);
	if (victim->rb_right)
		rb_set_parent(victim->rb_right, new);

	/* Copy the pointers/colour from the victim to the replacement */
	*new = *victim;
}


typedef unsigned long long nstime_t;


/*
 *  CFS code ...
 */
struct cfs_task {
	struct rb_node run_node;
	nstime_t t;
	/* ... */
};

struct cfs_rq {
	struct rb_root tasks_timeline;
	struct rb_node *rb_leftmost;
};

void cfs_rqinit(struct cfs_rq *rq) {
	rq->rb_leftmost = NULL;
	rq->tasks_timeline = RB_ROOT;
}
void cfs_queue(struct cfs_task *tsk, struct cfs_rq *rq, nstime_t t) {
	struct rb_node **link = &rq->tasks_timeline.rb_node;
	struct rb_node *parent = NULL;
	struct cfs_task *entry;
	int leftmost = 1;

	tsk->t = t;
	/*
	 * Find the right place in the rbtree:
	 */
	while (*link) {
		parent = *link;
		entry = rb_entry(parent, struct cfs_task, run_node);
		/*
		 *  * We dont care about collisions. Nodes with
		 *  * the same key stay together.
		 *  */
		if (t < entry->t) {
			link = &(*link)->rb_left;
		} else {
			link = &(*link)->rb_right;
			leftmost = 0;
		}
	}

	/*
	 * Maintain a cache of leftmost tree entries (it is frequently used)
	 */
	if (leftmost)
		rq->rb_leftmost = &tsk->run_node;
	rb_link_node(&tsk->run_node, parent, link);
	rb_insert_color(&tsk->run_node, &rq->tasks_timeline);
}
static void __dequeue_task_fair(struct cfs_rq *rq, struct cfs_task *p) {
	if (rq->rb_leftmost == &p->run_node)
		rq->rb_leftmost = NULL;
	rb_erase(&p->run_node, &rq->tasks_timeline);
}
static inline struct rb_node * first_fair(struct cfs_rq *rq) {
	if (rq->rb_leftmost)
		return rq->rb_leftmost;
	return rb_first(&rq->tasks_timeline);

}
struct cfs_task *cfs_dequeue(struct cfs_rq *rq) {
	struct rb_node *first = first_fair(rq);
	struct cfs_task *next;

	next = rb_entry(first, struct cfs_task, run_node);
	if (likely(next))
		__dequeue_task_fair(rq, next);

	return next;
}



/*
 * Timed ring code
 */
#define MAX_RQ		(1 << 8)
#define RQ_MASK		(MAX_RQ - 1)
#define MAP_LONGS	(MAX_RQ / BITS_X_LONG)
#define NS_SLOT		(5 * 1000000UL)

struct tr_task {
	struct list_head lnk;
	nstime_t t;
	/* ... */
};

struct tr_rq {
	unsigned int ibase;
	nstime_t tbase;
	struct list_head tsks[MAX_RQ];
	unsigned long map[MAP_LONGS];
};


void tr_rqinit(struct tr_rq *rq) {
	int i;

	rq->ibase = 0;
	rq->tbase = 0;
	for (i = 0; i < MAX_RQ; i++)
		INIT_LIST_HEAD(&rq->tsks[i]);
	memset(rq->map, 0, sizeof(rq->map));
}

static int rel_ffs(struct tr_rq *rq) {
	unsigned int i, b, n;
	unsigned long mask;

	i = rq->ibase / BITS_X_LONG;
	b = rq->ibase % BITS_X_LONG;
	mask = ~((1UL << b) - 1);
	for (n = MAP_LONGS; n; n--) {
		unsigned long v = rq->map[i] & mask;

		if (likely(v))
			return i * BITS_X_LONG + __ffs(v);
		i = (i + 1) % MAP_LONGS;
		mask = ~0UL;
	}
	return MAX_RQ;
}

struct tr_task *tr_dequeue(struct tr_rq *rq) {
	unsigned int idx, d;
	struct tr_task *tsk;

	idx = rel_ffs(rq);
	if (unlikely(idx == MAX_RQ)) {
		rq->tbase = 0;
		return NULL;
	}
	tsk = list_entry(rq->tsks[idx].next, struct tr_task, lnk);
	list_del(&tsk->lnk);
	if (list_empty(&rq->tsks[idx]))
		__clear_bit(idx, rq->map);
	d = (idx - rq->ibase) & RQ_MASK;
	rq->ibase = idx;
	rq->tbase += NS_SLOT * d;

	return tsk;
}

void tr_queue(struct tr_task *tsk, struct tr_rq *rq, nstime_t t) {
	unsigned int idx;

	if (unlikely(rq->tbase == 0))
		rq->tbase = tsk->t /* sched_clock() */;
	tsk->t = t;
	idx = (unsigned int) ((t - rq->tbase) / NS_SLOT);
	if (unlikely(idx >= MAX_RQ))
		idx = MAX_RQ - 1;
	idx = (idx + rq->ibase) & RQ_MASK;
	list_add_tail(&tsk->lnk, &rq->tsks[idx]);
	__set_bit(idx, rq->map);
}




void cfs_test(int ntasks, int times, int loops) {
	int i;
	unsigned long long ts, te;
	struct cfs_task *tasks, *tsk;
	struct cfs_rq rq;

	tasks = (struct cfs_task *) malloc(ntasks * sizeof(struct cfs_task));
	cfs_rqinit(&rq);
	for (i = 0; i < ntasks; i++) {
		tasks[i].t = 0;
		cfs_queue(&tasks[i], &rq, (nstime_t) (rand() % times) * NS_SLOT);
	}
	rdtscll(ts);
	for (i = 0; i < loops; i++) {
		tsk = cfs_dequeue(&rq);
		cfs_queue(tsk, &rq, tsk->t + (nstime_t) (times / 7) * NS_SLOT);
	}
	rdtscll(te);
	free(tasks);

	fprintf(stdout, "CFS = %.2lf cycles/loop\n", (double) (te - ts) / loops);
}

void tr_test(int ntasks, int times, int loops) {
	int i;
	unsigned long long ts, te;
	struct tr_task *tasks, *tsk;
	struct tr_rq rq;

	tasks = (struct tr_task *) malloc(ntasks * sizeof(struct tr_task));
	tr_rqinit(&rq);
	for (i = 0; i < ntasks; i++) {
		tasks[i].t = 0;
		tr_queue(&tasks[i], &rq, (nstime_t) (rand() % times) * NS_SLOT);
	}
	rdtscll(ts);
	for (i = 0; i < loops; i++) {
		tsk = tr_dequeue(&rq);
		tr_queue(tsk, &rq, tsk->t + (nstime_t) (times / 7) * NS_SLOT);
	}
	rdtscll(te);
	free(tasks);

	fprintf(stdout, "TR  = %.2lf cycles/loop\n", (double) (te - ts) / loops);
}



int main(int ac, char **av) {
	int i, ntasks = 128, loops = 200000;

	for (i = 1; i < ac; i++) {
		if (!strcmp(av[i], "-n")) {
			if (++i < ac)
				ntasks = atoi(av[i]);
		} else if (!strcmp(av[i], "-l")) {
			if (++i < ac)
				loops = atoi(av[i]);
		}
	}

	cfs_test(ntasks, MAX_RQ, loops);
	tr_test(ntasks, MAX_RQ, loops);

	return 0;
}

