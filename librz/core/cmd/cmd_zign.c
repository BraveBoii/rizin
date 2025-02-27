// SPDX-FileCopyrightText: 2009-2020 pancake <pancake@nopcode.org>
// SPDX-FileCopyrightText: 2009-2020 nibble <nibble.ds@gmail.com>
// SPDX-License-Identifier: LGPL-3.0-only

#include <rz_core.h>
#include <rz_analysis.h>
#include <rz_sign.h>
#include <rz_list.h>
#include <rz_cons.h>
#include <rz_util.h>
#include <rz_flirt.h>

#include "../core_private.h"

#define ZB_DEFAULT_N 5

static void addFcnZign(RzCore *core, RzAnalysisFunction *fcn, const char *name) {
	char *ptr = NULL;
	char *zignspace = NULL;
	char *zigname = NULL;
	const RzSpace *curspace = rz_spaces_current(&core->analysis->zign_spaces);
	int len = 0;

	if (name) {
		zigname = rz_str_new(name);
	} else {
		// If the user has set funtion names containing a single ':' then we assume
		// ZIGNSPACE:FUNCTION, and for now we only support the 'zg' command
		if ((ptr = strchr(fcn->name, ':')) != NULL) {
			len = ptr - fcn->name;
			zignspace = rz_str_newlen(fcn->name, len);
			rz_spaces_push(&core->analysis->zign_spaces, zignspace);
		} else if (curspace) {
			zigname = rz_str_newf("%s:", curspace->name);
		}
		zigname = rz_str_appendf(zigname, "%s", fcn->name);
	}

	// create empty item
	RzSignItem *it = rz_sign_item_new();
	if (!it) {
		free(zigname);
		return;
	}
	// add sig types info to item
	it->name = zigname; // will be free'd when item is free'd
	it->space = rz_spaces_current(&core->analysis->zign_spaces);
	rz_sign_addto_item(core->analysis, it, fcn, RZ_SIGN_GRAPH);
	rz_sign_addto_item(core->analysis, it, fcn, RZ_SIGN_BYTES);
	rz_sign_addto_item(core->analysis, it, fcn, RZ_SIGN_XREFS);
	rz_sign_addto_item(core->analysis, it, fcn, RZ_SIGN_REFS);
	rz_sign_addto_item(core->analysis, it, fcn, RZ_SIGN_VARS);
	rz_sign_addto_item(core->analysis, it, fcn, RZ_SIGN_TYPES);
	rz_sign_addto_item(core->analysis, it, fcn, RZ_SIGN_BBHASH);
	rz_sign_addto_item(core->analysis, it, fcn, RZ_SIGN_OFFSET);
	rz_sign_addto_item(core->analysis, it, fcn, RZ_SIGN_NAME);

	/* rz_sign_add_addr (core->analysis, zigname, fcn->addr); */

	// commit the item to anal
	rz_sign_add_item(core->analysis, it);

	/*
	XXX this is very slow and poorly tested
	char *comments = getFcnComments (core, fcn);
	if (comments) {
		rz_sign_add_comment (core->analysis, zigname, comments);
	}
	*/

	rz_sign_item_free(it); // causes zigname to be free'd
	if (zignspace) {
		rz_spaces_pop(&core->analysis->zign_spaces);
		free(zignspace);
	}
}

static bool addCommentZign(RzCore *core, const char *name, RzList *args) {
	if (rz_list_length(args) != 1) {
		eprintf("Invalid number of arguments\n");
		return false;
	}
	const char *comment = (const char *)rz_list_get_top(args);
	return rz_sign_add_comment(core->analysis, name, comment);
}

static bool addNameZign(RzCore *core, const char *name, RzList *args) {
	if (rz_list_length(args) != 1) {
		eprintf("Invalid number of arguments\n");
		return false;
	}
	const char *realname = (const char *)rz_list_get_top(args);
	return rz_sign_add_name(core->analysis, name, realname);
}

static bool addGraphZign(RzCore *core, const char *name, RzList *args) {
	RzSignGraph graph = { .cc = -1, .nbbs = -1, .edges = -1, .ebbs = -1, .bbsum = 0 };

	char *ptr;
	RzListIter *iter;
	rz_list_foreach (args, iter, ptr) {
		if (rz_str_startswith(ptr, "cc=")) {
			graph.cc = atoi(ptr + 3);
		} else if (rz_str_startswith(ptr, "nbbs=")) {
			graph.nbbs = atoi(ptr + 5);
		} else if (rz_str_startswith(ptr, "edges=")) {
			graph.edges = atoi(ptr + 6);
		} else if (rz_str_startswith(ptr, "ebbs=")) {
			graph.ebbs = atoi(ptr + 5);
		} else if (rz_str_startswith(ptr, "bbsum=")) {
			graph.bbsum = atoi(ptr + 6);
		} else {
			return false;
		}
	}
	return rz_sign_add_graph(core->analysis, name, graph);
}

