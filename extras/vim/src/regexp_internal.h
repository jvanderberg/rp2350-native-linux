/* Internal interface shared by the Vim regexp parser and backtracking engine. */
#ifndef VIM_REGEXP_INTERNAL_H
#define VIM_REGEXP_INTERNAL_H

// By default: do not create debugging logs or files related to regular
// expressions, even when compiling with -DDEBUG.
// Uncomment the second line to get the regexp debugging.
#undef DEBUG
// #define DEBUG

#include "vim.h"

#ifdef DEBUG
// show/save debugging data when BT engine is used
# define BT_REGEXP_DUMP
// save the debugging data to a file instead of displaying it
# define BT_REGEXP_LOG
#endif

enum
{
    CLASS_ALNUM = 0,
    CLASS_ALPHA,
    CLASS_BLANK,
    CLASS_CNTRL,
    CLASS_DIGIT,
    CLASS_GRAPH,
    CLASS_LOWER,
    CLASS_PRINT,
    CLASS_PUNCT,
    CLASS_SPACE,
    CLASS_UPPER,
    CLASS_XDIGIT,
    CLASS_TAB,
    CLASS_RETURN,
    CLASS_BACKSPACE,
    CLASS_ESCAPE,
    CLASS_IDENT,
    CLASS_KEYWORD,
    CLASS_FNAME,
    CLASS_NONE = 99
};

typedef struct
{
    char_u	*regparse;
    int		prevchr_len;
    int		curchr;
    int		prevchr;
    int		prevprevchr;
    int		nextchr;
    int		at_start;
    int		prev_at_start;
    int		regnpar;
} parse_state_T;

/*
 * Structure used to store the execution state of the regex engine.
 * Which ones are set depends on whether a single-line or multi-line match is
 * done:
 *			single-line		multi-line
 * reg_match		&regmatch_T		NULL
 * reg_mmatch		NULL			&regmmatch_T
 * reg_startp		reg_match->startp	<invalid>
 * reg_endp		reg_match->endp		<invalid>
 * reg_startpos		<invalid>		reg_mmatch->startpos
 * reg_endpos		<invalid>		reg_mmatch->endpos
 * reg_win		NULL			window in which to search
 * reg_buf		curbuf			buffer in which to search
 * reg_firstlnum	<invalid>		first line in which to search
 * reg_maxline		0			last line nr
 * reg_line_lbr		FALSE or TRUE		FALSE
 */
typedef struct {
    regmatch_T		*reg_match;
    regmmatch_T		*reg_mmatch;

    char_u		**reg_startp;
    char_u		**reg_endp;
    lpos_T		*reg_startpos;
    lpos_T		*reg_endpos;

    win_T		*reg_win;
    buf_T		*reg_buf;
    linenr_T		reg_firstlnum;
    linenr_T		reg_maxline;
    int			reg_line_lbr;	// "\n" in string is line break

    // The current match-position is stord in these variables:
    linenr_T	lnum;		// line number, relative to first line
    char_u	*line;		// start of current line
    char_u	*input;		// current input, points into "line"

    int	need_clear_subexpr;	// subexpressions still need to be cleared
#ifdef FEAT_SYN_HL
    int	need_clear_zsubexpr;	// extmatch subexpressions still need to be
				// cleared
#endif

    // Internal copy of 'ignorecase'.  It is set at each call to vim_regexec().
    // Normally it gets the value of "rm_ic" or "rmm_ic", but when the pattern
    // contains '\c' or '\C' the value is overruled.
    int			reg_ic;

    // Similar to "reg_ic", but only for 'combining' characters.  Set with \Z
    // flag in the regexp.  Defaults to false, always.
    int			reg_icombine;

    // Copy of "rmm_maxcol": maximum column to search for a match.  Zero when
    // there is no maximum.
    colnr_T		reg_maxcol;
} regexec_T;

typedef void (*fptr_T)(int *, int);

