/* $Id$ */

/* Cexp interface to object/symbol file using BFD; runtime loader support */

/* NOTE: read the comment near the end of this file about C++ exception handling */

/* IMPORTANT LICENSING INFORMATION:
 *
 *  linking this code against 'libbfd'/ 'libopcodes'
 *  may be subject to the GPL conditions.
 *  (This file itself is distributed under the EPICS
 *  open license)
 *
 */

/* Author: Till Straumann, 8/2002 */

/*
 * Copyright 2002, Stanford University and
 * 		Till Straumann <strauman@slac.stanford.edu>
 * 
 * Stanford Notice
 * ***************
 * 
 * Acknowledgement of sponsorship
 * * * * * * * * * * * * * * * * *
 * This software was produced by the Stanford Linear Accelerator Center,
 * Stanford University, under Contract DE-AC03-76SFO0515 with the Department
 * of Energy.
 * 
 * Government disclaimer of liability
 * - - - - - - - - - - - - - - - - -
 * Neither the United States nor the United States Department of Energy,
 * nor any of their employees, makes any warranty, express or implied,
 * or assumes any legal liability or responsibility for the accuracy,
 * completeness, or usefulness of any data, apparatus, product, or process
 * disclosed, or represents that its use would not infringe privately
 * owned rights.
 * 
 * Stanford disclaimer of liability
 * - - - - - - - - - - - - - - - - -
 * Stanford University makes no representations or warranties, express or
 * implied, nor assumes any liability for the use of this software.
 * 
 * This product is subject to the EPICS open license
 * - - - - - - - - - - - - - - - - - - - - - - - - - 
 * Consult the LICENSE file or http://www.aps.anl.gov/epics/license/open.php
 * for more information.
 * 
 * Maintenance of notice
 * - - - - - - - - - - -
 * In the interest of clarity regarding the origin and status of this
 * software, Stanford University requests that any recipient of it maintain
 * this notice affixed to any distribution by the recipient that contains a
 * copy or derivative of this software.
 */

/*#include <libiberty.h>*/
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <fcntl.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#define _INSIDE_CEXP_
#include "cexp.h"
#include "cexpmodP.h"
#include "cexpsymsP.h"
#include "cexpHelp.h"

/* Oh well; rtems/score/ppctypes.h defines boolean and bfd
 * defines boolean as well :-( Can't you people use names
 * less prone to clashes???
 * We redefine bfd's boolean here
 */
#define  boolean bfdddd_bbboolean
#include <bfd.h>

#ifdef HAVE_ELF_BFD_H
#include "elf-bfd.h"
#endif

#define	DEBUG_COMMON	(1<<0)
#define DEBUG_SECT		(1<<1)
#define DEBUG_RELOC		(1<<2)
#define DEBUG_SYM		(1<<3)

#define	DEBUG			0

#include "spencer_regexp.h"

/* magic symbol names for C++ support; probably gcc specific */
#define CTOR_DTOR_PATTERN		"^_+GLOBAL_[_.$][ID][_.$]"
#ifdef OBSOLETE_EH_STUFF	/* old suselinux-ppc modified gcc needed that */
#define EH_FRAME_BEGIN_PATTERN	"__FRAME_BEGIN__"
#define EH_FRAME_END_PATTERN	"__FRAME_END__"
#endif
#define EH_SECTION_NAME			".eh_frame"
#define TEXT_SECTION_NAME		".text"

/* using one static variable for the pattern matcher is
 * OK since the entire cexpLoadFile() routine is called
 * with the WRITE_LOCK held, so mutex is guaranteed for
 * the ctorCtorRegexp.
 */
static  spencer_regexp	*ctorDtorRegexp=0;

#ifdef HAVE_BFD_DISASSEMBLER
/* as a side effect, this code gives access to a disassembler which
 * itself is implemented by libopcodes
 */
extern void	cexpDisassemblerInstall(bfd *abfd);
#else
#define		cexpDisassemblerInstall(a) do {} while(0)
#endif

/* this is PowerPC specific; note that some architectures
 * (mpc860) have smaller cache lines. Setting this to a smaller
 * value than the actual cache line size is safe and performance
 * is not an issue here
 */
#define CACHE_LINE_SIZE 16

/* an output segment description */
typedef struct SegmentRec_ {
	PTR				chunk;		/* pointer to memory */
	unsigned long	vmacalc;	/* working counter */
	unsigned long	size;
	unsigned		attributes; /* such as 'read-only' etc.; currently unused */
} SegmentRec, *Segment;

/* currently, the cexp module facility only supports a single segment */
#define NUM_SEGS 1
/* a paranoid index for the only segment */
#define ONLY_SEG (assert(NUM_SEGS==1),0)

/* data we need for linking */
typedef struct LinkDataRec_ {
	bfd				*abfd;	
	SegmentRec		segs[NUM_SEGS];
	asymbol			**st;
	asymbol			***new_commons;
	CexpSymTbl		cst;
	int				errors;
	int				num_alloc_sections;
	BitmapWord		*depend;
	int				nCtors;
	int				nDtors;
#ifdef OBSOLETE_EH_STUFF
	asymbol			*eh_frame_b_sym;
	asymbol			*eh_frame_e_sym;
#else
	asection		*eh_section;
#endif
	unsigned long	text_vma;
	asection		*text;
	asection		*eh;
	void			*iniCallback;
	void			*finiCallback;
} LinkDataRec, *LinkData;

/* forward declarations */

