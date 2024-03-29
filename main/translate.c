/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2006, Digium, Inc.
 *
 * Mark Spencer <markster@digium.com>
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*! \file
 *
 * \brief Translate via the use of pseudo channels
 *
 * \author Mark Spencer <markster@digium.com> 
 */

/*** MODULEINFO
	<support_level>core</support_level>
 ***/

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision: 369001 $")

#include <sys/time.h>
#include <sys/resource.h>
#include <math.h>

#include "asterisk/lock.h"
#include "asterisk/channel.h"
#include "asterisk/translate.h"
#include "asterisk/module.h"
#include "asterisk/frame.h"
#include "asterisk/sched.h"
#include "asterisk/cli.h"
#include "asterisk/term.h"

#define MAX_RECALC 1000 /* max sample recalc */

/*! \brief the list of translators */
static AST_RWLIST_HEAD_STATIC(translators, ast_translator);


/*! \brief these values indicate how a translation path will affect the sample rate
 *
 *  \note These must stay in this order.  They are ordered by most optimal selection first.
 */
enum path_samp_change {

	/* Lossless Source Translation Costs */

	/*! [lossless -> lossless] original sampling */
	AST_TRANS_COST_LL_LL_ORIGSAMP = 400000,
	/*! [lossless -> lossy]    original sampling */
	AST_TRANS_COST_LL_LY_ORIGSAMP = 600000,

	/*! [lossless -> lossless] up sample */
	AST_TRANS_COST_LL_LL_UPSAMP   = 800000,
	/*! [lossless -> lossy]    up sample */
	AST_TRANS_COST_LL_LY_UPSAMP   = 825000,

	/*! [lossless -> lossless] down sample */
	AST_TRANS_COST_LL_LL_DOWNSAMP = 850000,
	/*! [lossless -> lossy]    down sample */
	AST_TRANS_COST_LL_LY_DOWNSAMP = 875000,

	/*! [lossless -> unknown]    unknown.
	 * This value is for a lossless source translation
	 * with an unknown destination and or sample rate conversion. */
	AST_TRANS_COST_LL_UNKNOWN     = 885000,

	/* Lossy Source Translation Costs */

	/*! [lossy -> lossless]    original sampling */
	AST_TRANS_COST_LY_LL_ORIGSAMP = 900000,
	/*! [lossy -> lossy]       original sampling */
	AST_TRANS_COST_LY_LY_ORIGSAMP = 915000,

	/*! [lossy -> lossless]    up sample */
	AST_TRANS_COST_LY_LL_UPSAMP   = 930000,
	/*! [lossy -> lossy]       up sample */
	AST_TRANS_COST_LY_LY_UPSAMP   = 945000,

	/*! [lossy -> lossless]    down sample */
	AST_TRANS_COST_LY_LL_DOWNSAMP = 960000,
	/*! [lossy -> lossy]       down sample */
	AST_TRANS_COST_LY_LY_DOWNSAMP = 975000,

	/*! [lossy -> unknown]    unknown.
	 * This value is for a lossy source translation
	 * with an unknown destination and or sample rate conversion. */
	AST_TRANS_COST_LY_UNKNOWN     = 985000,
};

struct translator_path {
	struct ast_translator *step;	/*!< Next step translator */
	unsigned int cost;		/*!< Complete cost to destination */
	unsigned int multistep;		/*!< Multiple conversions required for this translation */
	enum path_samp_change rate_change; /*!< does this path require a sample rate change, if so what kind. */
};

/*! \brief a matrix that, for any pair of supported formats,
 * indicates the total cost of translation and the first step.
 * The full path can be reconstricted iterating on the matrix
 * until step->dstfmt == desired_format.
 *
 * Array indexes are 'src' and 'dest', in that order.
 *
 * Note: the lock in the 'translators' list is also used to protect
 * this structure.
 */
static struct translator_path tr_matrix[MAX_FORMAT][MAX_FORMAT];

/*! \todo
 * TODO: sample frames for each supported input format.
 * We build this on the fly, by taking an SLIN frame and using
 * the existing converter to play with it.
 */

/*! \brief returns the index of the lowest bit set */
static force_inline int powerof(format_t d)
{
	int x = ffsll(d);

	if (x)
		return x - 1;

	ast_log(LOG_WARNING, "No bits set? %llu\n", (unsigned long long) d);

	return -1;
}

/*
 * wrappers around the translator routines.
 */

/*!
 * \brief Allocate the descriptor, required outbuf space,
 * and possibly desc.
 */
static void *newpvt(struct ast_translator *t)
{
	struct ast_trans_pvt *pvt;
	int len;
	char *ofs;

	/*
	 * compute the required size adding private descriptor,
	 * buffer, AST_FRIENDLY_OFFSET.
	 */
	len = sizeof(*pvt) + t->desc_size;
	if (t->buf_size)
		len += AST_FRIENDLY_OFFSET + t->buf_size;
	pvt = ast_calloc(1, len);
	if (!pvt)
		return NULL;
	pvt->t = t;
	ofs = (char *)(pvt + 1);	/* pointer to data space */
	if (t->desc_size) {		/* first comes the descriptor */
		pvt->pvt = ofs;
		ofs += t->desc_size;
	}
	if (t->buf_size)		/* finally buffer and header */
		pvt->outbuf.c = ofs + AST_FRIENDLY_OFFSET;
	/* call local init routine, if present */
	if (t->newpvt && t->newpvt(pvt)) {
		ast_free(pvt);
		return NULL;
	}
	ast_module_ref(t->module);
	return pvt;
}

