/*
 * Copyright 2004-2006 Jeff Garzik <jgarzik@pobox.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */


#ifndef HAVE_CONFIG_H
#error missing autoconf-generated config.h.
#endif
#include "posixutils-config.h"

#ifndef _GNU_SOURCE
#define _GNU_SOURCE		/* for strcasestr(3), fgets_unlocked(3) */
#endif

#define __STDC_FORMAT_MACROS 1

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <limits.h>
#include <regex.h>
#include <libpu.h>


static const char doc[] =
N_("grep - print lines matching a pattern");

static const char args_doc[] = N_("[pattern_list] [file...]");

static struct argp_option options[] = {
	{ "extended-regexp", 'E', NULL, 0,
	  N_("Match using extended regular expressions") },
	{ "fixed-strings", 'F', NULL, 0,
	  N_("Match using fixed strings") },
	{ "count", 'c', NULL, 0,
	  N_("Write only a count of selected lines to standard output") },
	{ "regexp", 'e', "pattern_list", 0,
	  N_("Specify one or more patterns to be used during the search for input") },
	{ "file", 'f', "pattern_file", 0,
	  N_("Read one or more patterns from the file named by the pathname pattern_file") },
	{ "ignore-case", 'i', NULL, 0,
	  N_("Perform pattern matching in searches without regard to case") },
	{ "files-with-matches", 'l', NULL, 0,
	  N_("Write only the names of files containing selected lines to standard output") },
	{ "line-number", 'n', NULL, 0,
	  N_("Precede each output line by its relative line number in the file") },
	{ "quiet", 'q', NULL, 0,
	  N_("Quiet. Nothing shall be written to the standard output, regardless of matching lines. Exit with zero status if an input line is selected.") },
	{ "silent", 0, NULL, OPTION_ALIAS },
	{ "no-messages", 's', NULL, 0,
	  N_("Suppress the error messages ordinarily written for nonexistent or unreadable files") },
	{ "invert-match", 'v', NULL, 0,
	  N_("Select lines not matching any of the specified patterns") },
	{ "line-regexp", 'x', NULL, 0,
	  N_("Consider only input lines that use all characters in the line, excluding the terminating <newline>, to match an entire fixed string or regular expression to be matching lines") },
	{ }
};

static int grep_init(struct walker *w, int argc, char **argv);
static int grep_pre_walk(struct walker *w);
static int grep_post_walk(struct walker *w);
static int grep_file(struct walker *w, const char *pr_fn, FILE *f);
static error_t parse_opt (int key, char *arg, struct argp_state *state);
static const struct argp argp = { options, parse_opt, args_doc, doc };

static struct walker walker;

enum match_type {
	MATCH_BRE,		/* match basic regex */
	MATCH_ERE,		/* match extended regex */
	MATCH_STRING,		/* match bytestring rather than regex */
};

enum output_type {
	OUT_NONE,		/* no output */
	OUT_CONTENT,		/* output [non-]matching lines */
	OUT_PATH,		/* output a pathname */
	OUT_COUNT,		/* output a count of [non-]matching lines */
};

enum output_options {
	OUT_LINENO		= (1 << 0),	/* number output lines */
	OUT_SUPPRESS_ERRMSG	= (1 << 1),	/* suppress err msgs */
};

enum {
	INIT_PATTERNS		= 16,		/* pattern array start size */

	STOP_LOOP		= (1 << 30),
};

struct pattern {
	regex_t			rx;
	const char		*pat_str;
};


/* command line options */
static int opt_match = MATCH_BRE;
static int opt_output = OUT_CONTENT;
static int output_opts;
static bool opt_ignore_case;
static bool opt_match_invert;
static bool opt_match_line;

/* pattern array */
static struct pattern *patterns;
static int n_patterns;
static int alloc_patterns;

/* input buffer */
static char linebuf[LINE_MAX + 1];

/* various per-file or per-invocation counters */
static uintmax_t n_matches;
static uintmax_t total_matches;
static uintmax_t n_lines;
static int n_files;