static bool addHashZign(RzCore *core, const char *name, int type, RzList *args) {
	if (rz_list_length(args) != 1) {
		eprintf("error: invalid syntax\n");
		return false;
	}
	const char *hash = (const char *)rz_list_get_top(args);
	int len = strlen(hash);
	if (!len) {
		return false;
	}
	return rz_sign_add_hash(core->analysis, name, type, hash, len);
}

static bool addBytesZign(RzCore *core, const char *name, int type, RzList *args) {
	ut8 *mask = NULL, *bytes = NULL, *sep = NULL;
	int size = 0;
	bool retval = true;

	if (rz_list_length(args) != 1) {
		eprintf("error: invalid syntax\n");
		return false;
	}

	const char *hexbytes = (const char *)rz_list_get_top(args);
	if ((sep = (ut8 *)strchr(hexbytes, ':'))) {
		size_t blen = sep - (ut8 *)hexbytes;
		sep++;
		if (!blen || (blen & 1) || strlen((char *)sep) != blen) {
			eprintf("error: cannot parse hexpairs\n");
			return false;
		}
		bytes = calloc(1, blen + 1);
		mask = calloc(1, blen + 1);
		memcpy(bytes, hexbytes, blen);
		memcpy(mask, sep, blen);
		size = rz_hex_str2bin((char *)bytes, bytes);
		if (size != blen / 2 || rz_hex_str2bin((char *)mask, mask) != size) {
			eprintf("error: cannot parse hexpairs\n");
			retval = false;
			goto out;
		}
	} else {
		size_t blen = strlen(hexbytes) + 4;
		bytes = malloc(blen);
		mask = malloc(blen);

		size = rz_hex_str2binmask(hexbytes, bytes, mask);
		if (size <= 0) {
			eprintf("error: cannot parse hexpairs\n");
			retval = false;
			goto out;
		}
	}

	switch (type) {
	case RZ_SIGN_BYTES:
		retval = rz_sign_add_bytes(core->analysis, name, size, bytes, mask);
		break;
	case RZ_SIGN_ANALYSIS:
		retval = rz_sign_add_analysis(core->analysis, name, size, bytes, 0);
		break;
	}

out:
	free(bytes);
	free(mask);

	return retval;
}

static bool addOffsetZign(RzCore *core, const char *name, RzList *args) {
	if (rz_list_length(args) != 1) {
		eprintf("error: invalid syntax\n");
		return false;
	}
	const char *offstr = (const char *)rz_list_get_top(args);
	if (!offstr) {
		return false;
	}
	ut64 offset = rz_num_get(core->num, offstr);
	return rz_sign_add_addr(core->analysis, name, offset);
}

static bool addZign(RzCore *core, const char *name, int type, RzList *args) {
	switch (type) {
	case RZ_SIGN_BYTES:
	case RZ_SIGN_ANALYSIS:
		return addBytesZign(core, name, type, args);
	case RZ_SIGN_GRAPH:
		return addGraphZign(core, name, args);
	case RZ_SIGN_COMMENT:
		return addCommentZign(core, name, args);
	case RZ_SIGN_NAME:
		return addNameZign(core, name, args);
	case RZ_SIGN_OFFSET:
		return addOffsetZign(core, name, args);
	case RZ_SIGN_REFS:
		return rz_sign_add_refs(core->analysis, name, args);
	case RZ_SIGN_XREFS:
		return rz_sign_add_xrefs(core->analysis, name, args);
	case RZ_SIGN_VARS:
		return rz_sign_add_vars(core->analysis, name, args);
	case RZ_SIGN_TYPES:
		return rz_sign_add_types(core->analysis, name, args);
	case RZ_SIGN_BBHASH:
		return addHashZign(core, name, type, args);
	default:
		eprintf("error: unknown zignature type\n");
	}

	return false;
}

static RzCmdStatus print_zign_spaces(RzCore *core, RzSpaces *zs, RzOutputMode mode) {
	RzCmdStateOutput state;
	if (!rz_cmd_state_output_init(&state, mode)) {
		return RZ_CMD_STATUS_INVALID;
	}
	rz_core_spaces_print(core, zs, &state);
	rz_cmd_state_output_fini(&state);
	return RZ_CMD_STATUS_OK;
}

struct ctxSearchCB {
	RzCore *core;
	bool rad;
	int count;
	const char *prefix;
};

static void apply_name(RzCore *core, RzAnalysisFunction *fcn, RzSignItem *it, bool rad) {
	rz_return_if_fail(core && fcn && it && it->name);
	const char *name = it->realname ? it->realname : it->name;
	if (rad) {
		char *tmp = rz_name_filter2(name, true);
		if (tmp) {
			rz_cons_printf("\"afn %s @ 0x%08" PFMT64x "\"\n", tmp, fcn->addr);
			free(tmp);
		}
		return;
	}
	RzFlagItem *flag = rz_flag_get(core->flags, fcn->name);
	if (flag && flag->space && strcmp(flag->space->name, RZ_FLAGS_FS_FUNCTIONS)) {
		rz_flag_rename(core->flags, flag, name);
	}
	rz_analysis_function_rename(fcn, name);
	if (core->analysis->cb.on_fcn_rename) {
		core->analysis->cb.on_fcn_rename(core->analysis, core, fcn, name);
	}
}

