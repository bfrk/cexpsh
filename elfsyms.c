/* $Id$ */

/* routines to extract information from an ELF symbol table */

/* The ELF symbol table is reduced and copied to a local format.
 * (a lot of ELF info / symbols are not used by us and we want
 * to have two _sorted_ tables with name and address keys.
 * We don't bother to set up a hash table because we want
 * to implement regexp search and hence we want a sorted table
 * anyway...).
 */

/* Author/Copyright: Till Straumann <Till.Straumann@TU-Berlin.de>
 * License: GPL, see http://www.gnu.org for the exact terms
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include <libelf/libelf.h>
#include <regexp.h>

#include "elfsyms.h"
/* NOTE: DONT EDIT 'cexp.tab.h'; it is automatically generated by 'bison' */
#include "cexp.tab.h"

/* filter the symbol table entries we're interested in */

/* NOTE: this routine defines the CexpType which is assigned
 *       to an object read from the symbol table. Currently,
 *       this only based on object size with integers
 *       taking preference over floats.
 *       All of this knowledge / heuristics is concentrated
 *       in one place, namely HERE.
 */
static int
filter(Elf32_Sym *sp,CexpType *pt)
{
	switch (ELF32_ST_TYPE(sp->st_info)) {
	case STT_OBJECT:
		if (pt) {
			CexpType t;
			int s=sp->st_size;
			/* determine the type of variable */

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
				t=TVoid;
			}
			*pt=t;
		}
	break;

	case STT_FUNC:
		if (pt) *pt=TFuncP;
	break;

	case STT_NOTYPE:
		if (pt) *pt=TVoidP;
	break;

	default:
	return 0;
	}

	return 1;
}

/* compare the name of two symbols */
static int
namecomp(const void *a, const void *b)
{
	CexpSym sa=(CexpSym)a;
	CexpSym sb=(CexpSym)b;
	return strcmp(sa->name, sb->name);
}

/* compare the 'values' of two symbols, i.e. the addresses
 * they represent.
 */
static int
addrcomp(const void *a, const void *b)
{
	CexpSym *sa=(CexpSym*)a;
	CexpSym *sb=(CexpSym*)b;
	return (*sa)->value.ptv-(*sb)->value.ptv;
}

/* our implementation of the symbol table holds more information
 * that we make public
 */
typedef struct PrivSymTblRec_ {
	/* NOTE: the stab field MUST be first, so we can cast pointers around */
	CexpSymTblRec	stab;	/* symbol table, sorted in ascending order (key=name) */
	char		*strtbl;	/* string table */
	CexpSym		*aindex;	/* an index sorted to ascending addresses */
} PrivSymTblRec, *PrivSymTbl;

/* THE global system symbol table. We might have to think about
 * this again, once RTEMS has a dynamic loader...
 */
CexpSymTbl cexpSysSymTbl=0;

CexpSym
cexpSymTblLookup(char *name, CexpSymTbl t)
{
CexpSymRec key;
	if (!t) t=cexpSysSymTbl;
	key.name = name;
	return (CexpSym)bsearch((void*)&key,
				t->syms,
				t->nentries,
				sizeof(*t->syms),
				namecomp);
}

/* a semi-public routine which takes a precompiled regexp.
 * The reason this is not public is that we try to keep
 * the public from having to deal with/know about the regexp
 * implementation, i.e. which variant, which headers etc.
 */
CexpSym
_cexpSymTblLookupRegex(regexp *rc, int max, CexpSym s, FILE *f, CexpSymTbl t)
{
CexpSym	found=0;

	if (max<1)	max=25;
	if (!f)		f=stdout;
	if (!t)		t=cexpSysSymTbl;
	if (!s)		s=t->syms;

	while (s->name && max) {
		if (regexec(rc,s->name)) {
			cexpSymPrintInfo(s,f);
			max--;
			found=s;
		}
		s++;
	}

	return s->name ? found : 0;
}

#define USE_ELF_MEMORY

#define CHUNK    2000

#ifdef HAVE_RCMD
#include <sys/time.h>
#include <errno.h>
/* avoid pulling in networking headers under __rtems
 * until BSP stuff is separated out from the core
 */
#if !defined(RTEMS_TODO_DONE) && defined(__rtems)
#define	AF_INET	2
extern char *inet_ntop();
extern int	socket();
extern int  select();
#else
#include <sys/socket.h>
#include <arpa/inet.h>
#endif

#define RSH_PORT 514