#define Magic(x)	((int)(x) - 256)
#define is_Magic(x)	((x) < 0)
#define REGMAGIC	0234
#define EMSG_RET_NULL(m) return (emsg((m)), rc_did_emsg = TRUE, (void *)NULL)
#define IEMSG_RET_NULL(m) return (iemsg((m)), rc_did_emsg = TRUE, (void *)NULL)
#define EMSG2_RET_NULL(m, c) return (semsg((const char *)(m), (c) ? "" : "\\"), rc_did_emsg = TRUE, (void *)NULL)
#define EMSG3_RET_NULL(m, c, a) return (semsg((const char *)(m), (c) ? "" : "\\", (a)), rc_did_emsg = TRUE, (void *)NULL)
#define EMSG_ONE_RET_NULL EMSG2_RET_NULL(_(e_invalid_item_in_str_brackets), reg_magic == MAGIC_ALL)
#define MAX_LIMIT	(32767L << 16L)
#define NOT_MULTI	0
#define RA_FAIL		1	// something failed, abort
#define RA_CONT		2	// continue in inner loop
#define RA_BREAK	3	// break inner loop
#define RA_MATCH	4	// successful match
#define RA_NOMATCH	5	// didn't match
#define	    RI_DIGIT	0x01
#define	    RI_HEX	0x02
#define	    RI_OCTAL	0x04
#define	    RI_WORD	0x08
#define	    RI_HEAD	0x10
#define	    RI_ALPHA	0x20
#define	    RI_LOWER	0x40
#define	    RI_UPPER	0x80
#define	    RI_WHITE	0x100
#define ri_digit(c)	((c) < 0x100 && (class_tab[c] & RI_DIGIT))
#define ri_hex(c)	((c) < 0x100 && (class_tab[c] & RI_HEX))
#define ri_octal(c)	((c) < 0x100 && (class_tab[c] & RI_OCTAL))
#define ri_word(c)	((c) < 0x100 && (class_tab[c] & RI_WORD))
#define ri_head(c)	((c) < 0x100 && (class_tab[c] & RI_HEAD))
#define ri_alpha(c)	((c) < 0x100 && (class_tab[c] & RI_ALPHA))
#define ri_lower(c)	((c) < 0x100 && (class_tab[c] & RI_LOWER))
#define ri_upper(c)	((c) < 0x100 && (class_tab[c] & RI_UPPER))
#define RF_ICASE    1	// ignore case
#define RF_NOICASE  2	// don't ignore case
#define RF_HASNL    4	// can match a NL
#define RF_ICOMBINE 8	// ignore combining characters
#define RF_LOOKBH   16	// uses "\@<=" or "\@<!"
#define REG_NOPAREN	0	// toplevel reg()
#define REG_PAREN	1	// \(\)
#define REG_ZPAREN	2	// \z(\)
#define REG_NPAREN	3	// \%(\)
// Limit recursive parsing of nested regexp atoms.
#define REG_MAX_PAREN_DEPTH	1000
#define REG_MULTI	(rex.reg_match == NULL)

#ifdef FEAT_RELTIME
extern volatile sig_atomic_t *timeout_flag;
#endif
extern char_u REGEXP_INRANGE[];
extern char_u REGEXP_ABBR[];
extern short	class_tab[256];
extern char_u	*regparse;
extern int	regnpar;
#ifdef FEAT_SYN_HL
extern int	regnzpar;
extern int	re_has_z;
extern int	had_eol;
#endif
extern unsigned	regflags;
extern magic_T	reg_magic;
extern int	reg_string;
extern int	reg_strict;
extern int	curchr;
extern int reg_cpo_lit;
extern int reg_cpo_bsl;
extern char_u	*reg_tofree;
extern unsigned	reg_tofreelen;
extern regexec_T	rex;
#ifdef FEAT_SYN_HL
extern char_u	*reg_startzp[NSUBEXP];
extern char_u	*reg_endzp[NSUBEXP];
extern lpos_T	reg_startzpos[NSUBEXP];
extern lpos_T	reg_endzpos[NSUBEXP];
#endif

int no_Magic(int x);
int re_multi_type(int c);
int backslash_trans(int c);
int get_char_class(char_u **pp);
void init_class_tab(void);
int get_equi_class(char_u **pp);
int get_coll_element(char_u **pp);
void get_cpo_flags(void);
char_u *skip_anyof(char_u *p);
void initchr(char_u *str);
int peekchr(void);
void skipchr(void);
void skipchr_keepstart(void);
int getchr(void);
void ungetchr(void);
vimlong_T gethexchrs(int maxinputlen);
vimlong_T getdecchrs(void);
long getoctchrs(void);
int read_limits(long *minval, long *maxval);
int reg_iswordc(int c);
char_u *reg_getline(linenr_T lnum);
colnr_T reg_getline_len(linenr_T lnum);
#ifdef FEAT_SYN_HL
reg_extmatch_T *make_extmatch(void);
#endif
int reg_prev_class(void);
int reg_match_visual(void);
int prog_magic_wrong(void);
void cleanup_subexpr(void);
#ifdef FEAT_SYN_HL
void cleanup_zsubexpr(void);
#endif
void reg_nextline(void);
int match_with_backref(linenr_T start_lnum, colnr_T start_col, linenr_T end_lnum, colnr_T end_col, int *bytelen);
int re_mult_next(char *what);
int cstrncmp(char_u *s1, char_u *s2, int *n);
char_u *cstrchr(char_u *s, int c);
extern char_u *reg_prev_sub;
extern int prev_at_start;
extern int at_start;
extern int prevchr_len;
void init_regexec_multi(regmmatch_T *rmp, win_T *win, buf_T *buf, linenr_T lnum);
int coll_get_char(void);
extern regengine_T bt_regengine;
#ifdef EXITFREE
void bt_regfree_workspaces(void);
#endif

#endif