static void apply_types(RzCore *core, RzAnalysisFunction *fcn, RzSignItem *it) {
	rz_return_if_fail(core && fcn && it && it->name);
	if (!it->types) {
		return;
	}
	const char *name = it->realname ? it->realname : it->name;
	RzListIter *iter;
	char *type;
	char *start = rz_str_newf("func.%s.", name);
	size_t startlen = strlen(start);
	char *alltypes = NULL;
	rz_list_foreach (it->types, iter, type) {
		if (strncmp(start, type, startlen)) {
			eprintf("Unexpected type: %s\n", type);
			free(alltypes);
			free(start);
			return;
		}
		if (!(alltypes = rz_str_appendf(alltypes, "%s\n", type))) {
			free(alltypes);
			free(start);
			return;
		}
	}
	rz_str_remove_char(alltypes, '"');
	rz_type_db_load_callables_sdb_str(core->analysis->typedb, alltypes);
	free(start);
	free(alltypes);
}

static void apply_flag(RzCore *core, RzSignItem *it, ut64 addr, int size, int count, const char *prefix, bool rad) {
	const char *zign_prefix = rz_config_get(core->config, "zign.prefix");
	char *name = rz_str_newf("%s.%s.%s_%d", zign_prefix, prefix, it->name, count);
	if (name) {
		if (rad) {
			char *tmp = rz_name_filter2(name, true);
			if (tmp) {
				rz_cons_printf("f %s %d @ 0x%08" PFMT64x "\n", tmp, size, addr);
				free(tmp);
			}
		} else {
			rz_flag_set(core->flags, name, addr, size);
		}
		free(name);
	}
}

static const char *getprefix(RzSignType t) {
	switch (t) {
	case RZ_SIGN_BYTES:
		return "bytes";
	case RZ_SIGN_GRAPH:
		return "graph";
	case RZ_SIGN_OFFSET:
		return "offset";
	case RZ_SIGN_REFS:
		return "refs";
	case RZ_SIGN_TYPES:
		return "types";
	case RZ_SIGN_BBHASH:
		return "bbhash";
	default:
		rz_return_val_if_reached("unkown_type");
	}
}

static int searchHitCB(RzSignItem *it, RzSearchKeyword *kw, ut64 addr, void *user) {
	struct ctxSearchCB *ctx = (struct ctxSearchCB *)user;
	apply_flag(ctx->core, it, addr, kw->keyword_length, kw->count, ctx->prefix, ctx->rad);
	RzAnalysisFunction *fcn = rz_analysis_get_fcn_in(ctx->core->analysis, addr, 0);
	// TODO: create fcn if it does not exist
	if (fcn) {
		apply_name(ctx->core, fcn, it, ctx->rad);
		apply_types(ctx->core, fcn, it);
	}
	ctx->count++;
	return 1;
}

static int fcnMatchCB(RzSignItem *it, RzAnalysisFunction *fcn, RzSignType type, bool seen, void *user) {
	struct ctxSearchCB *ctx = (struct ctxSearchCB *)user;
	const char *prefix = getprefix(type);
	ut64 sz = rz_analysis_function_realsize(fcn);
	apply_flag(ctx->core, it, fcn->addr, sz, ctx->count, prefix, ctx->rad);
	if (!seen) {
		apply_name(ctx->core, fcn, it, ctx->rad);
		apply_types(ctx->core, fcn, it);
		ctx->count++;
	}
	return 1;
}

static bool searchRange(RzCore *core, ut64 from, ut64 to, bool rad, struct ctxSearchCB *ctx) {
	ut8 *buf = malloc(core->blocksize);
	ut64 at;
	int rlen;
	bool retval = true;
	int minsz = rz_config_get_i(core->config, "zign.minsz");

	if (!buf) {
		return false;
	}
	RzSignSearch *ss = rz_sign_search_new();
	ss->search->align = rz_config_get_i(core->config, "search.align");
	rz_sign_search_init(core->analysis, ss, minsz, searchHitCB, ctx);

	rz_cons_break_push(NULL, NULL);
	for (at = from; at < to; at += core->blocksize) {
		if (rz_cons_is_breaked()) {
			retval = false;
			break;
		}
		rlen = RZ_MIN(core->blocksize, to - at);
		if (!rz_io_is_valid_offset(core->io, at, 0)) {
			retval = false;
			break;
		}
		(void)rz_io_read_at(core->io, at, buf, rlen);
		if (rz_sign_search_update(core->analysis, ss, &at, buf, rlen) == -1) {
			eprintf("search: update read error at 0x%08" PFMT64x "\n", at);
			retval = false;
			break;
		}
	}
	rz_cons_break_pop();
	free(buf);
	rz_sign_search_free(ss);

	return retval;
}

