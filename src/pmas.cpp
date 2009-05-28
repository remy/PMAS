/****************************************************************************\
PMAS - Pika Macro ASsembler
by Rafael Vuijk (aka DarkFader)
http://darkfader.net/pm/

TODO:
	more tests
	instruction tree for faster lookups
	function-like macro's
	reduce symbol output
\****************************************************************************/

/*
 * Includes
 */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#ifdef WIN32
#include <io.h>			// for unlink
#else
#include <unistd.h>		// for unlink
#endif
#include "pmas.h"
#include "eval.h"
#include "macrolist.h"
#include "instruction.h"
#include "mem.h"
#include "symbol.h"

/****************************************************************************\
 File
\****************************************************************************/

File *file;

//#define MAX_INCLUDELEVEL	16

/*
struct FileType
{
	FILE *fi;
	int line_num;
	char *filename;

	void Free() { if (fi) { fclose(fi); fi = 0; }; if (filename) { free(filename); filename = 0; } }/
	FileType() { fi=0; line_num=0; filename=0; }
	~ValueType() { Free(); }
};

typedef Stack<FileType> FileStack;
FileStack fileStack;
*/

#define MAX_INCLUDEPATHS		32
char *includepaths[MAX_INCLUDEPATHS];		// no trailing slashes
int includepaths_count = 0;

/****************************************************************************\
 Error
\****************************************************************************/

#ifdef DEBUG
	#define MAX_ERRORS			1
#else
	#define MAX_ERRORS			20
#endif

int errors = 0;				// increased in eprintf

//extern File files[];			// TODO: put into stack
//extern int currentfile;

/*
 * eexit
 */
void eexit()
{
	exit(1);
}

/*
 * eprintf
 * Print error
 */
void eprintf(const char *fmt, ...)
{
	char errormsg[TMPSIZE];
	va_list marker;
	va_start(marker, fmt);
	int r = vsprintf(errormsg, fmt, marker);
	va_end(marker);
	char *p;
	if ((p = strchr(file->origline, '\r'))) *p = 0;
	if ((p = strchr(file->origline, '\n'))) *p = 0;
	while ((p = strchr(file->origline, '\t'))) *p = ' ';
	fprintf(stderr, "%s, line %d: \'%s\' : %s",
		file->filename, file->line_num, file->origline, errormsg
	);
	if (++errors >= MAX_ERRORS)
	{
		fprintf(stderr, "Maximum number of errors reached.\n"); eexit();
	}
}

/****************************************************************************\
 Misc
\****************************************************************************/

int pass;					// pass 1..2
int option_range;			// range checking of immediate values (default = 0)
unsigned int option_base;	// start address of output file (default = 0)
int option_localjump;		// jumps to local labels default to short jumps? (default = 1)
int option_farjump;			// jumps to non-local labels and without suffix default to far jumps? (default = 1)
int option_word;			// *NOT WORKING YET* non-jumps without B or W suffix default to word? (default = 0)
int option_fill;			// byte to fill uninitialized data with (default = 0xFF)

bool condition;				// conditional assembling
bool condition_met;			// conditional already met?
char locallabelprefix[TMPSIZE];		// prefix for macro/local labels
int repeat;					// macro repeat count

/****************************************************************************\
 Directive
\****************************************************************************/

#define PARSE_COND_DIRECTIVES		1
#define PARSE_MACRO_DIRECTIVES		2
#define PARSE_REPT_DIRECTIVES		4
#define PARSE_OTHER_DIRECTIVES		8
#define PARSE_ALL_DIRECTIVES		(PARSE_COND_DIRECTIVES | PARSE_MACRO_DIRECTIVES | PARSE_REPT_DIRECTIVES | PARSE_OTHER_DIRECTIVES)

int option_directive;		// invalid directive error (default = 1)
int parse_directives;		// what to parse


// macro that checks validity and wether it should be handled
#define DIRECTIVE(name, category)	\
	(strword(line, name) && (valid_directive = true, (parse_directives & category)))