/* Hmm - c++ exception handling is _highly_ compiler and version and 
 * target dependent :-(.
 *
 * It is very unlikely that exception handling works with anything other
 * than gcc.
 *
 * Gcc (2.95.3) seems to implement (at least) two flavours of exception
 * handling: longjump and exception frame tables. The latter requires
 * the dynamic loader to load and register the exception frame table, the
 * former is transparent.
 * Unfortunately, the __register_frame()/__deregister_frame() routines
 * are absent on targets which do not support this kind of exception handling.
 * 
 * Therefore, we use the CEXP symbol table itself to retrieve the actual
 * routines __register_frame() and __deregister_frame(). Our private pointers
 * are initialized when we read the system symbol table.
 */
static void (*my__register_frame)(void*)=0;
static void (*my__deregister_frame)(void*)=0;

static asymbol *
asymFromCexpSym(bfd *abfd, CexpSym csym, BitmapWord *depend, CexpModule mod);

#ifdef __rtems__
static FILE *
copyFileToTmp(int fd, char *tmpfname);
#endif

static void
bfdCleanupCallback(CexpModule);

/* how to decide where a particular section should go */
static Segment
segOf(LinkData ld, asection *sect)
{
	/* multiple sections not supported (yet) */
	return &ld->segs[ONLY_SEG];
}

/* determine the alignment power of a common symbol
 * (currently only works for ELF)
 */
#ifdef HAVE_ELF_BFD_H
static __inline__ int
get_align_pwr(bfd *abfd, asymbol *sp)
{
register unsigned long rval=0,tst;
elf_symbol_type *esp;
	if (esp=elf_symbol_from(abfd, sp))
		for (tst=1; tst<esp->internal_elf_sym.st_size; rval++)
			tst<<=1;
	return rval;
}

/* we sort in descending order; hence the routine must return b-a */
static int
align_size_compare(const void *a, const void *b)
{
elf_symbol_type *espa, *espb;
asymbol			*spa=**(asymbol***)a;
asymbol			*spb=**(asymbol***)b;

	return
		((espa=elf_symbol_from(bfd_asymbol_bfd(spa),spa)) &&
	     (espb=elf_symbol_from(bfd_asymbol_bfd(spb),spb)))
		? (espb->internal_elf_sym.st_size - espa->internal_elf_sym.st_size)
		: 0;
}
#else
#define get_align_pwr(abfd,sp)	0
#define align_size_compare		0
#endif

/* determine if a given symbol is a constructor or destructor 
 * by matching to a magic pattern.
 *
 * RETURNS: 1 if the symbol is a constructor
 *         -1 if it is a destructor
 *          0 otherwise
 */
static __inline__ int
isCtorDtor(asymbol *asym, int quiet)
{
	/* From bfd/linker.c: */
	/* "A constructor or destructor name starts like this:
	   _+GLOBAL_[_.$][ID][_.$] where the first [_.$] and
	   the second are the same character (we accept any
	   character there, in case a new object file format
	   comes along with even worse naming restrictions)."  */

	if (spencer_regexec(ctorDtorRegexp,bfd_asymbol_name(asym))) {
		switch (*(ctorDtorRegexp->endp[0]-2)) {
			case 'I':	return  1; /* constructor */
			case 'D':	return -1; /* destructor  */
			default:
				if (!quiet) {
					fprintf(stderr,"WARNING: unclassified BSF_CONSTRUCTOR symbol;");
					fprintf(stderr," ignoring '%s'\n",bfd_asymbol_name(asym));
				}
			break;
		}
	}
	return 0;
}

/* this routine defines which symbols are retained by Cexp in its
 * internal symbol table
 */
static const char *
filter(void *ext_sym, void *closure)
{
LinkData	ld=closure;
asymbol		*asym=*(asymbol**)ext_sym;

		if ( !(BSF_KEEP & asym->flags) )
			return 0;
		return BSF_SECTION_SYM & asym->flags ? 
					bfd_get_section_name(ld->abfd,bfd_get_section(asym)) :
					bfd_asymbol_name(asym);
}

/* this routine is responsible for converting external (BFD) symbols
 * to their internal representation (CexpSym). Only symbols who have
 * passed the filter() above will be seen by assign().
 */
static void
assign(void *ext_sym, CexpSym cesp, void *closure)
{
LinkData	ld=(LinkData)closure;
asymbol		*asym=*(asymbol**)ext_sym;
int			s;
CexpType	t=TVoid;
#ifdef HAVE_ELF_BFD_H
elf_symbol_type *elfsp=elf_symbol_from(ld->abfd, asym);
#define ELFSIZE(elfsp)	((elfsp)->internal_elf_sym.st_size)
#else
#define		elfsp 0
#define	ELFSIZE(elfsp)	(0)
#endif

#if DEBUG & DEBUG_SYM
		printf("assigning symbol: %s\n",bfd_asymbol_name(asym));
#endif


		s = elfsp ? ELFSIZE(elfsp) : 0;

		if (BSF_FUNCTION & asym->flags) {
			t=TFuncP;
			if (0==s)
				s=sizeof(void(*)());
		} else if (BSF_SECTION_SYM & asym->flags) {
			/* section name: leave it void* */
		} else {
			/* must be BSF_OBJECT */
			if (asym->flags & BSF_OLD_COMMON) {
				/* value holds the size */
				s = bfd_asymbol_value(asym);
			}
			if (CEXP_BASE_TYPE_SIZE(TUCharP) == s) {
				t=TUChar;
			} else if (CEXP_BASE_TYPE_SIZE(TUShortP) == s) {
				t=TUShort;
			} else if (CEXP_BASE_TYPE_SIZE(TULongP) == s) {
				t=TULong;
			} else if (CEXP_BASE_TYPE_SIZE(TDoubleP) == s) {
				t=TDouble;
			} else if (CEXP_BASE_TYPE_SIZE(TFloatP) == s) {
				/* if sizeof(float) == sizeof(long), long has preference */
				t=TFloat;
			} else {
				/* if it's bigger than double, leave it (void*) */
			}
		}
		/* last attempt: if there is no size info and no flag set,
		 * at least look at the section; functions are in the text
		 * section...
		 */
		if (TVoid == t &&
			! (asym->flags & (BSF_FUNCTION | BSF_OBJECT | BSF_SECTION_SYM)) &&
			0 == s) {
			if (ld->text && ld->text == bfd_get_section(asym))
				t=TFuncP;
		} 

		cesp->size = s;
		cesp->value.type = t;
		/* We can only set the real value after relocation.
		 * Therefore, store a pointer to the external symbol,
		 * so we have a handle even after the internal symbols
		 * are sorted. Note that the aindex[] built by cexpCreateSymTbl()
		 * will be incorrect and has to be re-sorted further 
		 * down the line.
		 */
		cesp->value.ptv  = ext_sym;

		if (asym->flags & BSF_GLOBAL)
			cesp->flags |= CEXP_SYMFLG_GLBL;
		if (asym->flags & BSF_WEAK)
			cesp->flags |= CEXP_SYMFLG_WEAK;

}