static bool searchRange2(RzCore *core, RzSignSearch *ss, ut64 from, ut64 to, bool rad, struct ctxSearchCB *ctx) {
	ut8 *buf = malloc(core->blocksize);
	ut64 at;
	int rlen;
	bool retval = true;

	if (!buf) {
		return false;
	}
	rz_cons_break_push(NULL, NULL);
	for (at = from; at < to; at += core->blocksize) {
		if (rz_cons_is_breaked()) {
			retval = false;
			break;
		}
		rlen = RZ_MIN(core->blocksize, to - at);
		if (!rz_io_is_valid_offset(core->io, at, 0)) {
			retval = false;
			break;
		}
		(void)rz_io_read_at(core->io, at, buf, rlen);
		if (rz_sign_search_update(core->analysis, ss, &at, buf, rlen) == -1) {
			eprintf("search: update read error at 0x%08" PFMT64x "\n", at);
			retval = false;
			break;
		}
	}
	rz_cons_break_pop();
	free(buf);

	return retval;
}

static void search_add_to_types(RzCore *c, RzSignSearchMetrics *sm, RzSignType t, const char *str, unsigned int *i) {
	unsigned int count = *i;
	rz_return_if_fail(count < sizeof(sm->types) / sizeof(RzSignType) - 1);
	if (rz_config_get_i(c->config, str)) {
		sm->types[count++] = t;
		sm->types[count] = 0;
		*i = count;
	}
}

static bool fill_search_metrics(RzSignSearchMetrics *sm, RzCore *c, void *user) {
	unsigned int i = 0;
	search_add_to_types(c, sm, RZ_SIGN_GRAPH, "zign.match.graph", &i);
	search_add_to_types(c, sm, RZ_SIGN_OFFSET, "zign.match.offset", &i);
	search_add_to_types(c, sm, RZ_SIGN_REFS, "zign.match.refs", &i);
	search_add_to_types(c, sm, RZ_SIGN_BBHASH, "zign.match.hash", &i);
	search_add_to_types(c, sm, RZ_SIGN_TYPES, "zign.match.types", &i);
#if 0
	// untested
	search_add_to_types(c, sm, RZ_SIGN_VARS, "zign.match.vars", &i);
#endif
	sm->mincc = rz_config_get_i(c->config, "zign.mincc");
	sm->analysis = c->analysis;
	sm->cb = fcnMatchCB;
	sm->user = user;
	sm->fcn = NULL;
	return (i > 0);
}

static bool search(RzCore *core, bool rad, bool only_func) {
	RzList *list;
	RzListIter *iter;
	RzAnalysisFunction *fcni = NULL;
	RzIOMap *map;
	bool retval = true;
	int hits = 0;

	struct ctxSearchCB bytes_search_ctx = { core, rad, 0, "bytes" };
	const char *mode = rz_config_get(core->config, "search.in");
	bool useBytes = rz_config_get_i(core->config, "zign.match.bytes");
	const char *zign_prefix = rz_config_get(core->config, "zign.prefix");
	int maxsz = rz_config_get_i(core->config, "zign.maxsz");

	struct ctxSearchCB metsearch_ctx = { core, rad, 0, NULL };
	RzSignSearchMetrics sm;
	bool metsearch = fill_search_metrics(&sm, core, (void *)&metsearch_ctx);

	if (rad) {
		rz_cons_printf("fss+ %s\n", zign_prefix);
	} else {
		if (!rz_flag_space_push(core->flags, zign_prefix)) {
			eprintf("error: cannot create flagspace\n");
			return false;
		}
	}

	// Bytes search
	if (useBytes && !only_func) {
		list = rz_core_get_boundaries_prot(core, -1, mode, "search");
		if (!list) {
			return false;
		}
		rz_list_foreach (list, iter, map) {
			eprintf("[+] searching 0x%08" PFMT64x " - 0x%08" PFMT64x "\n", map->itv.addr, rz_itv_end(map->itv));
			retval &= searchRange(core, map->itv.addr, rz_itv_end(map->itv), rad, &bytes_search_ctx);
		}
		rz_list_free(list);
	}

	// Function search
	if (metsearch) {
		eprintf("[+] searching function metrics\n");
		rz_cons_break_push(NULL, NULL);
		int count = 0;

		RzSignSearch *ss = NULL;

		if (useBytes && only_func) {
			ss = rz_sign_search_new();
			ss->search->align = rz_config_get_i(core->config, "search.align");
			int minsz = rz_config_get_i(core->config, "zign.minsz");
			rz_sign_search_init(core->analysis, ss, minsz, searchHitCB, &bytes_search_ctx);
		}

		rz_list_foreach (core->analysis->fcns, iter, fcni) {
			if (rz_cons_is_breaked()) {
				break;
			}
			if (useBytes && only_func) {
				eprintf("Matching func %d / %d (hits %d)\n", count, rz_list_length(core->analysis->fcns), bytes_search_ctx.count);
				int fcnlen = rz_analysis_function_realsize(fcni);
				int len = RZ_MIN(core->io->addrbytes * fcnlen, maxsz);
				retval &= searchRange2(core, ss, fcni->addr, fcni->addr + len, rad, &bytes_search_ctx);
			}
			sm.fcn = fcni;
			hits += rz_sign_fcn_match_metrics(&sm);
			sm.fcn = NULL;
			count++;
			// TODO: add useXRefs, useName
		}
		rz_cons_break_pop();
		rz_sign_search_free(ss);
	}

	if (rad) {
		rz_cons_printf("fs-\n");
	} else {
		if (!rz_flag_space_pop(core->flags)) {
			eprintf("error: cannot restore flagspace\n");
			return false;
		}
	}

	hits += bytes_search_ctx.count;
	eprintf("hits: %d\n", hits);

	return retval;
}