static void destroy(struct ast_trans_pvt *pvt)
{
	struct ast_translator *t = pvt->t;

	if (t->destroy)
		t->destroy(pvt);
	ast_free(pvt);
	ast_module_unref(t->module);
}

/*! \brief framein wrapper, deals with bound checks.  */
static int framein(struct ast_trans_pvt *pvt, struct ast_frame *f)
{
	int ret;
	int samples = pvt->samples;	/* initial value */
	
	/* Copy the last in jb timing info to the pvt */
	ast_copy_flags(&pvt->f, f, AST_FRFLAG_HAS_TIMING_INFO);
	pvt->f.ts = f->ts;
	pvt->f.len = f->len;
	pvt->f.seqno = f->seqno;

	if (f->samples == 0) {
		ast_log(LOG_WARNING, "no samples for %s\n", pvt->t->name);
	}
	if (pvt->t->buffer_samples) {	/* do not pass empty frames to callback */
		if (f->datalen == 0) { /* perform native PLC if available */
			/* If the codec has native PLC, then do that */
			if (!pvt->t->native_plc)
				return 0;
		}
		if (pvt->samples + f->samples > pvt->t->buffer_samples) {
			ast_log(LOG_WARNING, "Out of buffer space\n");
			return -1;
		}
	}
	/* we require a framein routine, wouldn't know how to do
	 * it otherwise.
	 */
	ret = pvt->t->framein(pvt, f);
	/* diagnostic ... */
	if (pvt->samples == samples)
		ast_log(LOG_WARNING, "%s did not update samples %d\n",
			pvt->t->name, pvt->samples);
	return ret;
}

/*! \brief generic frameout routine.
 * If samples and datalen are 0, take whatever is in pvt
 * and reset them, otherwise take the values in the caller and
 * leave alone the pvt values.
 */
struct ast_frame *ast_trans_frameout(struct ast_trans_pvt *pvt,
	int datalen, int samples)
{
	struct ast_frame *f = &pvt->f;

	if (samples)
		f->samples = samples;
	else {
		if (pvt->samples == 0)
			return NULL;
		f->samples = pvt->samples;
		pvt->samples = 0;
	}
	if (datalen)
		f->datalen = datalen;
	else {
		f->datalen = pvt->datalen;
		pvt->datalen = 0;
	}

	f->frametype = AST_FRAME_VOICE;
	f->subclass.codec = 1LL << (pvt->t->dstfmt);
	f->mallocd = 0;
	f->offset = AST_FRIENDLY_OFFSET;
	f->src = pvt->t->name;
	f->data.ptr = pvt->outbuf.c;

	return ast_frisolate(f);
}

static struct ast_frame *default_frameout(struct ast_trans_pvt *pvt)
{
	return ast_trans_frameout(pvt, 0, 0);
}

/* end of callback wrappers and helpers */

void ast_translator_free_path(struct ast_trans_pvt *p)
{
	struct ast_trans_pvt *pn = p;
	while ( (p = pn) ) {
		pn = p->next;
		destroy(p);
	}
}

/*! \brief Build a chain of translators based upon the given source and dest formats */
struct ast_trans_pvt *ast_translator_build_path(format_t dest, format_t source)
{
	struct ast_trans_pvt *head = NULL, *tail = NULL;
	
	source = powerof(source);
	dest = powerof(dest);

	if (source == -1 || dest == -1) {
		ast_log(LOG_WARNING, "No translator path: (%s codec is not valid)\n", source == -1 ? "starting" : "ending");
		return NULL;
	}

	AST_RWLIST_RDLOCK(&translators);

	while (source != dest) {
		struct ast_trans_pvt *cur;
		struct ast_translator *t = tr_matrix[source][dest].step;
		if (!t) {
			ast_log(LOG_WARNING, "No translator path from %s to %s\n", 
				ast_getformatname(source), ast_getformatname(dest));
			AST_RWLIST_UNLOCK(&translators);
			return NULL;
		}
		if (!(cur = newpvt(t))) {
			ast_log(LOG_WARNING, "Failed to build translator step from %s to %s\n",
				ast_getformatname(source), ast_getformatname(dest));
			if (head)
				ast_translator_free_path(head);	
			AST_RWLIST_UNLOCK(&translators);
			return NULL;
		}
		if (!head)
			head = cur;
		else
			tail->next = cur;
		tail = cur;
		cur->nextin = cur->nextout = ast_tv(0, 0);
		/* Keep going if this isn't the final destination */
		source = cur->t->dstfmt;
	}

	AST_RWLIST_UNLOCK(&translators);
	return head;
}

/*! \brief do the actual translation */
struct ast_frame *ast_translate(struct ast_trans_pvt *path, struct ast_frame *f, int consume)
{
	struct ast_trans_pvt *p = path;
	struct ast_frame *out = f;
	struct timeval delivery;
	int has_timing_info;
	long ts;
	long len;
	int seqno;