static void compile_patterns(void)
{
	struct pattern *pat;
	int rc, i, flags;

	flags = REG_NOSUB;
	if (opt_ignore_case)
		flags |= REG_ICASE;
	if (opt_match == MATCH_ERE)
		flags |= REG_EXTENDED;

	for (i = 0; i < n_patterns; i++) {
		const char *pat_str;

		pat = &patterns[i];
		pat_str = pat->pat_str;

		/* if match-full-line, add ^ and $ to string */
		if (opt_match_line) {
			int need_pre, need_suff;
			size_t len = strlen(pat_str);

			need_pre = (pat_str[0] != '^');
			if (!len)
				need_suff = 1;
			else
				need_suff = (pat_str[len - 1] != '$');

			/* replace pattern with new string */
			if (need_pre || need_suff) {
				char *s = xmalloc(len + 3);
				sprintf(s, "%s%s%s",
					need_pre ? "^" : "",
					pat_str,
					need_suff ? "$" : "");
				pat_str = pat->pat_str = s;
			}
		}

		/* compile pattern */
		rc = regcomp(&pat->rx, pat_str, flags);
		if (rc) {
			regerror(rc, &pat->rx, linebuf, sizeof(linebuf));
			fprintf(stderr, _("invalid pattern '%s': %s\n"),
				pat_str, linebuf);
			exit(2);
		}
	}
}

static void add_pattern(const char *pat_str)
{
	struct pattern *pat;

	/* grow pattern array if necessary */
	if (alloc_patterns == n_patterns) {
		if (alloc_patterns == 0)
			alloc_patterns = INIT_PATTERNS;
		else
			alloc_patterns <<= 1;

		patterns = xrealloc(patterns,
				    alloc_patterns * sizeof(struct pattern));
	}

	/* add pattern to array */
	pat = &patterns[n_patterns];
	pat->pat_str = pat_str;

	n_patterns++;
}

static void add_patterns(const char *pattern_list_in)
{
	char *pattern_list = xstrdup(pattern_list_in);
	size_t len = strlen(pattern_list_in);

	while (len > 0) {
		char *nl;
		int i;

		/* search for newline */
		nl = memchr(pattern_list, '\n', len);

		/* if no newline, this is last pattern in list */
		if (!nl) {
			add_pattern(pattern_list);
			break;
		}

		/* null-terminate string at newline, add as pattern */
		*nl = 0;
		i = nl - pattern_list;
		add_pattern(pattern_list);

		/* advance to next pattern, if any */
		i++;	/* include newline now */
		pattern_list += i;
		len -= i;
	}
}

static void add_pattern_file(const char *fn)
{
	FILE *f;
	char *s;
	size_t len;

	if (ro_file_open(&f, fn))
		goto err_out_exit;

	setlinebuf(f);

	while ((s = fgets_unlocked(linebuf, sizeof(linebuf), f)) != NULL) {
		len = strlen(s);
		if (s[len - 1] == '\n') {
			len--;
			s[len] = 0;
		}

		add_pattern(xstrdup(s));
	}

	if (ferror(f))
		perror(fn);
	if (fclose(f))
		goto err_out;

	return;

err_out:
	perror(fn);
err_out_exit:
	exit(2);
}

static error_t parse_opt (int key, char *arg, struct argp_state *state)
{
	PU_OPT_BEGIN
	PU_OPT_SET('i', ignore_case)
	PU_OPT_SET('v', match_invert)
	PU_OPT_SET('x', match_line)

	case 'E':
		opt_match = MATCH_ERE;
		break;
	case 'F':
		opt_match = MATCH_STRING;
		break;
	case 'c':
		opt_output = OUT_COUNT;
		break;
	case 'l':
		opt_output = OUT_PATH;
		break;
	case 'n':
		output_opts |= OUT_LINENO;
		break;
	case 'q':
		opt_output = OUT_NONE;
		break;
	case 's':
		output_opts |= OUT_SUPPRESS_ERRMSG;
		break;

	case 'e':
		add_patterns(arg);
		break;
	case 'f':
		add_pattern_file(arg);
		break;

	PU_OPT_ARG
	PU_OPT_DEFAULT
	PU_OPT_END
}