static void print_possible_matches(RzList *list) {
	RzListIter *itr;
	RzSignCloseMatch *row;
	rz_list_foreach (list, itr, row) {
		// total score
		if (row->bscore > 0.0 && row->gscore > 0.0) {
			rz_cons_printf("%02.5lf  ", row->score);
		}
		if (row->bscore > 0.0) {
			rz_cons_printf("%02.5lf B  ", row->bscore);
		}
		if (row->gscore > 0.0) {
			rz_cons_printf("%02.5lf G  ", row->gscore);
		}
		rz_cons_printf(" %s\n", row->item->name);
	}
}

static RzSignItem *item_frm_signame(RzAnalysis *a, const char *signame) {
	// example zign|*|sym.unlink_blk
	const RzSpace *space = rz_spaces_current(&a->zign_spaces);
	char *k = rz_str_newf("zign|%s|%s", space ? space->name : "*", signame);
	char *value = sdb_querys(a->sdb_zigns, NULL, 0, k);
	if (!value) {
		free(k);
		return NULL;
	}

	RzSignItem *it = rz_sign_item_new();
	if (!it) {
		free(k);
		free(value);
		return NULL;
	}

	if (!rz_sign_deserialize(a, it, k, value)) {
		rz_sign_item_free(it);
		it = NULL;
	}
	free(k);
	free(value);
	return it;
}

static double get_zb_threshold(RzCore *core) {
	const char *th = rz_config_get(core->config, "zign.threshold");
	double thresh = rz_num_get_float(NULL, th);
	if (thresh < 0.0 || thresh > 1.0) {
		eprintf("Invalid zign.threshold %s, using 0.0\n", th);
		thresh = 0.0;
	}
	return thresh;
}

static bool do_bestmatch_fcn(RzCore *core, const char *zigname, int count) {
	rz_return_val_if_fail(core, false);
	RzSignItem *it = item_frm_signame(core->analysis, zigname);
	if (!it) {
		eprintf("Couldn't get signature for %s\n", zigname);
		return false;
	}

	if (!rz_config_get_i(core->config, "zign.match.bytes")) {
		rz_sign_bytes_free(it->bytes);
		it->bytes = NULL;
	}
	if (!rz_config_get_i(core->config, "zign.match.graph")) {
		rz_sign_graph_free(it->graph);
		it->graph = NULL;
	}

	double thresh = get_zb_threshold(core);
	RzList *list = rz_sign_find_closest_fcn(core->analysis, it, count, thresh);
	rz_sign_item_free(it);

	if (list) {
		print_possible_matches(list);
		rz_list_free(list);
		return true;
	}
	return false;
}

static bool do_bestmatch_sig(RzCore *core, int count) {
	RzAnalysisFunction *fcn = rz_analysis_get_fcn_in(core->analysis, core->offset, 0);
	if (!fcn) {
		eprintf("No function at 0x%08" PFMT64x "\n", core->offset);
		return false;
	}

	RzSignItem *item = rz_sign_item_new();
	if (!item) {
		return false;
	}

	if (rz_config_get_i(core->config, "zign.match.bytes")) {
		rz_sign_addto_item(core->analysis, item, fcn, RZ_SIGN_BYTES);
		RzSignBytes *b = item->bytes;
		int minsz = rz_config_get_i(core->config, "zign.minsz");
		if (b && b->size < minsz) {
			eprintf("Warning: Function signature is too small (%d < %d) See e zign.minsz", b->size, minsz);
			rz_sign_item_free(item);
			return false;
		}
	}
	if (rz_config_get_i(core->config, "zign.match.graph")) {
		rz_sign_addto_item(core->analysis, item, fcn, RZ_SIGN_GRAPH);
	}

	double th = get_zb_threshold(core);
	bool found = false;
	if (item->graph || item->bytes) {
		rz_cons_break_push(NULL, NULL);
		RzList *list = rz_sign_find_closest_sig(core->analysis, item, count, th);
		if (list) {
			found = true;
			print_possible_matches(list);
			rz_list_free(list);
		}
		rz_cons_break_pop();
	} else {
		eprintf("Warning: no signatures types available for testing\n");
	}

	rz_sign_item_free(item);
	return found;
}