	has_timing_info = ast_test_flag(f, AST_FRFLAG_HAS_TIMING_INFO);
	ts = f->ts;
	len = f->len;
	seqno = f->seqno;

	/* XXX hmmm... check this below */
	if (!ast_tvzero(f->delivery)) {
		if (!ast_tvzero(path->nextin)) {
			/* Make sure this is in line with what we were expecting */
			if (!ast_tveq(path->nextin, f->delivery)) {
				/* The time has changed between what we expected and this
				   most recent time on the new packet.  If we have a
				   valid prediction adjust our output time appropriately */
				if (!ast_tvzero(path->nextout)) {
					path->nextout = ast_tvadd(path->nextout,
								  ast_tvsub(f->delivery, path->nextin));
				}
				path->nextin = f->delivery;
			}
		} else {
			/* This is our first pass.  Make sure the timing looks good */
			path->nextin = f->delivery;
			path->nextout = f->delivery;
		}
		/* Predict next incoming sample */
		path->nextin = ast_tvadd(path->nextin, ast_samp2tv(f->samples, ast_format_rate(f->subclass.codec)));
	}
	delivery = f->delivery;
	for ( ; out && p ; p = p->next) {
		framein(p, out);
		if (out != f)
			ast_frfree(out);
		out = p->t->frameout(p);
	}
	if (consume)
		ast_frfree(f);
	if (out == NULL)
		return NULL;
	/* we have a frame, play with times */
	if (!ast_tvzero(delivery)) {
		/* Regenerate prediction after a discontinuity */
		if (ast_tvzero(path->nextout))
			path->nextout = ast_tvnow();

		/* Use next predicted outgoing timestamp */
		out->delivery = path->nextout;
		
		/* Predict next outgoing timestamp from samples in this
		   frame. */
		path->nextout = ast_tvadd(path->nextout, ast_samp2tv(out->samples, ast_format_rate(out->subclass.codec)));
	} else {
		out->delivery = ast_tv(0, 0);
		ast_set2_flag(out, has_timing_info, AST_FRFLAG_HAS_TIMING_INFO);
		if (has_timing_info) {
			out->ts = ts;
			out->len = len;
			out->seqno = seqno;
		}
	}
	/* Invalidate prediction if we're entering a silence period */
	if (out->frametype == AST_FRAME_CNG)
		path->nextout = ast_tv(0, 0);
	return out;
}

/*! \brief compute the cost of a single translation step */
static void calc_cost(struct ast_translator *t, int seconds)
{
	int num_samples = 0;
	struct ast_trans_pvt *pvt;
	struct rusage start;
	struct rusage end;
	int cost;
	int out_rate = ast_format_rate(t->dstfmt);

	if (!seconds)
		seconds = 1;
	
	/* If they don't make samples, give them a terrible score */
	if (!t->sample) {
		ast_log(LOG_WARNING, "Translator '%s' does not produce sample frames.\n", t->name);
		t->cost = 999999;
		return;
	}

	pvt = newpvt(t);
	if (!pvt) {
		ast_log(LOG_WARNING, "Translator '%s' appears to be broken and will probably fail.\n", t->name);
		t->cost = 999999;
		return;
	}

	getrusage(RUSAGE_SELF, &start);

	/* Call the encoder until we've processed the required number of samples */
	while (num_samples < seconds * out_rate) {
		struct ast_frame *f = t->sample();
		if (!f) {
			ast_log(LOG_WARNING, "Translator '%s' failed to produce a sample frame.\n", t->name);
			destroy(pvt);
			t->cost = 999999;
			return;
		}
		framein(pvt, f);
		ast_frfree(f);
		while ((f = t->frameout(pvt))) {
			num_samples += f->samples;
			ast_frfree(f);
		}
	}

	getrusage(RUSAGE_SELF, &end);

	cost = ((end.ru_utime.tv_sec - start.ru_utime.tv_sec) * 1000000) + end.ru_utime.tv_usec - start.ru_utime.tv_usec;
	cost += ((end.ru_stime.tv_sec - start.ru_stime.tv_sec) * 1000000) + end.ru_stime.tv_usec - start.ru_stime.tv_usec;

	destroy(pvt);

	t->cost = cost / seconds;

	if (!t->cost)
		t->cost = 1;
}

static enum path_samp_change get_rate_change_result(format_t src, format_t dst)
{
	int src_ll = src == AST_FORMAT_SLINEAR || src == AST_FORMAT_SLINEAR16;
	int dst_ll = dst == AST_FORMAT_SLINEAR || src == AST_FORMAT_SLINEAR16;
	int src_rate = ast_format_rate(src);
	int dst_rate = ast_format_rate(dst);