/* call this after relocation to assign the internal
 * symbol representation their values
 */
void
cexpSymTabSetValues(CexpSymTbl cst)
{
CexpSym	cesp;
	for (cesp=cst->syms; cesp->name; cesp++) {
		asymbol *sp=*(asymbol**)cesp->value.ptv;
		cesp->value.ptv=(CexpVal)bfd_asymbol_value(sp);
	}

	/* resort the address index */
	qsort((void*)cst->aindex,
			cst->nentries,
			sizeof(*cst->aindex),
			_cexp_addrcomp);
}

/* All 's_xxx()' routines defined here are for use with
 * bfd_map_over_sections()
 */

/* compute the size requirements for a section */
static void
s_count(bfd *abfd, asection *sect, PTR arg)
{
Segment		seg=segOf((LinkData)arg, sect);
flagword	flags=bfd_get_section_flags(abfd,sect);
const char	*secn=bfd_get_section_name(abfd,sect);
#if DEBUG & DEBUG_SECT
	printf("Section %s, flags 0x%08x\n", secn, flags);
	printf("size: %i, alignment %i\n",
			bfd_section_size(abfd,sect),
			(1<<bfd_section_alignment(abfd,sect)));
#endif
	if (SEC_ALLOC & flags) {
		seg->size+=(1<<bfd_get_section_alignment(abfd,sect))-1;
		seg->size+=bfd_section_size(abfd,sect);
		/* exception handler frame tables have to be 0 terminated and
		 * we must reserve a little extra space.
		 */
		if ( !strcmp(secn, EH_SECTION_NAME) ) {
#ifdef OBSOLETE_EH_STUFF
			asymbol *asym;
			/* allocate space for a terminating 0 */
			seg->size+=sizeof(long);
			/* create a symbol to tag the terminating 0 */
			asym=((LinkData)arg)->eh_frame_e_sym=bfd_make_empty_symbol(abfd);
			bfd_asymbol_name(asym) = EH_FRAME_END_PATTERN;
			asym->value=(symvalue)bfd_section_size(abfd,sect);
			asym->section=sect;
#else
			((LinkData)arg)->eh_section = sect;
#endif
		}
	}
}

/* compute the section's vma for loading */

static void
s_setvma(bfd *abfd, asection *sect, PTR arg)
{
Segment		seg=segOf((LinkData)arg, sect);
LinkData	ld=(LinkData)arg;

	if (SEC_ALLOC & sect->flags) {
		seg->vmacalc=align_power(seg->vmacalc, bfd_get_section_alignment(abfd,sect));
#if DEBUG & DEBUG_SECT
		printf("%s allocated at 0x%08x\n",
				bfd_get_section_name(abfd,sect),
				seg->vmacalc);
#endif
		bfd_set_section_vma(abfd,sect,seg->vmacalc);
		seg->vmacalc+=bfd_section_size(abfd,sect);
#ifdef OBSOLETE_EH_STUFF
		if ( ld->eh_frame_e_sym && bfd_get_section(ld->eh_frame_e_sym) == sect ) {
			/* allocate space for a terminating 0 */
			seg->vmacalc+=sizeof(long);
		}
#endif
		sect->output_section = sect;
		if (ld->text && sect == ld->text) {
			ld->text_vma=bfd_get_section_vma(abfd,sect);
		}
	}
}

/* find basic sections and the number of sections which are
 * actually allocated and resolve gnu.linkonce.xxx sections
 */
static void
s_basic(bfd *abfd, asection *sect, PTR arg)
{
LinkData	ld=(LinkData)arg;
flagword	flags=bfd_get_section_flags(ld->abfd, sect);
	if (!strcmp(TEXT_SECTION_NAME,bfd_get_section_name(abfd,sect))) {
		ld->text = sect;
	}
	if ( SEC_ALLOC & flags ) {
		if (SEC_LINK_ONCE & flags) {
			const char *secn=bfd_get_section_name(ld->abfd,sect);
			if (cexpSymLookup(secn, 0)) {
				/* a linkonce section with this name had been loaded already;
				 * discard this one...
				 */
				if ( DEBUG || (SEC_LINK_DUPLICATES & flags))
					fprintf(stderr,"Warning: section '%s' exists; discarding...\n", secn);
				/* discard this section */
				sect->flags &= ~SEC_ALLOC;
				return;
			}
		}
		ld->num_alloc_sections++;
	}
}

/* read the section contents and process the relocations.
 */