static int cmdCheck(void *data, const char *input) {
	RzCore *core = (RzCore *)data;
	RzSignSearch *ss;
	RzListIter *iter;
	RzAnalysisFunction *fcni = NULL;
	ut64 at = core->offset;
	bool retval = true;
	bool rad = input[0] == '*';
	int hits = 0;

	struct ctxSearchCB bytes_search_ctx = { core, rad, 0, "bytes" };

	const char *zign_prefix = rz_config_get(core->config, "zign.prefix");
	int minsz = rz_config_get_i(core->config, "zign.minsz");
	bool useBytes = rz_config_get_i(core->config, "zign.match.bytes");

	struct ctxSearchCB metsearch_ctx = { core, rad, 0, NULL };
	RzSignSearchMetrics sm;
	bool metsearch = fill_search_metrics(&sm, core, (void *)&metsearch_ctx);

	if (rad) {
		rz_cons_printf("fss+ %s\n", zign_prefix);
	} else {
		if (!rz_flag_space_push(core->flags, zign_prefix)) {
			eprintf("error: cannot create flagspace\n");
			return false;
		}
	}

	// Bytes search
	if (useBytes) {
		eprintf("[+] searching 0x%08" PFMT64x " - 0x%08" PFMT64x "\n", at, at + core->blocksize);
		ss = rz_sign_search_new();
		rz_sign_search_init(core->analysis, ss, minsz, searchHitCB, &bytes_search_ctx);
		if (rz_sign_search_update(core->analysis, ss, &at, core->block, core->blocksize) == -1) {
			eprintf("search: update read error at 0x%08" PFMT64x "\n", at);
			retval = false;
		}
		rz_sign_search_free(ss);
	}

	// Function search
	if (metsearch) {
		eprintf("[+] searching function metrics\n");
		rz_cons_break_push(NULL, NULL);
		rz_list_foreach (core->analysis->fcns, iter, fcni) {
			if (rz_cons_is_breaked()) {
				break;
			}
			if (fcni->addr == core->offset) {
				sm.fcn = fcni;
				hits += rz_sign_fcn_match_metrics(&sm);
				break;
			}
		}
		rz_cons_break_pop();
	}

	if (rad) {
		rz_cons_printf("fs-\n");
	} else {
		if (!rz_flag_space_pop(core->flags)) {
			eprintf("error: cannot restore flagspace\n");
			return false;
		}
	}

	hits += bytes_search_ctx.count;
	eprintf("hits: %d\n", hits);

	return retval;
}

RZ_IPI RzCmdStatus rz_zign_show_handler(RzCore *core, int argc, const char **argv, RzOutputMode mode) {
	char *out;
	RzStrBuf sb;
	if (mode != RZ_OUTPUT_MODE_SDB) {
		rz_strbuf_init(&sb);
	}
	switch (mode) {
	case RZ_OUTPUT_MODE_STANDARD:
		rz_sign_list(core->analysis, &sb, '\0');
		break;
	case RZ_OUTPUT_MODE_QUIET:
		rz_sign_list(core->analysis, &sb, 'q');
		break;
	case RZ_OUTPUT_MODE_JSON:
		rz_sign_list(core->analysis, &sb, 'j');
		break;
	case RZ_OUTPUT_MODE_RIZIN:
		rz_sign_list(core->analysis, &sb, '*');
		break;
	case RZ_OUTPUT_MODE_SDB:
		out = sdb_querys(core->sdb, NULL, 0, "analysis/zigns/*");
		if (!out) {
			return RZ_CMD_STATUS_ERROR;
		}
		rz_cons_print(out);
		free(out);
		return RZ_CMD_STATUS_OK;
	default:
		return RZ_CMD_STATUS_ERROR;
	}
	rz_cons_print(rz_strbuf_get(&sb));
	rz_strbuf_fini(&sb);
	return RZ_CMD_STATUS_OK;
}

RZ_IPI RzCmdStatus rz_zign_find_handler(RzCore *core, int argc, const char **argv, RzOutputMode mode) {
	return cmdCheck(core, mode == RZ_OUTPUT_MODE_RIZIN ? "*" : "") ? RZ_CMD_STATUS_OK : RZ_CMD_STATUS_ERROR;
}

RZ_IPI RzCmdStatus rz_zign_best_handler(RzCore *core, int argc, const char **argv) {
	int count = ZB_DEFAULT_N;
	if (argc > 1) {
		count = rz_num_math(core->num, argv[1]);
		if (count <= 0) {
			eprintf("Invalid count: %s\n", argv[1]);
			return RZ_CMD_STATUS_ERROR;
		}
	}
	return do_bestmatch_sig(core, count) ? RZ_CMD_STATUS_OK : RZ_CMD_STATUS_ERROR;
}

RZ_IPI RzCmdStatus rz_zign_best_name_handler(RzCore *core, int argc, const char **argv) {
	const char *zigname = argv[1];
	int count = ZB_DEFAULT_N;
	if (argc > 2) {
		count = rz_num_math(core->num, argv[2]);
		if (count <= 0) {
			eprintf("Invalid count: %s\n", argv[2]);
			return RZ_CMD_STATUS_ERROR;
		}
	}
	return do_bestmatch_fcn(core, zigname, count) ? RZ_CMD_STATUS_OK : RZ_CMD_STATUS_ERROR;
}