static int match_string(const char *line)
{
	int i;

	/* attempt to match entire line */
	if (opt_match_line)
		for (i = 0; i < n_patterns; i++) {
			int rc;
			if (opt_ignore_case)
				rc = strcasecmp(line, patterns[i].pat_str);
			else
				rc = strcmp(line, patterns[i].pat_str);
			if (rc == 0)
				return 1;
		}

	/* attach to match substring within line */
	else
		for (i = 0; i < n_patterns; i++) {
			char *s;
			if (opt_ignore_case)
				 s = strcasestr(line, patterns[i].pat_str);
			else
				 s = strstr(line, patterns[i].pat_str);
			if (s)
				return 1;
		}

	return 0;
}

static int match_regex(const char *line)
{
	int i;

	for (i = 0; i < n_patterns; i++)
		if (regexec(&patterns[i].rx, line, 0, NULL, 0) == 0)
			return 1;

	return 0;
}

static int grep_line(const char *fn, const char *line)
{
	int match;
	char numbuf[36];

	if (opt_match == MATCH_STRING)
		match = match_string(line);
	else
		match = match_regex(line);
	if (opt_match_invert)
		match = !match;

	n_lines++;
	if (match)
		n_matches++;

	switch (opt_output) {
	case OUT_NONE:
		return match ? STOP_LOOP : 0;
	case OUT_PATH:
		if (match) {
			printf("%s\n", fn);
			return STOP_LOOP;
		}
		/* fall through */
	case OUT_COUNT:
		return 0;
	case OUT_CONTENT:
		if (!match)
			return 0;
		break;
	}

	if (output_opts & OUT_LINENO)
		sprintf(numbuf, "%" PRIuMAX ":", n_lines);
	else
		numbuf[0] = 0;

	if (n_files > 1)
		printf("%s:%s%s\n", fn, numbuf, line);
	else
		printf("%s%s\n", numbuf, line);

	return 0;
}

static int grep_file(struct walker *w, const char *pr_fn, FILE *f)
{
	n_lines = 0;
	n_matches = 0;

	setlinebuf(f);

	while (1) {
		char *line;
		size_t len;
		int ret;

		line = fgets_unlocked(linebuf, sizeof(linebuf), f);
		if (!line) {
			if (ferror(f))
				w->exit_status = 2;
			break;
		}

		len = strlen(line);
		if (line[len - 1] == '\n')
			line[len - 1] = 0;

		ret = grep_line(pr_fn, line);
		if (ret & STOP_LOOP)
			break;
	}

	if (opt_output == OUT_COUNT)
		printf("%s%s%" PRIuMAX "\n",
		       n_files > 1 ? pr_fn : "",
		       n_files > 1 ? ":" : "",
		       n_matches);

	total_matches += n_matches;

	return 0;
}

static int grep_init(struct walker *w, int argc, char **argv)
{
	pu_init();

	/*
	 * support 'fgrep', 'egrep' program names
	 */
	struct pathelem *pe = path_split(argv[0]);
	if (!strcmp(pe->basen, "egrep"))
		opt_match = MATCH_ERE;
	else if (!strcmp(pe->basen, "fgrep"))
		opt_match = MATCH_STRING;
	path_free(pe);

	return 0;
}

static int grep_pre_walk(struct walker *w)
{
	if (!n_patterns) {
		char *s = slist_shift(&w->strlist);
		if (s)
			add_pattern(s);
	}

	if (!n_patterns) {
		fprintf(stderr, _("no patterns specified"));
		return 2;
	}

	if (opt_match != MATCH_STRING)
		compile_patterns();

	n_files = w->strlist.len;

	return 0;
}

static int grep_post_walk(struct walker *w)
{
	/* if -q, there were errors, AND there was a match, return 0 */
	if (total_matches && opt_output == OUT_NONE)
		return 0;	/* match */
	if (w->exit_status > 1)
		return w->exit_status; /* error */
	if (total_matches)
		return 0;	/* match */
	return 1;		/* no match */
}

int main (int argc, char *argv[])
{
	walker.argp			= &argp;
	walker.flags			= WF_NO_FILES_STDIN;
	walker.init			= grep_init;
	walker.pre_walk			= grep_pre_walk;
	walker.post_walk		= grep_post_walk;
	walker.cmdline_file		= grep_file;
	return walk(&walker, argc, argv);
}
