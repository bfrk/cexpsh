/* $Id$ */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

/* help information for important internal 'cexp' routines */

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

#include "cexpmod.h"
#include "cexpsyms.h"
#include "cexp.h"
#define _INSIDE_CEXP_
#include "cexpHelp.h"

#ifdef HAVE_BFD_DISASSEMBLER
extern int cexpDisassemble();
#endif

static CexpHelpTabRec CEXP_HELP_TAB[]={
	HELP(
"Load an object module (module_name may be NULL), returns ID",
		CexpModule,
		cexpModuleLoad,(char *file_name, char *module_name)
	),

	HELP(
"Unload a module (pass handle, RETURNS: 0 on success)",
		int,
		cexpModuleUnload,(CexpModule moduleHandle)
	),
	HELP(
"Search the system symbol table and the user variables\n\
for a regular expression.  Info about the symbol will be\n\
printed on stdout.",
		int,
		lkup,(char *regexp_pattern)
	),
	HELP(
"Search for an address in the system symbol table and\n\
print a range (howmany) of symbols close to the address\n\
of interest to stdout. Howmany defaults to +-5 if passed 0",
		int,
		lkaddr,(void *addr, int howmany)
	),
	HELP(
"The main interpreter loop, it can be registered with a shell...",
		int,
		cexp_main,(int argc, char **argv)
	),
	HELP(
"Alternative entry point for the main interpreter; more convenient\n\
for calling from cexp itself (e.g. for evaluating scripts)",
		int,
		cexp,(char* cmdline)
	),
	HELP(
"Return a module's name (string owned by module code)",
		char*,
		cexpModuleName,(CexpModule moduleID)
	),
	HELP(
"List the IDs of modules whose name matches a pattern\n\
to file 'f' (stdout if NULL).\n\
RETURNS: First module ID found, NULL on no match.",
		CexpModule,
		cexpModuleFindByName,(char *pattern, FILE *f)
	),
	HELP(
"Dump info about a module to 'f' (stdout if NULL).\n\
If NULL is passed for the module ID, info about all\n\
modules is given.",
		int,
		cexpModuleInfo,(CexpModule mod, FILE *f)
	),
#ifdef HAVE_BFD_DISASSEMBLER
	HELP(
"Disassemble 'n' lines (defaults to 10 if 0) from 'addr'.\n\
If 'addr' is passed NULL, disassembly resumes where the\n\
last call to cexpDisassemble() stopped.\n\
Parameter 'di' should be set to NULL (automatically determined).",
		int,
		cexpDisassemble,(void *addr, int n, disassemble_info *di)
	),
#endif
	HELP("",,0,)
};

void
cexpAddHelpToSymTab(CexpHelpTab h, CexpSymTbl t)
{
CexpSym found;
	for (; h->addr; h++) {
		if ((found=cexpSymTblLkAddr(h->addr,0,0,t))) {
			if (found->value.ptv == h->addr) {
				found->help=h->info.text;
			}
		}	
	}
}