RZ_IPI RzCmdStatus rz_zign_delete_handler(RzCore *core, int argc, const char **argv) {
	rz_sign_delete(core->analysis, argv[1]);
	return RZ_CMD_STATUS_OK;
}

RZ_IPI RzCmdStatus rz_zign_add_handler(RzCore *core, int argc, const char **argv) {
	const char *zigname = argv[1];
	if (strlen(argv[2]) != 1) {
		return RZ_CMD_STATUS_WRONG_ARGS;
	}
	int type = argv[2][0];
	RzList *args = rz_list_new_from_array((const void **)argv + 3, argc - 3);
	bool res = addZign(core, zigname, type, args);
	rz_list_free(args);
	return res ? RZ_CMD_STATUS_OK : RZ_CMD_STATUS_ERROR;
}

RZ_IPI RzCmdStatus rz_zign_add_fcn_handler(RzCore *core, int argc, const char **argv) {
	RzAnalysisFunction *fcni = NULL;
	RzListIter *iter = NULL;
	const char *fcnname = argc > 1 ? argv[1] : NULL;
	const char *zigname = argc > 2 ? argv[2] : NULL;
	rz_cons_break_push(NULL, NULL);
	rz_list_foreach (core->analysis->fcns, iter, fcni) {
		if (rz_cons_is_breaked()) {
			break;
		}
		if ((!fcnname && core->offset == fcni->addr) ||
			(fcnname && !strcmp(fcnname, fcni->name))) {
			addFcnZign(core, fcni, zigname);
			break;
		}
	}
	rz_cons_break_pop();
	return RZ_CMD_STATUS_OK;
}

RZ_IPI RzCmdStatus rz_zign_add_all_fcns_handler(RzCore *core, int argc, const char **argv) {
	RzAnalysisFunction *fcni = NULL;
	RzListIter *iter = NULL;
	int count = 0;
	rz_cons_break_push(NULL, NULL);
	rz_list_foreach (core->analysis->fcns, iter, fcni) {
		if (rz_cons_is_breaked()) {
			break;
		}
		addFcnZign(core, fcni, NULL);
		count++;
	}
	rz_cons_break_pop();
	eprintf("generated zignatures: %d\n", count);
	return RZ_CMD_STATUS_OK;
}

RZ_IPI RzCmdStatus rz_zign_generate_handler(RzCore *core, int argc, const char **argv) {
	return rz_zign_add_all_fcns_handler(core, argc, argv);
}

RZ_IPI RzCmdStatus rz_zign_load_sdb_handler(RzCore *core, int argc, const char **argv) {
	return rz_sign_load(core->analysis, argv[1]) ? RZ_CMD_STATUS_OK : RZ_CMD_STATUS_ERROR;
}

RZ_IPI RzCmdStatus rz_zign_load_gzip_sdb_handler(RzCore *core, int argc, const char **argv) {
	return rz_sign_load_gz(core->analysis, argv[1]) ? RZ_CMD_STATUS_OK : RZ_CMD_STATUS_ERROR;
}

RZ_IPI RzCmdStatus rz_zign_save_sdb_handler(RzCore *core, int argc, const char **argv) {
	return rz_sign_save(core->analysis, argv[1]) ? RZ_CMD_STATUS_OK : RZ_CMD_STATUS_ERROR;
}

RZ_IPI RzCmdStatus rz_zign_flirt_create_handler(RzCore *core, int argc, const char **argv) {
	const char *filename = argv[1];
	ut32 written_nodes = 0;
	if (!rz_core_flirt_create_file(core, filename, &written_nodes)) {
		RZ_LOG_ERROR("failed to create FLIRT file '%s'\n", filename);
		return RZ_CMD_STATUS_ERROR;
	}
	rz_cons_printf("%u FLIRT signatures were written in '%s'\n", written_nodes, filename);
	return RZ_CMD_STATUS_OK;
}

RZ_IPI RzCmdStatus rz_zign_flirt_dump_handler(RzCore *core, int argc, const char **argv) {
	rz_core_flirt_dump_file(argv[1]);
	return RZ_CMD_STATUS_OK;
}

RZ_IPI RzCmdStatus rz_zign_flirt_scan_handler(RzCore *core, int argc, const char **argv) {
	int new, old;
	int depth = rz_config_get_i(core->config, "dir.depth");
	const char *arch = rz_config_get(core->config, "asm.arch");
	char *file = NULL;
	RzListIter *iter = NULL;
	RzList *files = rz_file_globsearch(argv[1], depth);
	ut8 arch_id = rz_core_flirt_arch_from_name(arch);

	old = rz_flag_count(core->flags, "flirt");
	rz_list_foreach (files, iter, file) {
		rz_sign_flirt_apply(core->analysis, file, arch_id);
	}
	rz_list_free(files);
	new = rz_flag_count(core->flags, "flirt");

	rz_cons_printf("Found %d FLIRT signatures via %s\n", new - old, argv[1]);
	return RZ_CMD_STATUS_OK;
}