	if (src_ll) {
		if (dst_ll && (src_rate == dst_rate)) {
			return AST_TRANS_COST_LL_LL_ORIGSAMP;
		} else if (!dst_ll && (src_rate == dst_rate)) {
			return AST_TRANS_COST_LL_LY_ORIGSAMP;
		} else if (dst_ll && (src_rate < dst_rate)) {
			return AST_TRANS_COST_LL_LL_UPSAMP;
		} else if (!dst_ll && (src_rate < dst_rate)) {
			return AST_TRANS_COST_LL_LY_UPSAMP;
		} else if (dst_ll && (src_rate > dst_rate)) {
			return AST_TRANS_COST_LL_LL_DOWNSAMP;
		} else if (!dst_ll && (src_rate > dst_rate)) {
			return AST_TRANS_COST_LL_LY_DOWNSAMP;
		} else {
			return AST_TRANS_COST_LL_UNKNOWN;
		}
	} else {
		if (dst_ll && (src_rate == dst_rate)) {
			return AST_TRANS_COST_LY_LL_ORIGSAMP;
		} else if (!dst_ll && (src_rate == dst_rate)) {
			return AST_TRANS_COST_LY_LY_ORIGSAMP;
		} else if (dst_ll && (src_rate < dst_rate)) {
			return AST_TRANS_COST_LY_LL_UPSAMP;
		} else if (!dst_ll && (src_rate < dst_rate)) {
			return AST_TRANS_COST_LY_LY_UPSAMP;
		} else if (dst_ll && (src_rate > dst_rate)) {
			return AST_TRANS_COST_LY_LL_DOWNSAMP;
		} else if (!dst_ll && (src_rate > dst_rate)) {
			return AST_TRANS_COST_LY_LY_DOWNSAMP;
		} else {
			return AST_TRANS_COST_LY_UNKNOWN;
		}
	}
}

/*!
 * \brief rebuild a translation matrix.
 * \note This function expects the list of translators to be locked
*/
static void rebuild_matrix(int samples)
{
	struct ast_translator *t;
	int new_rate_change;
	int newcost;
	int x;      /* source format index */
	int y;      /* intermediate format index */
	int z;      /* destination format index */

	ast_debug(1, "Resetting translation matrix\n");

	memset(tr_matrix, '\0', sizeof(tr_matrix));

	/* first, compute all direct costs */
	AST_RWLIST_TRAVERSE(&translators, t, list) {
		if (!t->active)
			continue;

		x = t->srcfmt;
		z = t->dstfmt;

		if (samples)
			calc_cost(t, samples);

		new_rate_change = get_rate_change_result(1LL << t->srcfmt, 1LL << t->dstfmt);

		/* this translator is the best choice if any of the below are true.
		 * 1. no translation path is set between x and z yet.
		 * 2. the new translation costs less and sample rate is no worse than old one. 
		 * 3. the new translation has a better sample rate conversion than the old one.
		 */
		if (!tr_matrix[x][z].step ||
			((t->cost < tr_matrix[x][z].cost) && (new_rate_change <= tr_matrix[x][z].rate_change)) ||
			(new_rate_change < tr_matrix[x][z].rate_change)) {

			tr_matrix[x][z].step = t;
			tr_matrix[x][z].cost = t->cost;
			tr_matrix[x][z].rate_change = new_rate_change;
		}
	}

	/*
	 * For each triple x, y, z of distinct formats, check if there is
	 * a path from x to z through y which is cheaper than what is
	 * currently known, and in case, update the matrix.
	 * Repeat until the matrix is stable.
	 */
	for (;;) {
		int changed = 0;
		int better_choice = 0;
		for (x = 0; x < MAX_FORMAT; x++) {      /* source format */
			for (y = 0; y < MAX_FORMAT; y++) {    /* intermediate format */
				if (x == y)                     /* skip ourselves */
					continue;
				for (z = 0; z < MAX_FORMAT; z++) {  /* dst format */
					if (z == x || z == y)       /* skip null conversions */
						continue;
					if (!tr_matrix[x][y].step)  /* no path from x to y */
						continue;
					if (!tr_matrix[y][z].step)  /* no path from y to z */
						continue;

					/* Does x->y->z result in a less optimal sample rate change?
					 * Never downgrade the sample rate conversion quality regardless
					 * of any cost improvements */
					if (tr_matrix[x][z].step &&
						((tr_matrix[x][z].rate_change < tr_matrix[x][y].rate_change) ||
						(tr_matrix[x][z].rate_change < tr_matrix[y][z].rate_change))) {
						continue;
					}

					/* is x->y->z a better sample rate confersion that the current x->z? */
					new_rate_change = tr_matrix[x][y].rate_change + tr_matrix[y][z].rate_change;

					/* calculate cost from x->y->z */
					newcost = tr_matrix[x][y].cost + tr_matrix[y][z].cost;

					/* Is x->y->z a better choice than x->z?
					 * There are three conditions for x->y->z to be a better choice than x->z
					 * 1. if there is no step directly between x->z then x->y->z is the best and only current option.
					 * 2. if x->y->z results in a more optimal sample rate conversion. */
					if (!tr_matrix[x][z].step) {
						better_choice = 1;
					} else if (new_rate_change < tr_matrix[x][z].rate_change) {
						better_choice = 1;
					} else {
						better_choice = 0;
					}

					if (!better_choice) {
						continue;
					}
					/* ok, we can get from x to z via y with a cost that
					   is the sum of the transition from x to y and from y to z */
					tr_matrix[x][z].step = tr_matrix[x][y].step;
					tr_matrix[x][z].cost = newcost;
					tr_matrix[x][z].multistep = 1;

					/* now calculate what kind of sample rate change is required for this multi-step path
					 * 
					 * if both paths require a change in rate, and they are not in the same direction
					 * then this is a up sample down sample conversion scenario. */
					tr_matrix[x][z].rate_change = tr_matrix[x][y].rate_change + tr_matrix[y][z].rate_change;

					ast_debug(3, "Discovered %d cost path from %s to %s, via %s\n", tr_matrix[x][z].cost,
						  ast_getformatname(1LL << x), ast_getformatname(1LL << z), ast_getformatname(1LL << y));
					changed++;
				}
			}
		}
		if (!changed)
			break;
	}
}