static char *
handleInput(int fd, int errfd, unsigned long *psize)
{
long	n=0,size,avail;
fd_set	r,w,e;
char	errbuf[1000],*ptr,*buf;
struct  timeval timeout;

register long ntot=0,got;

	if (n<fd)		n=fd;
	if (n<errfd)	n=errfd;

	n++;

	buf=ptr=0;
	size=avail=0;

	while (fd>=0 || errfd>=0) {
		FD_ZERO(&r);
		FD_ZERO(&w);
		FD_ZERO(&e);

		timeout.tv_sec=5;
		timeout.tv_usec=0;
		if (fd>=0) 		FD_SET(fd,&r);
		if (errfd>=0)	FD_SET(errfd,&r);
		if ((got=select(n,&r,&w,&e,&timeout))<=0) {
				if (got) {
					fprintf(stderr,"rsh select() error: %s.\n",
							strerror(errno));
				} else {
					fprintf(stderr,"rsh timeout\n");
				}
				goto cleanup;
		}
		if (errfd>=0 && FD_ISSET(errfd,&r)) {
				got=read(errfd,errbuf,sizeof(errbuf));
				if (got<0) {
					fprintf(stderr,"rsh error (reading stderr): %s.\n",
							strerror(errno));
					goto cleanup;
				}
				if (got)
					write(2,errbuf,got);
				else {
					errfd=-1; 
				}
		}
		if (fd>=0 && FD_ISSET(fd,&r)) {
				if (avail < CHUNK) {
					size+=CHUNK; avail+=CHUNK;
					if (!(buf=realloc(buf,size))) {
						fprintf(stderr,"out of memory\n");
						goto cleanup;
					}
					ptr = buf + (size-avail);
				}
				got=read(fd,ptr,avail);
				if (got<0) {
					fprintf(stderr,"rsh error (reading stdout): %s.\n",
							strerror(errno));
					goto cleanup;
				}
				if (got) {
					ptr+=got;
					ntot+=got;
					avail-=got;
				} else {
					fd=-1;
				}
		}
	}
	if (psize) *psize=ntot;
	return buf;
cleanup:
	free(buf);
	return 0;
}

char *
rshLoad(char *host, char *user, char *cmd)
{
char	*chpt=host,*buf=0;
int		fd,errfd;
long	ntot;
extern  int rcmd();

	fd=rcmd(&chpt,RSH_PORT,user,user,cmd,&errfd);
	if (fd<0) {
		fprintf(stderr,"rcmd: got no remote stdout descriptor\n");
		goto cleanup;
	}
	if (errfd<0) {
		fprintf(stderr,"rcmd: got no remote stderr descriptor\n");
		goto cleanup;
	}

	if (!(buf=handleInput(fd,errfd,&ntot))) {
		goto cleanup; /* error message has already been printed */
	}

	fprintf(stderr,"0x%lx (%li) bytes read\n",ntot,ntot);

cleanup:
	if (fd>=0)		close(fd);
	if (errfd>=0)	close(errfd);
	return buf;
}
#endif

CexpSym
cexpSymTblLookupRegex(char *re, int max, CexpSym s, FILE *f, CexpSymTbl t)
{
CexpSym	found;
regexp	*rc;

	if (!(rc=regcomp(re))) {
		fprintf(stderr,"unable to compile regexp '%s'\n",re);
		return 0;
	}
	found=_cexpSymTblLookupRegex(rc,max,s,f,t);

	if (rc) free(rc);
	return found;
}

/* read an ELF file, extract the relevant information and
 * build our internal version of the symbol table.
 * All libelf resources are released upon return from this
 * routine.
 */