RZ_IPI RzCmdStatus rz_zign_search_handler(RzCore *core, int argc, const char **argv, RzOutputMode mode) {
	return search(core, mode == RZ_OUTPUT_MODE_RIZIN, false) ? RZ_CMD_STATUS_OK : RZ_CMD_STATUS_ERROR;
}

RZ_IPI RzCmdStatus rz_zign_search_fcn_handler(RzCore *core, int argc, const char **argv, RzOutputMode mode) {
	return search(core, mode == RZ_OUTPUT_MODE_RIZIN, true) ? RZ_CMD_STATUS_OK : RZ_CMD_STATUS_ERROR;
}

RZ_IPI RzCmdStatus rz_zign_cmp_handler(RzCore *core, int argc, const char **argv) {
	const char *raw_bytes_thresh = rz_config_get(core->config, "zign.diff.bthresh");
	const char *raw_graph_thresh = rz_config_get(core->config, "zign.diff.gthresh");
	RzSignOptions *options = rz_sign_options_new(raw_bytes_thresh, raw_graph_thresh);
	RzList *matches = rz_sign_diff(core->analysis, options, argv[1]);
	RzListIter *it;
	const char *match;
	rz_list_foreach (matches, it, match) {
		rz_cons_println(match);
	}
	rz_list_free(matches);
	rz_sign_options_free(options);
	return matches ? RZ_CMD_STATUS_OK : RZ_CMD_STATUS_ERROR;
}

static RzCmdStatus zcn_handler_common(RzCore *core, int argc, const char **argv, bool negative_match) {
	const char *raw_bytes_thresh = rz_config_get(core->config, "zign.diff.bthresh");
	const char *raw_graph_thresh = rz_config_get(core->config, "zign.diff.gthresh");
	RzSignOptions *options = rz_sign_options_new(raw_bytes_thresh, raw_graph_thresh);
	RzList *matches = rz_sign_diff_by_name(core->analysis, options, argv[1], negative_match);
	RzListIter *it;
	const char *match;
	rz_list_foreach (matches, it, match) {
		rz_cons_println(match);
	}
	rz_list_free(matches);
	rz_sign_options_free(options);
	return matches ? RZ_CMD_STATUS_OK : RZ_CMD_STATUS_ERROR;
}

RZ_IPI RzCmdStatus rz_zign_cmp_name_handler(RzCore *core, int argc, const char **argv) {
	return zcn_handler_common(core, argc, argv, false);
}

RZ_IPI RzCmdStatus rz_zign_cmp_diff_name_handler(RzCore *core, int argc, const char **argv) {
	return zcn_handler_common(core, argc, argv, true);
}

RZ_IPI RzCmdStatus rz_zign_space_select_handler(RzCore *core, int argc, const char **argv, RzOutputMode mode) {
	if (argc == 1) {
		return print_zign_spaces(core, &core->analysis->zign_spaces, mode);
	} else {
		rz_spaces_set(&core->analysis->zign_spaces, argv[1]);
	}
	return RZ_CMD_STATUS_OK;
}

RZ_IPI RzCmdStatus rz_zign_space_delete_handler(RzCore *core, int argc, const char **argv) {
	if (argc == 1) {
		rz_spaces_pop(&core->analysis->zign_spaces);
		return RZ_CMD_STATUS_OK;
	}
	if (!strcmp(argv[1], "*")) {
		rz_spaces_unset(&core->analysis->zign_spaces, NULL);
	} else {
		rz_spaces_unset(&core->analysis->zign_spaces, argv[1]);
	}
	return RZ_CMD_STATUS_OK;
}

RZ_IPI RzCmdStatus rz_zign_space_add_handler(RzCore *core, int argc, const char **argv) {
	rz_spaces_push(&core->analysis->zign_spaces, argv[1]);
	return RZ_CMD_STATUS_OK;
}

RZ_IPI RzCmdStatus rz_zign_space_rename_handler(RzCore *core, int argc, const char **argv) {
	rz_spaces_rename(&core->analysis->zign_spaces, NULL, argv[1]);
	return RZ_CMD_STATUS_OK;
}

RZ_IPI RzCmdStatus rz_zign_info_handler(RzCore *core, int argc, const char **argv, RzCmdStateOutput *state) {
	rz_flag_space_push(core->flags, RZ_FLAGS_FS_SIGNS);
	rz_core_flag_print(core->flags, state);
	rz_flag_space_pop(core->flags);
	return RZ_CMD_STATUS_OK;
}

RZ_IPI RzCmdStatus rz_zign_info_range_handler(RzCore *core, int argc, const char **argv, RzCmdStateOutput *state) {
	ut64 range_from = rz_num_math(core->num, argv[1]);
	ut64 range_to = rz_num_math(core->num, argv[2]);
	rz_flag_space_push(core->flags, RZ_FLAGS_FS_SIGNS);
	rz_core_flag_range_print(core->flags, state, range_from, range_to);
	rz_flag_space_pop(core->flags);
	return RZ_CMD_STATUS_OK;
}