static void
s_reloc(bfd *abfd, asection *sect, PTR arg)
{
LinkData	ld=(LinkData)arg;
int			i;
long		err;

	if ( ! (SEC_ALLOC & sect->flags) )
		return;

	/* read section contents to its memory segment
	 * NOTE: this automatically clears section with
	 *       ALLOC set but with no CONTENTS (such as
	 *       bss)
	 */
	if (!bfd_get_section_contents(
				abfd,
				sect,
				(PTR)bfd_get_section_vma(abfd,sect),
				0,
				bfd_section_size(abfd,sect))) {
		bfd_perror("reading section contents");
		ld->errors++;
		return;
	}

	/* if there are relocations, resolve them */
	if ((SEC_RELOC & sect->flags)) {
		arelent **cr=0;
		long	sz;
		sz=bfd_get_reloc_upper_bound(abfd,sect);
		if (sz<=0) {
			fprintf(stderr,"No relocs for section %s???\n",
					bfd_get_section_name(abfd,sect));
			ld->errors++;
			return;
		}
		/* slurp the relocation records; build a list */
		cr=(arelent**)xmalloc(sz);
		sz=bfd_canonicalize_reloc(abfd,sect,cr,ld->st);
		if (sz<=0) {
			fprintf(stderr,"ERROR: unable to canonicalize relocs\n");
			free(cr);
			ld->errors++;
			return;
		}
		for (i=0; i<sz; i++) {
			arelent 	*r=cr[i];
			asection	*symsect=bfd_get_section(*r->sym_ptr_ptr);
			if (   bfd_is_und_section(symsect) ||					
				   /* reloc references an undefined sym */
				( !(SEC_ALLOC & bfd_get_section_flags(abfd,symsect)) &&
				   /* reloc references a dropped linkonce */
				  !( bfd_is_abs_section(symsect) && (ld->eh_section == sect) ) )
				   /* it does reference a dropped linkonce but in the eh_frame
					* this will be ignored by the frame unwinder (I hope - I did
					* some digging through libgcc (gcc-3.2) sources and it seemed
					* so althouth it's a very complicated issue)
					* NOTE: the converse case - i.e. if an eh_frame section in the
					*       object we are loading right now	references a 'linkonce'
                    *       section we have deleted - is probably OK. We relocate
                    *       the EH info to a PC range already in memory - this should
					*       be safe. I also found some comments in bfd/elflink and 
					*       libgcc which give me some confidence.
					*/
			    ) {
				CexpModule	mod;
				asymbol		*sp;
				CexpSym		ts=cexpSymLookup(bfd_asymbol_name((sp=*(r->sym_ptr_ptr))),&mod);
				if (ts) {
					/* Resolved reference; replace the symbol pointer
					 * in this slot with a new asymbol holding the
					 * resolved value.
					 */
					sp=*r->sym_ptr_ptr=asymFromCexpSym(abfd,ts,ld->depend,mod);
				} else {
					fprintf(stderr,"Unresolved symbol: %s\n",bfd_asymbol_name(sp));
					ld->errors++;
		continue;
				}
			}
#if DEBUG & DEBUG_RELOC
			printf("relocating (%s=",
					bfd_asymbol_name(*(r->sym_ptr_ptr))
					);
			printf("0x%08x)->0x%08x [sym_ptr_ptr = 0x%08x]\n",
			bfd_asymbol_value(*(r->sym_ptr_ptr)),
			r->address,
			r->sym_ptr_ptr);
#endif

			if ((err=bfd_perform_relocation(
				abfd,
				r,
				(PTR)bfd_get_section_vma(abfd,sect),
				sect,
				0 /* output bfd */,
				0))) {
				bfd_perror("Relocation failed");
				ld->errors++;
			}
		}
		free(cr);
	}
}

/* build the module's static constructor and
 * destructor lists.
 */
static void
buildCtorsDtors(LinkData ld, CexpModule mod)
{
asymbol		**asymp;

	if (ld->nCtors) {
		mod->ctor_list=(VoidFnPtr*)xmalloc(sizeof(*mod->ctor_list) * (ld->nCtors));
		memset(mod->ctor_list,0,sizeof(*mod->ctor_list) * (ld->nCtors));
	}
	if (ld->nDtors) {
		mod->dtor_list=(VoidFnPtr*)xmalloc(sizeof(*mod->dtor_list) * (ld->nDtors));
		memset(mod->dtor_list,0,sizeof(*mod->dtor_list) * (ld->nDtors));
	}
	for (asymp=ld->st; *asymp; asymp++) {
		switch (isCtorDtor(*asymp,0/*quiet*/)) {
			case 1:
				assert(mod->nCtors<ld->nCtors);
				mod->ctor_list[mod->nCtors++]=(VoidFnPtr)bfd_asymbol_value(*asymp);
			break;

			case -1:
				assert(mod->nDtors<ld->nDtors);
				mod->dtor_list[mod->nDtors++]=(VoidFnPtr)bfd_asymbol_value(*asymp);
			break;

			default:
			break;
		}
	}
}

/* convert a CexpSym to a (temporary) BFD symbol and update module
 * dependencies
 */
static asymbol *
asymFromCexpSym(bfd *abfd, CexpSym csym, BitmapWord *depend, CexpModule mod)
{
asymbol *sp = bfd_make_empty_symbol(abfd);
	/* TODO: check size and alignment */

	/* copy pointer to name */
	bfd_asymbol_name(sp) = csym->name;
	sp->value=(symvalue)csym->value.ptv;
	sp->section=bfd_abs_section_ptr;
	sp->flags=BSF_GLOBAL;
	/* mark the referenced module in the bitmap */
	assert(mod->id < MAX_NUM_MODULES);
	BITMAP_SET(depend,mod->id);
	return sp;
}