CexpSymTbl
cexpSlurpElf(char *filename)
{
Elf			*elf=0;
Elf32_Ehdr	*ehdr;
Elf_Scn		*scn;
Elf32_Shdr	*shdr=0;
Elf_Data	*strs;
PrivSymTbl	rval=0;
CexpSym		sane;
#ifdef USE_ELF_MEMORY
char		*buf=0,*ptr=0;
long		size=0,avail=0,got;
#ifdef		__rtems
extern		struct in_addr rtems_bsdnet_bootp_server_address;
char		HOST[30];
#else
#define		HOST "localhost"
#endif
#endif
int			fd=-1;

	elf_version(EV_CURRENT);

#ifdef USE_ELF_MEMORY
#ifdef HAVE_RCMD
	if ('~'==filename[0]) {
		char *cmd=malloc(strlen(filename)+40);
		strcpy(cmd,filename);
		ptr=strchr(cmd,'/');
		if (!ptr) {
			fprintf(stderr,"Illegal filename for rshLoad %s\n",filename);
			free(cmd);
			goto cleanup;
		}
		*(ptr++)=0;
		memmove(ptr+4,ptr,strlen(ptr));
		memcpy(ptr,"cat ",4);
#ifdef __rtems
		inet_ntop(AF_INET, &rtems_bsdnet_bootp_server_address, HOST, sizeof(HOST));
#endif
		/* try to load via rsh */
		if (!(buf=rshLoad(HOST,cmd+1,ptr)))
			goto cleanup;
	}
	else
#endif
	{
		if ((fd=open(filename,O_RDONLY,0))<0)
			goto cleanup;

		do {
			if (avail<CHUNK) {
				size+=CHUNK; avail+=CHUNK;
				if (!(buf=realloc(buf,size)))
					goto cleanup;
				ptr=buf+(size-avail);
			}
			got=read(fd,ptr,avail);
			if (got<0)
				goto cleanup;
			avail-=got;
			ptr+=got;
		} while (got);
			got = ptr-buf;
	}
	if (!(elf=elf_memory(buf,got)))
		goto cleanup;
#else
	if ((fd=open(filename,O_RDONLY,0))<0)
		goto cleanup;

	if (!(elf=elf_begin(fd,ELF_C_READ,0)))
		goto cleanup;
#endif

	/* we need the section header string table */
	if (!(ehdr=elf32_getehdr(elf)) ||
	    !(scn =elf_getscn(elf,ehdr->e_shstrndx)) ||
	    !(strs=elf_getdata(scn,0)))
		goto cleanup;
	
	for (scn=0; (scn=elf_nextscn(elf,scn)) && (shdr=elf32_getshdr(scn));) {
		if (!(strcmp((char*)strs->d_buf + shdr->sh_name,".symtab")))
			break;
	}
	if (!shdr)
		goto cleanup;

	/* get the string table */
	if ((rval=(PrivSymTbl)malloc(sizeof(*rval)))) {
		long		n,nsyms,nDstSyms,nDstChars;
		char		*strtab,*src,*dst;
		Elf32_Sym	*syms, *sp;
		CexpSym		cesp;

		memset((void*)rval, 0, sizeof(*rval));

		strtab=(char*)elf_getdata(elf_getscn(elf,shdr->sh_link),0)->d_buf;
		syms=(Elf32_Sym*)elf_getdata(scn,0)->d_buf;

		nsyms=shdr->sh_size/shdr->sh_entsize;

		/* count the number of valid symbols */
		for (sp=syms,n=0,nDstSyms=0,nDstChars=0; n<nsyms; sp++,n++) {
			if (filter(sp,0)) {
				nDstChars+=(strlen(strtab+sp->st_name) + 1);
				nDstSyms++;
			}
		}

		rval->stab.nentries=nDstSyms;

		/* create our copy of the symbol table - ELF contains
		 * many things we're not interested in and also, it's not
		 * sorted...
		 */
		
		/* allocate all the table space */
		if (!(rval->stab.syms=(CexpSym)malloc(sizeof(CexpSymRec)*(nDstSyms+1))))
			goto cleanup;


		if (!(rval->strtbl=(char*)malloc(nDstChars)) ||
                    !(rval->aindex=(CexpSym*)malloc(nDstSyms*sizeof(*rval->aindex))))
			goto cleanup;

		/* now copy the relevant stuff */
		for (sp=syms,n=0,cesp=rval->stab.syms,dst=rval->strtbl; n<nsyms; sp++,n++) {
			CexpType t;
			if (filter(sp,&t)) {
				/* copy the name to the string table and put a pointer
				 * into the symbol table.
				 */
				cesp->name=dst;
				src=strtab+sp->st_name;
				while ((*(dst++)=*(src++)))
						/* do nothing else */;

				cesp->size = sp->st_size;

				cesp->value.type = t;
				cesp->value.ptv  = (CexpVal)sp->st_value;
				rval->aindex[cesp-rval->stab.syms]=cesp;
				
				cesp++;
			}
		}
		/* mark the last table entry */
		cesp->name=0;
		/* sort the tables */
		qsort((void*)rval->stab.syms,
			rval->stab.nentries,
			sizeof(*rval->stab.syms),
			namecomp);
		qsort((void*)rval->aindex,
			rval->stab.nentries,
			sizeof(*rval->aindex),
			addrcomp);
	} else {
		goto cleanup;
	}
	

	elf_cntl(elf,ELF_C_FDDONE);
	elf_end(elf); elf=0;
#ifdef USE_ELF_MEMORY
	if (buf) {
		free(buf); buf=0;
	}
#endif
	if (fd>=0) close(fd);

#ifndef ELFSYMS_TEST_MAIN
	/* do a couple of sanity checks */
	if ((sane=cexpSymTblLookup("cexpSlurpElf",&rval->stab))) {
		extern void *_edata, *_etext;
		/* it must be the main symbol table */
		if (sane->value.ptv!=(CexpVal)cexpSlurpElf)
			goto bailout;
		if (!(sane=cexpSymTblLookup("_etext",&rval->stab)) || sane->value.ptv!=(CexpVal)&_etext)
			goto bailout;
		if (!(sane=cexpSymTblLookup("_edata",&rval->stab)) || sane->value.ptv!=(CexpVal)&_edata)
			goto bailout;
		/* OK, sanity test passed */
	}
#endif


	return &rval->stab;

bailout:
	fprintf(stderr,"ELFSYMS SANITY CHECK FAILED: you possibly loaded the wrong symbol table\n");

cleanup:
	if (elf_errno())
		fprintf(stderr,"ELF error: %s\n",elf_errmsg(elf_errno()));
	if (elf)
		elf_end(elf);
#ifdef USE_ELF_MEMORY
	if (buf)
		free(buf);
#endif
	if (rval)
		cexpFreeSymTbl((CexpSymTbl*)&rval);
	if (fd>=0)
		close(fd);
	if (elf_errno()) fprintf(stderr,"ELF error: %s\n",elf_errmsg(elf_errno()));
	return 0;
}

