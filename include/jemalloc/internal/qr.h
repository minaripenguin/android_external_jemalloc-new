#ifndef JEMALLOC_INTERNAL_QR_H
#define JEMALLOC_INTERNAL_QR_H

/* Ring definitions. */
#define qr(a_type)							\
struct {								\
	a_type	*qre_next;						\
	a_type	*qre_prev;						\
}

/* Ring functions. */
#define qr_new(a_qr, a_field) do {					\
	(a_qr)->a_field.qre_next = (a_qr);				\
	(a_qr)->a_field.qre_prev = (a_qr);				\
} while (0)

#define qr_next(a_qr, a_field) ((a_qr)->a_field.qre_next)

#define qr_prev(a_qr, a_field) ((a_qr)->a_field.qre_prev)

/* a_qr_a can directly be a qr_next() macro, but a_qr_b cannot.  */
#define qr_meld(a_qr_a, a_qr_b, a_field) do {				\
	(a_qr_b)->a_field.qre_prev->a_field.qre_next =			\
	    (a_qr_a)->a_field.qre_prev;					\
	(a_qr_a)->a_field.qre_prev = (a_qr_b)->a_field.qre_prev;	\
	(a_qr_b)->a_field.qre_prev =					\
	    (a_qr_b)->a_field.qre_prev->a_field.qre_next;		\
	(a_qr_a)->a_field.qre_prev->a_field.qre_next = (a_qr_a);	\
	(a_qr_b)->a_field.qre_prev->a_field.qre_next = (a_qr_b);	\
} while (0)

#define qr_before_insert(a_qrelm, a_qr, a_field)			\
	qr_meld((a_qrelm), (a_qr), a_field)

#define qr_after_insert(a_qrelm, a_qr, a_field)				\
	qr_before_insert(qr_next(a_qrelm, a_field), (a_qr), a_field)

/*
 * qr_meld() and qr_split() are functionally equivalent, so there's no need to
 * have two copies of the code.
 */
#define qr_split(a_qr_a, a_qr_b, a_field)				\
	qr_meld((a_qr_a), (a_qr_b), a_field)

#define qr_remove(a_qr, a_field)					\
	qr_split(qr_next(a_qr, a_field), (a_qr), a_field)

#define qr_foreach(var, a_qr, a_field)					\
	for ((var) = (a_qr);						\
	    (var) != NULL;						\
	    (var) = (((var)->a_field.qre_next != (a_qr))		\
	    ? (var)->a_field.qre_next : NULL))

#define qr_reverse_foreach(var, a_qr, a_field)				\
	for ((var) = ((a_qr) != NULL) ? qr_prev(a_qr, a_field) : NULL;	\
	    (var) != NULL;						\
	    (var) = (((var) != (a_qr))					\
	    ? (var)->a_field.qre_prev : NULL))

#endif /* JEMALLOC_INTERNAL_QR_H */