const char *ast_translate_path_to_str(struct ast_trans_pvt *p, struct ast_str **str)
{
	struct ast_trans_pvt *pn = p;

	if (!p || !p->t) {
		return "";
	}

	ast_str_set(str, 0, "%s", ast_getformatname(1LL << p->t->srcfmt));

	while ( (p = pn) ) {
		pn = p->next;
		ast_str_append(str, 0, "->%s", ast_getformatname(1LL << p->t->dstfmt));
	}

	return ast_str_buffer(*str);
}

static char *complete_trans_path_choice(const char *line, const char *word, int pos, int state)
{
	int which = 0;
	int wordlen = strlen(word);
	int i;
	char *ret = NULL;
	size_t len = 0;
	const struct ast_format_list *format_list = ast_get_format_list(&len);

	for (i = 0; i < len; i++) {
		if (!(format_list[i].bits & AST_FORMAT_AUDIO_MASK)) {
			continue;
		}
		if (!strncasecmp(word, format_list[i].name, wordlen) && ++which > state) {
			ret = ast_strdup(format_list[i].name);
			break;
		}
	}
	return ret;
}

static char *handle_cli_core_show_translation(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
#define SHOW_TRANS 64
	static const char * const option1[] = { "recalc", "paths", NULL };
	int x, y, z;
	int curlen = 0, longest = 0, magnitude[SHOW_TRANS] = { 0, };

	switch (cmd) {
	case CLI_INIT:
		e->command = "core show translation";
		e->usage =
			"Usage: 'core show translation' can be used in two ways.\n"
			"       1. 'core show translation [recalc [<recalc seconds>]]\n"
			"          Displays known codec translators and the cost associated\n"
			"          with each conversion.  If the argument 'recalc' is supplied along\n"
			"          with optional number of seconds to test a new test will be performed\n"
			"          as the chart is being displayed.\n"
			"       2. 'core show translation paths [codec]'\n"
			"           This will display all the translation paths associated with a codec\n";
		return NULL;
	case CLI_GENERATE:
		if (a->pos == 3) {
			return ast_cli_complete(a->word, option1, a->n);
		}
		if (a->pos == 4 && !strcasecmp(a->argv[3], option1[1])) {
			return complete_trans_path_choice(a->line, a->word, a->pos, a->n);
		}
		return NULL;
	}

	if (a->argc > 5)
		return CLI_SHOWUSAGE;

	if (a->argv[3] && !strcasecmp(a->argv[3], option1[1]) && a->argc == 5) {
		format_t input_src = 0;
		format_t src = 0;
		size_t len = 0;
		int dst;
		int i;
		const struct ast_format_list *format_list = ast_get_format_list(&len);
		struct ast_str *str = ast_str_alloca(256);
		struct ast_translator *step;

		for (i = 0; i < len; i++) {
			if (!(format_list[i].bits & AST_FORMAT_AUDIO_MASK)) {
				continue;
			}
			if (!strncasecmp(format_list[i].name, a->argv[4], strlen(format_list[i].name))) {
				input_src = format_list[i].bits;
			}
		}

		if (!input_src) {
			ast_cli(a->fd, "Source codec \"%s\" is not found.\n", a->argv[4]);
			return CLI_FAILURE;
		}

		AST_RWLIST_RDLOCK(&translators);
		ast_cli(a->fd, "--- Translation paths SRC Codec \"%s\" sample rate %d ---\n", a->argv[4], ast_format_rate(input_src));
		for (i = 0; i < len; i++) {
			if (!(format_list[i].bits & AST_FORMAT_AUDIO_MASK) || (format_list[i].bits == input_src)) {
				continue;
			}
			/* Note that dst can never be -1, as an element of format_list will have
			 * at least one bit set.  src cannot be -1 as well, as it is previously
			 * sanitized - hence it is safe to directly index tr_matrix with the results
			 * of powerof.
			 */
			dst = powerof(format_list[i].bits);
			src = powerof(input_src);
			ast_str_reset(str);
			if (tr_matrix[src][dst].step) {
				ast_str_append(&str, 0, "%s", ast_getformatname(1LL << tr_matrix[src][dst].step->srcfmt));
				while (src != dst) {
					step = tr_matrix[src][dst].step;
					if (!step) {
						ast_str_reset(str);
						break;
					}
					ast_str_append(&str, 0, "->%s", ast_getformatname(1LL << step->dstfmt));
					src = step->dstfmt;
				}
			}

			if (ast_strlen_zero(ast_str_buffer(str))) {
				ast_str_set(&str, 0, "No Translation Path");
			}

			ast_cli(a->fd, "\t%-10.10s To %-10.10s: %-60.60s\n", a->argv[4], format_list[i].name, ast_str_buffer(str));
		}
		AST_RWLIST_UNLOCK(&translators);

		return CLI_SUCCESS;
	} else if (a->argv[3] && !strcasecmp(a->argv[3], "recalc")) {
		z = a->argv[4] ? atoi(a->argv[4]) : 1;

		if (z <= 0) {
			ast_cli(a->fd, "         Recalc must be greater than 0.  Defaulting to 1.\n");
			z = 1;
		}

		if (z > MAX_RECALC) {
			ast_cli(a->fd, "         Maximum limit of recalc exceeded by %d, truncating value to %d\n", z - MAX_RECALC, MAX_RECALC);
			z = MAX_RECALC;
		}
		ast_cli(a->fd, "         Recalculating Codec Translation (number of sample seconds: %d)\n\n", z);
		AST_RWLIST_WRLOCK(&translators);
		rebuild_matrix(z);
		AST_RWLIST_UNLOCK(&translators);
	} else if (a->argc > 3)
		return CLI_SHOWUSAGE;

	AST_RWLIST_RDLOCK(&translators);

	ast_cli(a->fd, "         Translation times between formats (in microseconds) for one second of data\n");
	ast_cli(a->fd, "          Source Format (Rows) Destination Format (Columns)\n\n");
	/* Get the length of the longest (usable?) codec name, so we know how wide the left side should be */
	for (x = 0; x < SHOW_TRANS; x++) {
		/* translation only applies to audio right now. */
		if (!(AST_FORMAT_AUDIO_MASK & (1LL << (x))))
			continue;
		curlen = strlen(ast_getformatname(1LL << (x)));
		if (curlen > longest)
			longest = curlen;
		for (y = 0; y < SHOW_TRANS; y++) {
			if (!(AST_FORMAT_AUDIO_MASK & (1LL << (y))))
				continue;
			if (tr_matrix[x][y].cost > pow(10, magnitude[x])) {
				magnitude[y] = floor(log10(tr_matrix[x][y].cost));
			}
		}
	}
	for (x = -1; x < SHOW_TRANS; x++) {
		struct ast_str *out = ast_str_alloca(256);
		/* translation only applies to audio right now. */
		if (x >= 0 && !(AST_FORMAT_AUDIO_MASK & (1LL << (x))))
			continue;
		/*Go ahead and move to next iteration if dealing with an unknown codec*/
		if(x >= 0 && !strcmp(ast_getformatname(1LL << (x)), "unknown"))
			continue;
		ast_str_set(&out, -1, " ");
		for (y = -1; y < SHOW_TRANS; y++) {
			/* translation only applies to audio right now. */
			if (y >= 0 && !(AST_FORMAT_AUDIO_MASK & (1LL << (y))))
				continue;
			/*Go ahead and move to next iteration if dealing with an unknown codec*/
			if (y >= 0 && !strcmp(ast_getformatname(1LL << (y)), "unknown"))
				continue;
			if (y >= 0)
				curlen = strlen(ast_getformatname(1LL << (y)));
			if (y >= 0 && magnitude[y] + 1 > curlen) {
				curlen = magnitude[y] + 1;
			}
			if (curlen < 5)
				curlen = 5;
			if (x >= 0 && y >= 0 && tr_matrix[x][y].step) {
				/* Actual codec output */
				ast_str_append(&out, -1, "%*d", curlen + 1, tr_matrix[x][y].cost);
			} else if (x == -1 && y >= 0) {
				/* Top row - use a dynamic size */
				ast_str_append(&out, -1, "%*s", curlen + 1, ast_getformatname(1LL << (y)) );
			} else if (y == -1 && x >= 0) {
				/* Left column - use a static size. */
				ast_str_append(&out, -1, "%*s", longest, ast_getformatname(1LL << (x)) );
			} else if (x >= 0 && y >= 0) {
				/* Codec not supported */
				ast_str_append(&out, -1, "%*s", curlen + 1, "-");
			} else {
				/* Upper left hand corner */
				ast_str_append(&out, -1, "%*s", longest, "");
			}
		}
		ast_str_append(&out, -1, "\n");
		ast_cli(a->fd, "%s", ast_str_buffer(out));
	}
	AST_RWLIST_UNLOCK(&translators);
	return CLI_SUCCESS;
}