/* resolve undefined and common symbol references;
 * The routine also creates a list of
 * all common symbols that need to be created.
 *
 * RETURNS:
 *   value >= 0 : OK, number of new common symbols
 *   value <  0 : -number of errors (unresolved refs or multiple definitions)
 *
 * SIDE EFFECTS:
 *   - resolved undef or common slots are replaced by new symbols
 *     pointing to the ABS section
 *   - KEEP flag is set for all globals defined by this object.
 *   - KEEP flag is set for all section name symbols, except for
 *     non-ALLOCed linkonce sections (these had already been loaded
 *     as part of other modules).
 *   - raise a bit for each module referenced by this new one.
 */
static int
resolve_syms(bfd *abfd, asymbol **syms, LinkData ld)
{
asymbol		*sp;
int			i,num_new_commons=0,errs=0;

	/* resolve undefined and common symbols;
	 * find name clashes
	 */
	for (i=0; (sp=syms[i]); i++) {
		asection	*sect=bfd_get_section(sp);
		CexpSym		ts;
		int			res;
		CexpModule	mod;
		const char	*symname;

#if DEBUG  & DEBUG_SYM
		printf("scanning SYM (flags 0x%08x, value 0x%08x) '%s'\n",
						sp->flags,
						bfd_asymbol_value(sp),
						bfd_asymbol_name(sp));
		printf("         section vma 0x%08x in %s\n",
						bfd_get_section_vma(abfd,sect),
						bfd_get_section_name(abfd,sect));
#endif
		/* count constructors/destructors */
		if ((res=isCtorDtor(sp,1/*warn*/))) {
			if (res>0)
				ld->nCtors++;
			else
				ld->nDtors++;
		}

#ifdef OBSOLETE_EH_STUFF
		/* mark end with 32-bit 0 if this symbol is contained
 		 * in a ".eh_frame" section
		 */
		if (!strcmp(EH_FRAME_BEGIN_PATTERN, bfd_asymbol_name(sp))) {
			ld->eh_frame_b_sym=sp;

		/* check for magic initializer/finalizer symbols */
		} else
#endif
		if (!strcmp(CEXPMOD_INITIALIZER_SYM, bfd_asymbol_name(sp))) {

			/* set the local flag; we dont want these in our symbol table */
			sp->flags |= BSF_LOCAL;
			/* store a pointer to the internal symbol table slot; we lookup the
			 * value after relocation
			 */
			if (ld->iniCallback) {
				fprintf(stderr,
						"Only one '%s' may be present in a loaded object\n",
						CEXPMOD_INITIALIZER_SYM);
				errs++;
			} else {
				ld->iniCallback=syms+i;
			}

		} else if (!strcmp(CEXPMOD_FINALIZER_SYM, bfd_asymbol_name(sp))) {
			sp->flags |= BSF_LOCAL;
			if (ld->finiCallback) {
				fprintf(stderr,
						"Only one '%s' may be present in a loaded object\n",
						CEXPMOD_FINALIZER_SYM);
				errs++;
			} else {
				ld->finiCallback=syms+i;
			}
		}


		/* we only care about global symbols
		 * (NOTE: undefined symbols are neither local
		 *        nor global)
		 * The only exception are the name symbols of allocated sections
		 * - there may be quite a lot of them ("gnu.linkonce.x.yyy") in a
		 * large C++ object...
		 */

		if ( BSF_LOCAL & sp->flags ) {
			if ( BSF_SECTION_SYM & sp->flags ) {
				/* keep allocated section names in CEXP's symbol table so we can
				 * tell the debugger their load address.
				 *
				 * NOTE: The already loaded modules have been scanned for
				 *       gnu.linkonce.xxx sections in 's_basic()' where
				 *		 redundant occurrencies have been de-SEC_ALLOCed
				 *		 from this module.
				 */
				if (SEC_ALLOC & bfd_get_section_flags(abfd, sect)) {
						sp->flags|=BSF_KEEP;
				}
			}
			continue; /* proceed with the next symbol */
		}

		/* non-local, i.e. global or undefined */
		symname = bfd_asymbol_name(sp);

		ts=cexpSymLookup(symname, &mod);

		if (bfd_is_und_section(sect)) {
			/* do some magic on undefined symbols which do have a
			 * type. These are probably sitting in a shared
			 * library and _do_ have a valid value. (This is the
			 * case when loading the symbol table e.g. on a 
			 * linux platform.)
			 */
			if (!ts && bfd_asymbol_value(sp) && (BSF_FUNCTION & sp->flags)) {
					sp->flags |= BSF_KEEP;
			}
			/* resolve references at relocation time */
		}
		else if (bfd_is_com_section(sect)) {
			if (ts) {
				/* use existing value of common sym */
				sp = asymFromCexpSym(abfd,ts,ld->depend,mod);
			} else {
				/* it's the first definition of this common symbol */

				/* we'll have to add it to our internal symbol table */
				sp->flags |= BSF_KEEP|BSF_OLD_COMMON;

				/* 
				 * we must not change the order of the symbol list;
				 * _elf_bfd_canonicalize_reloc() relies on the order being
				 * preserved.
				 * Therefore, we create an array of pointers which
				 * we can sort in any desired way...
				 */
				ld->new_commons=(asymbol***)xrealloc(ld->new_commons,
													sizeof(*ld->new_commons)*(num_new_commons+1));
				ld->new_commons[num_new_commons] = syms+i;
				num_new_commons++;
			}
			syms[i]=sp; /* use new instance */
		} else {
			/* any other symbol, i.e. neither undefined nor common */
			if (ts) {
				if (sp->flags & BSF_WEAK) {
					/* use existing instance */
					sp=syms[i]=asymFromCexpSym(abfd,ts,ld->depend,mod);
				} else {
					fprintf(stderr,"Symbol '%s' already exists\n",symname);
					errs++;
					/* TODO: check size and alignment; allow multiple
					 *       definitions??? - If yes, we have to track
					 *		 the dependencies also.
					 */
					/* TODO: we should allow a new 'strong' symbol to
					 *       override an existing 'weak' one.
					 */
				}
			} else {
				/* new symbol defined by the loaded object; account for it */
				/* mark for second pass */
				sp->flags |= BSF_KEEP;
			}
		}
	}

	return errs ? -errs : num_new_commons;
}