/*
 * ParseDirective
 * Note: macro directives are handled before recording
 */
void ParseDirective()	//File *file)
{
	char *line = file->line + 1;
//printf("'%s'\n", file->line);
//printf("'%s'\n", line);
	bool valid_directive = false;
	//bool done_directive = true;
	
	/*
	 * macro ending directives
	 */

	if (DIRECTIVE("endm", PARSE_MACRO_DIRECTIVES))
	{
		current_macro = 0;
		parse_directives = PARSE_ALL_DIRECTIVES;
	}
	else if (DIRECTIVE("endr", PARSE_REPT_DIRECTIVES))
	{
		current_macro = 0;
		parse_directives = PARSE_ALL_DIRECTIVES;
		while (repeat-- > 0) MacroExecute("_rept");
	}

	/*
	 * record to macro
	 */
	if (current_macro)
	{
		MacroLine();		// record with period
		return;				// only process macro ending directives
	}

	/*
	 * macro starting directives
	 */
	if (DIRECTIVE("macro", PARSE_MACRO_DIRECTIVES))
	{
		char *name = strtok(strskipspace(line), delim_chars);
		current_macro = FindMacro(name);
		if (pass == 1)
		{
			if (current_macro) { eprintf("Macro name already defined.\n"); eexit(); }
			current_macro = NewMacro(name);
EEKS{printf("new macro at %p\n", current_macro);}
			char *paramname;
			while ((paramname = strtok(0, delim_chars)))
			{
				if (isspace2(paramname[0])) paramname = strskipspace(paramname);
				if (strchr(endline_chars, *paramname)) break;

				current_macro->AddParameter(paramname);
			}
		}
		parse_directives = PARSE_MACRO_DIRECTIVES;
	}
	else if (DIRECTIVE("rept", PARSE_REPT_DIRECTIVES))
	{
		repeat = EvaluateExpression(strskipspace(line));
		current_macro = NewMacro("_rept");
		parse_directives = PARSE_REPT_DIRECTIVES;
	}

	/*
	 * conditional directives
	 * TODO: make it recursive (stack)
	 */

	else if (DIRECTIVE("if", PARSE_COND_DIRECTIVES))
	{
		condition = EvaluateExpression(strskipspace(line));
		condition_met = condition;
		parse_directives = condition ? PARSE_ALL_DIRECTIVES : PARSE_COND_DIRECTIVES;
	}
	else if (DIRECTIVE("elsif", PARSE_COND_DIRECTIVES))
	{
		if (condition_met)
		{
			condition = false;
		}
		else
		{
			condition = EvaluateExpression(strskipspace(line));
			condition_met |= condition;
		}
		parse_directives = condition ? PARSE_ALL_DIRECTIVES : PARSE_COND_DIRECTIVES;
	}
	else if (DIRECTIVE("else", PARSE_COND_DIRECTIVES))
	{
		condition = !condition_met;
		parse_directives = condition ? PARSE_ALL_DIRECTIVES : PARSE_COND_DIRECTIVES;
	}
	else if (DIRECTIVE("endif", PARSE_COND_DIRECTIVES))
	{
		condition = true;
		parse_directives = condition ? PARSE_ALL_DIRECTIVES : PARSE_COND_DIRECTIVES;
	}

	/*
	 * other directives
	 */

	else if (DIRECTIVE("db", PARSE_OTHER_DIRECTIVES))
	{
EEKS{printf("%d %s\n", parse_directives, line);}
		char *p = strskipspace(line);
		
		while (p)
		{
EEKS{printf("'%s'\n", p);}
			ValueType n = EvaluateExpression(p, &p);

			const char *s = n.getString();
			if (s)
			{
				while (*s)
				{
					if (pass == 2) reloc_data[addr] = *s;
					addr++;
					s++;
				}
			}
			else
			{
				if (pass == 2) reloc_data[addr] = n;
				addr++;
			}
		}
	}
	else if (DIRECTIVE("dw", PARSE_OTHER_DIRECTIVES))
	{
		char *p = strskipspace(line);
		//p = strtok(p, ",");
		while (p)
		{
			long number = EvaluateExpression(p, &p);
			if (pass == 2)
			{
				reloc_data[addr+0] = number;
				reloc_data[addr+1] = number >> 8;
			}
			addr += 2;
			//p = strtok(0, ",");
		}
	}
	else if (DIRECTIVE("dd", PARSE_OTHER_DIRECTIVES))
	{
		char *p = strskipspace(line);
		//p = strtok(p, ",");
		while (p)
		{
			long number = EvaluateExpression(p, &p);
			if (pass == 2)
			{
				reloc_data[addr+0] = number;
				reloc_data[addr+1] = number >> 8;
				reloc_data[addr+2] = number >> 16;
				reloc_data[addr+3] = number >> 24;
			}
			addr += 4;
			//p = strtok(0, ",");
		}
	}
	else if (DIRECTIVE("equ", PARSE_OTHER_DIRECTIVES) || DIRECTIVE("set", PARSE_OTHER_DIRECTIVES))
	{
		char *s = strskipspace(line);
		char *name = strtok(s, delim_chars);
		char *expr = strtok(0, "");
EEKS{printf("equ '%s'='%s'\n", name, expr);}
		SetSymbolExpression(name, expr);
EEKS{printf("verify "); GetSymbolValue(name).print();}
	}
	else if (DIRECTIVE("option", PARSE_OTHER_DIRECTIVES))
	{
		char *s = strskipspace(line);
		char *name = strtok(s, delim_chars);
		int value = EvaluateExpression(strtok(0, ""));
		if (!strcmp(name, "directive")) option_directive = value;
		else if (!strcmp(name, "range")) option_range = value;
		else if (!strcmp(name, "base")) option_base = value;
		else if (!strcmp(name, "farjump")) option_farjump = value;
		else if (!strcmp(name, "localjump")) option_localjump = value;
		else if (!strcmp(name, "word")) option_word = value;
		else if (!strcmp(name, "fill")) option_fill = value;
	}
	else if (DIRECTIVE("unset", PARSE_OTHER_DIRECTIVES))
	{
		char *s = strskipspace(line);
		char *name = strtok(s, delim_chars);
		UnsetSymbol(name);
	}
	else if (DIRECTIVE("instruction", PARSE_OTHER_DIRECTIVES))
	{
		char *p = strskipspace(line);
		Instruction *instruction = new Instruction;
		instruction->fmt = strdup(EvaluateExpression(p, &p).getString());
		instruction->flags = EvaluateExpression(p, &p);
		instruction->fixed = EvaluateExpression(p, &p);
		instruction->size = EvaluateExpression(p, &p);
		instruction->argnum = EvaluateExpression(p, &p);
		for (int i=0; i<instruction->argnum; i++)
		{
			instruction->argInfo[i].shift = EvaluateExpression(p, &p);
			instruction->argInfo[i].flags = EvaluateExpression(p, &p);
		}
		AddInstruction(instruction);
	}
	else if (DIRECTIVE("org", PARSE_OTHER_DIRECTIVES) || DIRECTIVE("orgfill", PARSE_OTHER_DIRECTIVES))
	{
		char *s = strskipspace(line);
		addr = EvaluateExpression(s);
	}
	else if (DIRECTIVE("reloc", PARSE_OTHER_DIRECTIVES))
	{
		char *s = strskipspace(line);
		if (!*s || (s == strpbrk(s, endline_chars)))		// default = 0
			reloc_offset = addr;
		else
			reloc_offset = addr - EvaluateExpression(s);
		addr = addr - reloc_offset;
	}
	else if (DIRECTIVE("endreloc", PARSE_OTHER_DIRECTIVES))
	{
		addr += reloc_offset;
		reloc_offset = 0;
	}
	else if (DIRECTIVE("align", PARSE_OTHER_DIRECTIVES))
	{
		int value = EvaluateExpression(strskipspace(line));
		int check = value; while (~check & 1) check >>= 1;
		if (check != 1) { eprintf("Value must be power of 2.\n"); eexit(); }
		addr = (addr + (value-1)) / value * value;
	}
	else if (DIRECTIVE("ds", PARSE_OTHER_DIRECTIVES))
	{
		char *next;
		int size = EvaluateExpression(strskipspace(line), &next);
		int fill = next ? (int)EvaluateExpression(next) : -1;

		if ((pass == 2) && (fill >= 0))
		{
			while (size--) reloc_data[addr++] = fill;
		}
		else
		{
			addr += size;
		}
	}
	else if (DIRECTIVE("parse", PARSE_OTHER_DIRECTIVES))
	{
		ValueType n = EvaluateExpression(strskipspace(line));
		const char *s = n.getString();
		if (!s) { eprintf("String expression expected.\n"); return; }
EEKS{printf("'%s' '%s'\n", strskipspace(line), s);}
		char tmp[TMPSIZE];
		strcpy(tmp, s);
		file->line = tmp;
		ParseLine();
	}
	else if (DIRECTIVE("include", PARSE_OTHER_DIRECTIVES))
	{
		char name[TMPSIZE], *p = strskipspace(line);
		ParseString(name, p);
		ParseFile(name);
	}
	else if (DIRECTIVE("incbin", PARSE_OTHER_DIRECTIVES))
	{
		char name[TMPSIZE], *p = strskipspace(line);
		ParseString(name, p);
		FILE *fb = fopen(name, "rb");

		if (!fb) { eprintf("Cannot open binary file '%s'.\n", name); eexit(); }
		if (pass == 1)
		{
			fseek(fb, 0, SEEK_END);
			addr += ftell(fb);
		}
		else	// if (pass == 2)
		{
			while (1)
			{
				int c = fgetc(fb);
				if (c < 0) break;
				reloc_data[addr++] = c;
			}
		}
		fclose(fb);
	}
	else if (DIRECTIVE("printf", PARSE_OTHER_DIRECTIVES))
	{
//printf("printf %d %d\n", parse_directives, PARSE_OTHER_DIRECTIVES);
		if (pass == 2)
		{
			char *inp = strskipspace(line);
			ValueType n = EvaluateExpression(inp, &inp);
			char *fmtp = (char *)n.getString();
			while (fmtp && *fmtp)
			{
				// find a formatted parameter
				char *p = strchr(fmtp, '%');
				if (p)
				{
					if (*(p+1) == '%')		// two %'s
					{
						*(p+1) = 0;
						p += 2;
					}
					else
					{
						// find end of parameter
						while (*p && !((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z'))) p++;
						if (*p)
						{
							// temporarily null-terminal and print formatted output
							p++;
							char backup = *p;
							*p = 0;
							ValueType n = EvaluateExpression(inp, &inp);
							const char *s = n.getString();
							if (s)
								printf(fmtp, s);
							else
								printf(fmtp, (int)n);
							*p = backup;
							fmtp = p;
							continue;		// ok, next
						}
					}
				}
				printf("%s", fmtp);		// not formatted
				fmtp = p;
			}
		}
	}
	else if (DIRECTIVE("exit", PARSE_OTHER_DIRECTIVES))
	{
		if (pass == 2)
		{
			eprintf("Exiting.\n");
			eexit();
		}
	}
	else if (DIRECTIVE("end", PARSE_OTHER_DIRECTIVES))
	{
		fseek(file->fi, 0, SEEK_END);
	}
	
	/*
	 * check valid directive
	 */
	if (!valid_directive && option_directive)		// test for unknown directive
	{
		eprintf("Unknown directive.\n");
	}
}

/****************************************************************************\
 Label
\****************************************************************************/

/*
 * ParseLabel
 */
void ParseLabel()	//File *file)
{
	char *line = file->line;

	if (!condition)
	{
	}
	else if (current_macro)
	{
		MacroLine();
	}
	else
	{
		char *p = strchr(line, ':');
		if (!p) { eprintf("Label does not end with a colon (':').\n"); return; }
		*p = 0;
		if (*line == '_')	// local label
		{
			if (pass == 1)
			{
				char tmp[TMPSIZE];
				strcpy(tmp, locallabelprefix);
				strcat(tmp, line);
				if (FindSymbol(tmp)) { eprintf("Symbol already defined.\n"); return; }
				SetSymbolValue(tmp, ValueType(addr));
			}
		}
		else	// normal label
		{
			strcpy(locallabelprefix, line);
			if (pass == 1)
			{
				if (FindSymbol(line)) { eprintf("Symbol already defined.\n"); return; }
EEKS{printf("label %06X\n", addr);}
				SetSymbolValue(line, ValueType(addr));
			}
		}
		if (p) { file->line = p+1; ParseLine(); }	// parse after colon
	}
}

/****************************************************************************\
 Parse
\****************************************************************************/

/*
 * ParseInstruction
 */
void ParseInstruction()		//File *file)
{
	if (!condition)
	{
		return;
	}
	else if (current_macro)
	{
		file->line--;	// record with leading space
		MacroLine();
		return;
	}
	else
	{
		if (TryInstructions(file->line)) return;	// try instructions
		if (MacroExecute(file->line)) return;		// try macro's otherwise

		if (!instructions)		// try default cpu
		{
			ParseFile("cpu/pm.s");
			if (TryInstructions(file->line)) return;
		}

		eprintf("Unknown instruction or invalid arguments.\n");
	}
}

/*
 * MacroLine
 */
void MacroLine()	//File *file)
{
	char *line = file->line;

	if (pass == 1)
	{
EEKS{printf("record %s\n", line);}
		current_macro->AddLine(line);
	}
}

/*
 * ParseLine
 */
void ParseLine()	//File *file)
{
EEKS{printf("parseline %s\n", file->line);}

	// C PreProcessor filename + linenumber fix
	if (file->line[0] == '#')
	{
		int new_line_num = strtoul(file->line+2,0,10);
		if (new_line_num > 0)
		{
			file->line_num = new_line_num - 1;
			FREE(file->filename);		// wrong filename
			char *p, *s = strskipspace(file->line+2);
			if ((p = strchr(s,'\r'))) *p = 0;
			if ((p = strchr(s,'\n'))) *p = 0;
			file->filename = strdup(s);
			return;
		}
	}

	// remove leading space
	bool leadingspace = false;
	while (isspace2(*(file->line))) { leadingspace=true; file->line++; }
	strcpy(file->origline, file->line);

	if (isendline(file->line[0]))		// comment line
	{
	}
	else if (file->line[0] == '.')	// directive
	{
		ParseDirective();
	}
	else if (leadingspace)		// instruction
	{
		ParseInstruction();
	}
	else	// label
	{
		ParseLabel();
	}
	
	UpdateMaxAddr();
}

/*
 * ParseFile
 */
void ParseFile(const char *filename)
{
	for (int i=0; i<includepaths_count; i++)
	{
		char fullpath[TMPSIZE];
		strcpy(fullpath, includepaths[i]);
		//strcat(fullpath, "/");
		strcat(fullpath, filename);
		
		FILE *fi = strcmp(filename, "-") ? fopen(fullpath, "rt") : stdin;

		//printf("%s\n", fullpath);
		
		if (fi)
		{
			File _file;
			File *calling_file = file;
			file = &_file;

			//if (currentfile == MAX_INCLUDELEVEL - 1) { eprintf("Maximum include level reached.\n"); eexit(); }
			//currentfile++;
			file->fi = fi;
			file->filename = strdup(filename);
			file->line_num = 0;
//printf("%s\n", file->filename);
		
			while (1)
			{
				char line[TMPSIZE];
				if (!fgets(line, TMPSIZE-1, file->fi)) break;
				file->line = line;
				file->line_num++;
				ParseLine();
			}
		
			FREE(file->filename);
			fclose(file->fi);
			//currentfile--;

			file = calling_file;
			return;
		}
	}
	fprintf(stderr, "Cannot open include file '%s'.\n", filename); eexit();
}

/****************************************************************************\
 stdin
\****************************************************************************/

FILE *stdinfile = 0;		// copy of stdin

/*
 * RemoveStdinTmp
 */
void RemoveStdinTmp()
{
	unlink("stdin.tmp");
}

/****************************************************************************\
 main
\****************************************************************************/

/*
 * main
 */
int main(int argc, char *argv[])
{
	// show title when not stdout
	if ((argc < 3) || strcmp(argv[2], "-"))
	{
		printf("Pika Macro ASsembler v" VERSION " (build " __DATE__ ") by Rafael Vuijk. http://darkfader.net/pm/\n");
	}

	// show help
	if (argc < 3)
	{
		printf("Syntax: pmas input.s output.min symbols.sym\n");
		printf("Syntax: pmas input.s [-o output.min] [-s symbols.sym]\n");
		printf("\n");
		printf("Use '-' for filenames to use stdin/stdout.\n");
		eexit();
	}

	// parse parameters
	char *inputfile = 0;
	char *outputfile = 0;
	char *symbolfile = 0;
	bool newstyleparams = false;
	bool oldstyleparams = false;
	for (int a=1; a<argc; a++)
	{
		if ((argv[a][0] == '-') && (argv[a][1] != 0))
		{
			switch (argv[a][1])
			{
				case 'o':
					newstyleparams = true;
					if (outputfile) { eprintf("Output file already specified.\n"); eexit(); }
					outputfile = argv[++a];
					break;
				
				case 's':
					newstyleparams = true;
					if (symbolfile) { eprintf("Symbol file already specified.\n"); eexit(); }
					symbolfile = argv[++a];
					break;
				
				default:
					break;
			}
		}
		else
		{
			oldstyleparams = true;
			if (!inputfile) inputfile = argv[a];
			else if (!outputfile) outputfile = argv[a];
			else if (!symbolfile) symbolfile = argv[a];
		}
	}
	
	// check conflicting parameter style
	if (newstyleparams && oldstyleparams) { eprintf("Cannot use old and new style parameters together.\n"); eexit(); }

	// stdin
	if (inputfile && !strcmp(inputfile, "-"))
	{
		stdinfile = fopen("stdin.tmp", "wt");
		if (!stdinfile) { fprintf(stderr, "Cannot create temporary file.\n"); eexit(); }
		char c; while ((c = fgetc(stdin)) >= 0) fputc(c, stdinfile);
		fclose(stdinfile);
		inputfile = "stdin.tmp";
		atexit(RemoveStdinTmp);
	}

	/*
	 * Include paths
	 */

	// add default directory to include path
	includepaths[includepaths_count++] = strdup("");
	
	// add PMAS executable directory to include path
	char tmp[TMPSIZE];
	strcpy(tmp, argv[0]);
	char *p = strrchr(tmp, '/');
	if (!p) p = strrchr(tmp, '\\');
	if (p) *(p+1) = 0;
	//strcpy(p, "/src");
	includepaths[includepaths_count++] = strdup(tmp);

	/*
	 * parse
	 */

	for (pass=1; pass<=2; pass++)
	{
		addr = 0;
		reloc_offset = 0;
		//currentfile = -1;
		option_directive = 1;
		option_range = 0;
		option_base = 0;
		option_localjump = 1;
		option_farjump = 1;
		option_word = 0;
		option_fill = 0xFF;
		current_macro = 0;
		parse_directives = PARSE_ALL_DIRECTIVES;
		macro_id = 0;
		repeat = 0;
		condition = true;
		strcpy(locallabelprefix, "");
		
		FreeInstructions();

		ParseFile(inputfile);

		if (errors > 0)
		{
			fprintf(stderr, "%d error(s) found.\n", errors);
			eexit();
		}
	}

	/*
	 * write output
	 */

	if (outputfile)
	{
		WriteOutput(outputfile);
	}
	
	/*
	 * write symbols
	 */

	if (argc >= 4) WriteSymbols(argv[3]);

	return 0;
}