static struct ast_cli_entry cli_translate[] = {
	AST_CLI_DEFINE(handle_cli_core_show_translation, "Display translation matrix")
};

/*! \brief register codec translator */
int __ast_register_translator(struct ast_translator *t, struct ast_module *mod)
{
	static int added_cli = 0;
	struct ast_translator *u;
	char tmp[80];

	if (!mod) {
		ast_log(LOG_WARNING, "Missing module pointer, you need to supply one\n");
		return -1;
	}

	if (!t->buf_size) {
		ast_log(LOG_WARNING, "empty buf size, you need to supply one\n");
		return -1;
	}

	t->module = mod;

	t->srcfmt = powerof(t->srcfmt);
	t->dstfmt = powerof(t->dstfmt);
	t->active = 1;

	if (t->srcfmt == -1 || t->dstfmt == -1) {
		ast_log(LOG_WARNING, "Invalid translator path: (%s codec is not valid)\n", t->srcfmt == -1 ? "starting" : "ending");
		return -1;
	}
	if (t->srcfmt >= MAX_FORMAT) {
		ast_log(LOG_WARNING, "Source format %s is larger than MAX_FORMAT\n", ast_getformatname(t->srcfmt));
		return -1;
	}

	if (t->dstfmt >= MAX_FORMAT) {
		ast_log(LOG_WARNING, "Destination format %s is larger than MAX_FORMAT\n", ast_getformatname(t->dstfmt));
		return -1;
	}

	if (t->buf_size) {
		/*
		 * Align buf_size properly, rounding up to the machine-specific
		 * alignment for pointers.
		 */
		struct _test_align { void *a, *b; } p;
		int align = (char *)&p.b - (char *)&p.a;

		t->buf_size = ((t->buf_size + align - 1) / align) * align;
	}

	if (t->frameout == NULL)
		t->frameout = default_frameout;
  
	calc_cost(t, 1);

	ast_verb(2, "Registered translator '%s' from format %s to %s, cost %d\n",
			    term_color(tmp, t->name, COLOR_MAGENTA, COLOR_BLACK, sizeof(tmp)),
			    ast_getformatname(1LL << t->srcfmt), ast_getformatname(1LL << t->dstfmt), t->cost);

	if (!added_cli) {
		ast_cli_register_multiple(cli_translate, ARRAY_LEN(cli_translate));
		added_cli++;
	}

	AST_RWLIST_WRLOCK(&translators);

	/* find any existing translators that provide this same srcfmt/dstfmt,
	   and put this one in order based on cost */
	AST_RWLIST_TRAVERSE_SAFE_BEGIN(&translators, u, list) {
		if ((u->srcfmt == t->srcfmt) &&
		    (u->dstfmt == t->dstfmt) &&
		    (u->cost > t->cost)) {
			AST_RWLIST_INSERT_BEFORE_CURRENT(t, list);
			t = NULL;
			break;
		}
	}
	AST_RWLIST_TRAVERSE_SAFE_END;

	/* if no existing translator was found for this format combination,
	   add it to the beginning of the list */
	if (t)
		AST_RWLIST_INSERT_HEAD(&translators, t, list);

	rebuild_matrix(0);

	AST_RWLIST_UNLOCK(&translators);

	return 0;
}