/* make a dummy section holding the new common data objects introduced by
 * the newly loaded file.
 *
 * RETURNS: 0 on failure, nonzero on success
 */
static int
make_new_commons(bfd *abfd, asymbol ***new_common_ps, int num_new_commons)
{
unsigned long	i,val;
asection	*csect;

	if (num_new_commons) {
		/* make a dummy section for new common symbols */
		csect=bfd_make_section(abfd,bfd_get_unique_section_name(abfd,".dummy",0));
		if (!csect) {
			bfd_perror("Creating dummy section");
			return -1;
		}
		csect->flags |= SEC_ALLOC;

		/* our common section alignment is the maximal alignment
		 * found during the sorting process which is the alignment
		 * of the first element...
		 */
		bfd_section_alignment(abfd,csect) = get_align_pwr(abfd,*new_common_ps[0]);

		/* set new common symbol values */
		for (val=0,i=0; i<num_new_commons; i++) {
			asymbol *sp;
			int apwr;

			sp = bfd_make_empty_symbol(abfd);

			/* to minimize fragmentation, the new commons have been sorted
			 * in decreasing order according to their alignment requirements
			 * (probably not supported on formats other than ELF)
			 */
			val=align_power(val,(
							apwr=
							get_align_pwr(abfd,*new_common_ps[i])));
#if DEBUG & DEBUG_COMMON
			printf("New common: %s; align_pwr %i\n",bfd_asymbol_name(sp),apwr);
#endif
			/* copy pointer to old name */
			bfd_asymbol_name(sp) = bfd_asymbol_name(*new_common_ps[i]);
			sp->value=val;
			sp->section=csect;
			sp->flags=(*new_common_ps[i])->flags;
			val+=(*new_common_ps[i])->value;
			*new_common_ps[i] = sp;
		}
		
		bfd_set_section_size(abfd, csect, val);
	}
	return 0;
}

/* this routine does the following:
 *
 *  - build the external (BFD) symtab
 *  - append external symbols for the names of new
 *    (i.e. not already loaded) 'linkonce' sections
 *    to the BFD symtab.
 *  - resolve undefined and already loaded common symbols
 *    (i.e. substitute their asymbol by a new one using
 *    the known values). This is actually performed by
 *    resolve_syms().
 *  - create the internal representation (CexpSym) of
 *    the symbol table.
 *  - make newly introduced common symbols. This is done
 *    by first creating a new section, sorting the common
 *    syms by size in reverse order (for meeting alignment
 *    requirements) and generating clones for them 
 *    associated with the new section.
 *
 * NOTE: Loading the initial symbol table with this routine
 *       relies on the assumption that there are no
 *        COMMON symbols present.
 */

static int
slurp_symtab(bfd *abfd, LinkData ld)
{
asymbol 			**asyms=0;
long				i,nsyms;
long				num_new_commons;

	if (!(HAS_SYMS & bfd_get_file_flags(abfd))) {
		fprintf(stderr,"No symbols found\n");
		return -1;
	}
	if ((i=bfd_get_symtab_upper_bound(abfd))<0) {
		fprintf(stderr,"Fatal error: illegal symtab size\n");
		return -1;
	}

	/* Allocate space for the symbol table  */
	if (i) {
		asyms=(asymbol**)xmalloc((i) * sizeof(asymbol*));
	}
	nsyms= i ? i/sizeof(asymbol*) - 1 : 0;
	if (bfd_canonicalize_symtab(abfd,asyms) <= 0) {
		bfd_perror("Canonicalizing symtab");
		goto cleanup;
	}


	if ( (num_new_commons=resolve_syms(abfd,asyms,ld)) <0 ) {
		goto cleanup;
	}

	/*
	 *	sort the list of new_commons by alignment
	 */
	if (align_size_compare && num_new_commons)
		qsort(
			(void*)ld->new_commons,
			num_new_commons,
			sizeof(*ld->new_commons),
			align_size_compare);
	
	/* Now, everything is in place to build our internal symbol table
	 * representation.
	 * We cannot do this later, because the size information will be lost.
	 * However, we cannot provide the values yet; this can only be done
	 * after relocation has been performed.
	 */

	ld->cst=cexpCreateSymTbl(asyms,
							 sizeof(*asyms),
							 nsyms,
							 filter, assign, ld);

	if (0!=make_new_commons(abfd,ld->new_commons,num_new_commons))
		goto cleanup;

	ld->st=asyms;
	asyms=0;

cleanup:
	if (asyms) {
		free(asyms);
		return -1;
	}

	return 0;
}


static void
flushCache(LinkData ld)
{
#if defined(__PPC__) || defined(__PPC) || defined(_ARCH_PPC) || defined(PPC)
int	i;
char	*start, *end;
	for (i=0; i<NUM_SEGS; i++) {
		if (ld->segs[i].size) {
			start=(char*)ld->segs[i].chunk;
			/* align back to cache line */
			start-=(unsigned long)start % CACHE_LINE_SIZE;
			end  =(char*)ld->segs[i].chunk+ld->segs[i].size;

			for (; start<end; start+=CACHE_LINE_SIZE)
				__asm__ __volatile__(
					"dcbf 0, %0\n"	/* flush out one data cache line */
					"icbi 0, %0\n"	/* invalidate cached instructions for this line */
					::"r"(start));
		}
	}
	/* enforce flush completion and discard preloaded instructions */
	__asm__ __volatile__("sync; isync");
#endif
}