CexpSymTbl
cexpCreateSymTbl(char *symFileName)
{
CexpSymTbl	t=0;
	if (symFileName) {
		t=cexpSlurpElf(symFileName);
		if (!t) {
			fprintf(stderr,"Unable to read symbol table file '%s'\n",symFileName);
			fprintf(stderr,"Trying to use existing sysSymTbl\n");
		} else if (!cexpSysSymTbl) {
			cexpSysSymTbl=t;
		}
	}
	if (!t && !(t=cexpSysSymTbl)) {
			fprintf(stderr,"ERROR: unable to find a valid symbol table\n");
	}
	return t;
}


void
cexpFreeSymTbl(CexpSymTbl *pt)
{
PrivSymTbl st=(PrivSymTbl)*pt;
	if (st && (st!=(void*)cexpSysSymTbl || pt==&cexpSysSymTbl)) {
		free(st->stab.syms);
		free(st->strtbl);
		free(st->aindex);
		free(st);
	}
	*pt=0;
}

int
cexpSymPrintInfo(CexpSym s, FILE *f)
{
CexpType t=s->value.type;
	if (!f) f=stdout;

	/* convert variables (which are internally treated as pointers)
	 * to their base type for display
	 */
	if (!CEXP_TYPE_FUNQ(t))
		t=CEXP_TYPE_PTR2BASE(t);	
	return
		fprintf(f,"%p[%4d]: %s %s\n",
			s->value.ptv,
			s->size,
			cexpTypeInfoString(t),
			s->name);
}


/* do a binary search for an address */
CexpSym
cexpSymTblLkAddr(void *addr, int margin, FILE *f, CexpSymTbl t)
{
PrivSymTbl	pt;
int			lo,hi,mid;
	if (!f) f=stdout;
	if (!t) t=cexpSysSymTbl;

	pt=(PrivSymTbl)t;
	lo=0; hi=t->nentries-1;
	while (lo < hi) {
		mid=(lo+hi)>>1;
		if (addr > (void*)pt->aindex[mid]->value.ptv)
			lo=mid+1;
		else
			hi=mid;
	}
	mid=lo;		if (mid>=t->nentries)	mid=t->nentries-1;
	lo-=margin; if (lo<0) 			 	lo=0;
	hi+=margin; if (hi>=t->nentries)	hi=t->nentries-1;
	while (lo<=hi)
		cexpSymPrintInfo(pt->aindex[lo++],f);
	return pt->aindex[mid];
}


#ifdef ELFSYMS_TEST_MAIN
/* only build this 'main' if we are testing the ELF subsystem */

int
elfsyms_main(int argc, char **argv)
{
int		fd,nsyms;
CexpSymTbl	t;
CexpSym		symp;

	if (argc<2) {
		fprintf(stderr,"Need a file name\n");
		return 1;
	}

	t=cexpSlurpElf(argv[1]);

	if (!t) {
		return 1;
	}

	fprintf(stderr,"%i symbols found\n",t->nentries);
	symp=t->syms;
	for (nsyms=0; nsyms<t->nentries;  nsyms++) {
		symp=((PrivSymTbl)t)->aindex[nsyms];
		fprintf(stderr,
			"%02i 0x%08xx (%2i) %s\n",
			symp->value.type,
			symp->value.tv.p,
			symp->size,
			symp->name);	
		symp++;
	}
	cexpFreeSymTbl(&t);
	return 0;
}

#endif