/*! \brief unregister codec translator */
int ast_unregister_translator(struct ast_translator *t)
{
	char tmp[80];
	struct ast_translator *u;
	int found = 0;

	AST_RWLIST_WRLOCK(&translators);
	AST_RWLIST_TRAVERSE_SAFE_BEGIN(&translators, u, list) {
		if (u == t) {
			AST_RWLIST_REMOVE_CURRENT(list);
			ast_verb(2, "Unregistered translator '%s' from format %s to %s\n", term_color(tmp, t->name, COLOR_MAGENTA, COLOR_BLACK, sizeof(tmp)), ast_getformatname(1LL << t->srcfmt), ast_getformatname(1LL << t->dstfmt));
			found = 1;
			break;
		}
	}
	AST_RWLIST_TRAVERSE_SAFE_END;

	if (found)
		rebuild_matrix(0);

	AST_RWLIST_UNLOCK(&translators);

	return (u ? 0 : -1);
}

void ast_translator_activate(struct ast_translator *t)
{
	AST_RWLIST_WRLOCK(&translators);
	t->active = 1;
	rebuild_matrix(0);
	AST_RWLIST_UNLOCK(&translators);
}

void ast_translator_deactivate(struct ast_translator *t)
{
	AST_RWLIST_WRLOCK(&translators);
	t->active = 0;
	rebuild_matrix(0);
	AST_RWLIST_UNLOCK(&translators);
}