/* the caller of this routine holds the module lock */
int
cexpLoadFile(char *filename, CexpModule mod)
{
LinkDataRec		ldr;
int				rval=1,i;
CexpSym			sane;
FILE			*f=0;
void			*ehFrame=0;
#ifdef __rtems__
char			tmpfname[30]={
		'/','t','m','p','/','m','o','d','X','X','X','X','X','X',
		0};
int				is_on_tftp, tftp_fd;
struct stat		dummybuf;
#endif

	/* clear out the private data area; the cleanup code
	 * relies on this...
	 */
	memset(&ldr,0,sizeof(ldr));

	ldr.depend = mod->needs;

	/* basic check for our bitmaps */
	assert( (1<<LD_WORDLEN) <= sizeof(BitmapWord)*8 );

	if (!filename) {
		fprintf(stderr,"Need filename arg\n");
		goto cleanup;
	}

	bfd_init();
	/* lazy init of regexp pattern */
	if (!ctorDtorRegexp)
		ctorDtorRegexp=spencer_regcomp(CTOR_DTOR_PATTERN);

#if 0
	if ( ! (ldr.abfd=bfd_openr(filename,0)) )
	{
		bfd_perror("Opening object file");
		goto cleanup;
	}
#else
#ifdef __rtems__
	/* RTEMS has no NFS support (yet), and the TFTPfs is strictly
	 * sequential access (no lseek(), no stat()). Hence, we copy
	 * the file to scratch file in memory (IMFS).
	 */

	/* we assume that a file we cannot stat() but open() is on TFTPfs */
	is_on_tftp = stat(filename, &dummybuf) ? open(filename,O_RDONLY) : -1;
	if ( is_on_tftp >= 0 ) {
		if ( ! (f=copyFileToTmp(is_on_tftp, tmpfname)) ) {
			goto cleanup;
		}
		filename=tmpfname;
	} else
#endif
	if (  ! (f=fopen(filename,"r")) ) {
		perror("opening object file");
		goto cleanup;
	}
	if ( ! (ldr.abfd=bfd_openstreamr(filename,0,f)) ) {
		bfd_perror("Opening BFD on object file stream");
		goto cleanup;
	}
	/* ldr.abfd now holds the descriptor and bfd_close_all_done() will release it */
	f=0;
#endif
	if (!bfd_check_format(ldr.abfd, bfd_object)) {
		bfd_perror("Checking format");
		goto cleanup;
	}

	/* get basic section info and filter linkonce sections */
	bfd_map_over_sections(ldr.abfd, s_basic, &ldr);

	if (slurp_symtab(ldr.abfd,&ldr)) {
		fprintf(stderr,"Error creating symbol table\n");
		goto cleanup;
	}

	/* the first thing to be loaded must be the system
	 * symbol table. Reject to load anything in this case
	 * which allows for using the executable as the
	 * symbol file.
	 */
	if ( ! (0==cexpSystemModule) ) {

		/* compute the memory space requirements */
		bfd_map_over_sections(ldr.abfd, s_count, &ldr);
	
		/* allocate segment space */
		for (i=0; i<NUM_SEGS; i++) {
			ldr.segs[i].chunk=(PTR)xmalloc(ldr.segs[i].size);
			ldr.segs[i].vmacalc=(unsigned long)ldr.segs[i].chunk;
		}

		/* compute and set the base addresses for all sections to be allocated */
		bfd_map_over_sections(ldr.abfd, s_setvma, &ldr);

		ldr.errors=0;
memset(ldr.segs[ONLY_SEG].chunk,0xee,ldr.segs[ONLY_SEG].size); /*TSILL*/
		bfd_map_over_sections(ldr.abfd, s_reloc, &ldr);
		if (ldr.errors)
			goto cleanup;

	} else {
		/* it's the system symtab */
		assert( 0==ldr.new_commons );
	}

	cexpSymTabSetValues(ldr.cst);

	/* system symbol table sanity check */
	if ((sane=cexpSymTblLookup("cexpLoadFile",ldr.cst))) {
		/* this must be the system table */
		extern void *_edata, *_etext;

        /* it must be the main symbol table */
        if ( sane->value.ptv==(CexpVal)cexpLoadFile     &&
       	     (sane=cexpSymTblLookup("_etext",ldr.cst))  &&
			 sane->value.ptv==(CexpVal)&_etext          &&
             (sane=cexpSymTblLookup("_edata",ldr.cst))  &&
			 sane->value.ptv==(CexpVal)&_edata ) {

        	/* OK, sanity test passed */

			/* as a side effect, since we have an open BFD, we
			 * use it to fetch a disassembler for the target
			 */
			cexpDisassemblerInstall(ldr.abfd);

		} else {
			fprintf(stderr,"SANITY CHECK FAILED! Stale symbol file?\n");
			goto cleanup;
		}
	}

	/* FROM HERE ON, NO CLEANUP MUST BE DONE */


	/* the following steps are not needed when
	 * loading the system symbol table
	 */
	if (cexpSystemModule) {
		/* if there is an exception frame table, we
		 * have to register it with libgcc. If it was
		 * in its own section, we also must write a terminating 0
		 */
#ifdef OBSOLETE_EH_STUFF
		if (ldr.eh_frame_b_sym) {
			if ( ldr.eh_frame_e_sym ) {
				/* eh_frame was in its own section; hence we have to write a terminating 0
				 */
				*(long*)bfd_asymbol_value(ldr.eh_frame_e_sym)=0;
			}
			assert(my__register_frame);
			ehFrame=(void*)bfd_asymbol_value(ldr.eh_frame_b_sym);
printf("TSILL registering EH_FRAME 0x%08lx\n",  ehFrame);
			my__register_frame(ehFrame);
		}
#else
		if (ldr.eh_section) {
			assert(my__register_frame);
			ehFrame=(void*)bfd_get_section_vma(abfd,ldr.eh_section);
			my__register_frame(ehFrame);
		}
#endif
		/* build ctor/dtor lists */
		if (ldr.nCtors || ldr.nDtors) {
			buildCtorsDtors(&ldr,mod);
		}
	} else {
		/* initialize the my__[de]register_frame() pointers if possible */
		if ((sane=cexpSymTblLookup("__register_frame",ldr.cst)))
			my__register_frame=(void(*)(void*))sane->value.ptv;
		if ((sane=cexpSymTblLookup("__deregister_frame",ldr.cst)))
			my__deregister_frame=(void(*)(void*))sane->value.ptv;

	}

	flushCache(&ldr);

	/* move the relevant data over to the module */
	mod->memSeg  = ldr.segs[ONLY_SEG].chunk;
	mod->memSize = ldr.segs[ONLY_SEG].size;
	ldr.segs[ONLY_SEG].chunk=0;

	mod->symtbl  = ldr.cst;
	ldr.cst=0;

	if (ldr.iniCallback) {
		/* it's still an asymbol**; we now lookup the value */
		mod->iniCallback=(void(*)(CexpModule)) bfd_asymbol_value((*(asymbol**)ldr.iniCallback));
	}
	if (ldr.finiCallback) {
		mod->finiCallback=(int(*)(CexpModule)) bfd_asymbol_value((*(asymbol**)ldr.finiCallback));
	}

	/* add help strings to symbol table */
	{
	asymbol *sp;
	int		len=strlen(CEXP_HELP_TAB_NAME);
	for (i=0; (sp=ldr.st[i]); i++) {
		if (0==strncmp(CEXP_HELP_TAB_NAME,bfd_asymbol_name(sp),len)) {
			/* this module has a help table; add it to the symbol table */
			cexpAddHelpToSymTab((CexpHelpTab)bfd_asymbol_value(sp), mod->symtbl);
		}
	}
	}

	mod->cleanup = bfdCleanupCallback;
	mod->text_vma= ldr.text_vma;

	/* store the frame info in the modPvt field */
	mod->modPvt=ehFrame;
	ehFrame=0;

	rval=0;

	/* fall through to release unused resources */
cleanup:
	if (ldr.st)
		free(ldr.st);
	if (ldr.new_commons)
		free(ldr.new_commons);

	if (ehFrame) {
		assert(my__deregister_frame);
		my__deregister_frame(ehFrame);
	}

	if (ldr.cst)
		cexpFreeSymTbl(&ldr.cst);

	if (ldr.abfd)
		bfd_close_all_done(ldr.abfd);

	if (f) {
		fclose(f);
	}
#ifdef __rtems__
	if (filename==tmpfname)
		unlink(tmpfname);
#endif


	for (i=0; i<NUM_SEGS; i++)
		if (ldr.segs[i].chunk) free(ldr.segs[i].chunk);

	return rval;
}