/*! \brief Calculate our best translator source format, given costs, and a desired destination */
format_t ast_translator_best_choice(format_t *dst, format_t *srcs)
{
	int x,y;
	int better = 0;
	int besttime = INT_MAX;
	int beststeps = INT_MAX;
	unsigned int best_rate_change = INT_MAX;
	format_t best = -1;
	format_t bestdst = 0;
	format_t cur, cursrc;
	format_t common = ((*dst) & (*srcs)) & AST_FORMAT_AUDIO_MASK;	/* are there common formats ? */

	/* when we're called on a local channel with no other channel, pick ulaw rather than the
	 * codec with the highest bit rate
	 */
	if ((*srcs & AST_FORMAT_AUDIO_MASK) == AST_FORMAT_AUDIO_MASK && (*dst & (1 << 2))) {
		*srcs = *dst = best = (1 << 2);
		return 0;
	} else if (common) { /* yes, pick one and return */
		for (cur = 1, y = 0; y <= MAX_AUDIO_FORMAT; cur <<= 1, y++) {
			if (!(cur & common)) {
				continue;
			}

			/* We are guaranteed to find one common format. */
			if (best == -1) {
				best = cur;
				continue;
			}
			/* If there are multiple common formats, pick the one with the highest sample rate */
			if (ast_format_rate(best) < ast_format_rate(cur)) {
				best = cur;
				continue;
			}
		}
		/* We are done, this is a common format to both. */
		*srcs = *dst = best;
		return 0;
	} else {      /* No, we will need to translate */
		AST_RWLIST_RDLOCK(&translators);
		for (cur = 1, y = 0; y <= MAX_AUDIO_FORMAT; cur <<= 1, y++) {
			if (! (cur & *dst)) {
				continue;
			}
			for (cursrc = 1, x = 0; x <= MAX_AUDIO_FORMAT; cursrc <<= 1, x++) {
				if (!(*srcs & cursrc) || !tr_matrix[x][y].step) {
					continue;
				}

				/* This is a better choice if any of the following are true.
				 * 1. The sample rate conversion is better than the current pick.
				 * 2. the sample rate conversion is no worse than the current pick and the cost or multistep is better
				 */
				better = 0;
				if (tr_matrix[x][y].rate_change < best_rate_change) {
					better = 1; /* this match has a better rate conversion */
				}
				if ((tr_matrix[x][y].rate_change <= best_rate_change) &&
					(tr_matrix[x][y].cost < besttime || tr_matrix[x][y].multistep < beststeps)) {
					better = 1; /* this match has no worse rate conversion and the conversion cost is less */
				}
				if (better) {
					/* better than what we have so far */
					best = cursrc;
					bestdst = cur;
					besttime = tr_matrix[x][y].cost;
					beststeps = tr_matrix[x][y].multistep;
					best_rate_change = tr_matrix[x][y].rate_change;
				}
			}
		}
		AST_RWLIST_UNLOCK(&translators);
		if (best > -1) {
			*srcs = best;
			*dst = bestdst;
			best = 0;
		}
		return best;
	}
}

unsigned int ast_translate_path_steps(format_t dest, format_t src)
{
	unsigned int res = -1;

	/* convert bitwise format numbers into array indices */
	src = powerof(src);
	dest = powerof(dest);

	if (src == -1 || dest == -1) {
		ast_log(LOG_WARNING, "No translator path: (%s codec is not valid)\n", src == -1 ? "starting" : "ending");
		return -1;
	}
	AST_RWLIST_RDLOCK(&translators);

	if (tr_matrix[src][dest].step)
		res = tr_matrix[src][dest].multistep + 1;

	AST_RWLIST_UNLOCK(&translators);

	return res;
}

format_t ast_translate_available_formats(format_t dest, format_t src)
{
	format_t res = dest;
	format_t x;
	format_t src_audio = src & AST_FORMAT_AUDIO_MASK;
	format_t src_video = src & AST_FORMAT_VIDEO_MASK;
	format_t x_bits;

	/* if we don't have a source format, we just have to try all
	   possible destination formats */
	if (!src)
		return dest;

	/* If we have a source audio format, get its format index */
	if (src_audio) {
		src_audio = powerof(src_audio);
	}

	/* If we have a source video format, get its format index */
	if (src_video) {
		src_video = powerof(src_video);
	}

	/* Note that src_audio and src_video are guaranteed to not be
	 * negative at this point, as we ensured they were non-zero.  It is
	 * safe to use the return value of powerof as an index into tr_matrix.
	 */

	AST_RWLIST_RDLOCK(&translators);

	/* For a given source audio format, traverse the list of
	   known audio formats to determine whether there exists
	   a translation path from the source format to the
	   destination format. */
	for (x = 1LL; src_audio && x > 0; x <<= 1) {
		if (!(x & AST_FORMAT_AUDIO_MASK)) {
			continue;
		}

		/* if this is not a desired format, nothing to do */
		if (!(dest & x))
			continue;

		/* if the source is supplying this format, then
		   we can leave it in the result */
		if (src & x)
			continue;

		/* if we don't have a translation path from the src
		   to this format, remove it from the result.  Note that x_bits
		   cannot be less than 0 as x will always have one bit set to 1 */
		x_bits = powerof(x);
		if (!tr_matrix[src_audio][x_bits].step) {
			res &= ~x;
			continue;
		}

		/* now check the opposite direction */
		if (!tr_matrix[x_bits][src_audio].step)
			res &= ~x;
	}

	/* For a given source video format, traverse the list of
	   known video formats to determine whether there exists
	   a translation path from the source format to the
	   destination format. */
	for (x = 1LL; src_video && x > 0; x <<= 1) {
		if (!(x & AST_FORMAT_VIDEO_MASK)) {
			continue;
		}

		/* if this is not a desired format, nothing to do */
		if (!(dest & x))
			continue;

		/* if the source is supplying this format, then
		   we can leave it in the result */
		if (src & x)
			continue;

		/* if we don't have a translation path from the src
		   to this format, remove it from the result.  Note that x_bits
		   cannot be less than 0 as x will always have one bit set to 1 */
		x_bits = powerof(x);
		if (!tr_matrix[src_video][x_bits].step) {
			res &= ~x;
			continue;
		}

		/* now check the opposite direction */
		if (!tr_matrix[x_bits][src_video].step)
			res &= ~x;
	}

	AST_RWLIST_UNLOCK(&translators);

	return res;
}