static void
bfdCleanupCallback(CexpModule mod)
{
	/* release the frame info we had stored in the modPvt field */
	if (mod->modPvt) {
		assert(my__deregister_frame);
		my__deregister_frame(mod->modPvt);
	}
}

#ifdef __rtems__
static FILE *
copyFileToTmp(int fd, char *tmpfname)
{
FILE		*rval=0,*infile=0,*outfile=0;
char		buf[BUFSIZ];
int			nread,nwritten;
struct stat	stbuf;

	/* a hack (rtems) to make sure the /tmp dir is present */
	if (stat("/tmp",&stbuf)) {
		mode_t old=umask(0);
		mkdir("/tmp",0777);
		umask(old);
	}

#if 0 /* sigh - there is a bug in RTEMS/IMFS; the memory of a
		 tmpfile() is actually never released, so we work around
		 this...
	   */
	/* open files; the tmpfile will be removed by the system
	 * as soon as it's closed
	 */
	if (!(outfile=tmpfile()) || ferror(outfile)) {
		perror("creating scratch file");
		goto cleanup;
	}
#else
	{
	int	fd;
	if ( (fd=mkstemp(tmpfname)) < 0 ) {
		perror("creating scratch file");
		goto cleanup;
	}
	if ( !(outfile=fdopen(fd,"w+")) ) {
		perror("opening scratch file");
		close(fd);
		unlink(tmpfname);
		goto cleanup;
	}
	}
#endif

	if (!(infile=fdopen(fd,"r")) || ferror(infile)) {
		perror("opening object file");
		goto cleanup;
	}

	/* do the copy */
	do {
		nread=fread(buf,1,sizeof(buf),infile);
		if (nread!=sizeof(buf) && ferror(infile)) {
			perror("reading object file");
			goto cleanup;
		}

		nwritten=fwrite(buf,1,nread,outfile);
		if (nwritten!=nread) {
			if (ferror(outfile))
				perror("writing scratch file");
			else
				fprintf(stderr,"Error writing scratch file");
			goto cleanup;
		}
	} while (!feof(infile));

	fflush(outfile);
	rewind(outfile);

	/* success; remap pointers and fall thru */
	rval=outfile;
	outfile=0;

cleanup:
	if (outfile) {
		fclose(outfile);
		unlink(tmpfname);
	}
	if (infile)
		fclose(infile);
	else
		close(fd);
	return rval;
}
#endif

#if 0
/*
 * Define a weak alias which is not very satisfying and will
 * fail on targets who do not support weak aliases :-(
 */
void (*__nullfn_hack)(void*)=0;

#ifndef __sparc__
void __register_frame(void*) __attribute__ (( weak, alias("__nullfn_hack") ));
void __deregister_frame(void*) __attribute__ (( weak, alias("__nullfn_hack") ));
#endif
#endif
